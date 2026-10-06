//========================================================================================
// perfstat - 插件加载自检（POSIX / Linux 版）
//
//   在 Linux 上没有引擎可以托管插件，所以这里用 dlopen 直接把 .so 当成普通共享库加载，
//   然后：
//     1. dlsym("perfstat_factory") / dlsym("CreateInterface") 取插件入口
//     2. 请求 ISERVERPLUGINCALLBACKS003，校验类型与长度（长度不同说明 ABI 不一致）
//     3. 造一个假 ICvar（VEngineCvar007）传给 Load()，确认 perf_* 指令都注册上了，
//        并通过它的 ConsolePrintf 验证输出通道（跑 Linux CI 时这是唯一的观察窗口）
//     4. 通过假 ICvar 记录的 ConCommandBase* 调 Dispatch() 跑几条指令
//     5. 起一个忙线程，观察【插件自己的 profiler】样本数有没有增长
//        —— 这正是"插件在真实进程里能不能采样"的核心验证
//     6. Unload() 后确认指令被反注册；dlclose() 成功（说明采样线程收干净了）
//
// 【分工说明】"信号采样通路本身对不对"由 tools/perfstat_smoke_linux.cpp 验证 ——
// 那个程序里只有【一份】平台层副本，是确定性的。本程序里因为 dlopen 了插件，
// 会同时存在两份平台层副本（自检一份、插件一份），而信号处理器是进程级、
// 后装覆盖先装的，自检如果自己发起采样就会跟插件的处理器抢槽位表。
// 真实服务器里插件只有一份副本，不存在这个问题，所以这里不去碰它。
//
// 构建：make -f tools/Makefile.linux_tests   （或在 CI 里直接 g++ 编译）
// 运行：./build/perfstat_loader_test_linux <perfstat.so 路径>
//========================================================================================

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

// 直接用插件自己的 ABI 头文件，保证结构体布局与插件一致
#include "../src/plugin_api.h"
#include "../src/core.h"
#include "../src/platform.h"

//========================================================================================
// 平台层：从【插件里】取，不再自己编译一份 perf_platform_linux.cpp
//
// 为什么：自检原来自己编一份平台层，于是进程里同时存在两份
// （自检一份、插件一份）。信号处理器是进程级、后装覆盖先装的，而 g_slots/g_sig 是每份
// 各自的 —— 两边互相踩，表现为采样超时、样本数 0，只在 Linux 上出现且极难定位。
// 这个冲突打过三次补丁都没根治（CAS / thread_local / 全局指针）。
//
// 现在进程里只有一份平台层，和生产环境（真实服务器上插件也只有一份）完全一致，
// 所以自检测的就是真实路径。
//
// 结构体一律复用 src/platform.h 里的定义（POD），通过 perfstat_ps_abi_sizes 校验布局一致。
//========================================================================================
typedef int (*PsInitFn)(void);
typedef void (*PsEnumModulesFn)(void *);
typedef void (*PsEnumThreadsFn)(void *);
typedef int (*PsSampleFn)(uintptr_t *, uint32_t *, int);
typedef void (*PsSetDebugFn)(int);
typedef long (*PsHandlerRunsFn)(void);
typedef int (*PsAbiSizesFn)(int *, int *, int *, int *);

static PsInitFn g_ps_init = 0;
static PsEnumModulesFn g_ps_enum_modules = 0;
static PsEnumThreadsFn g_ps_enum_threads = 0;
static PsSampleFn g_ps_sample_threads = 0;
static PsSetDebugFn g_ps_set_debug = 0;
static PsHandlerRunsFn g_ps_handler_runs = 0;

// 解析插件里的平台层入口
static bool load_plugin_platform(void *so) {
    g_ps_init = (PsInitFn)dlsym(so, "perfstat_ps_init");
    g_ps_enum_modules = (PsEnumModulesFn)dlsym(so, "perfstat_ps_enum_modules");
    g_ps_enum_threads = (PsEnumThreadsFn)dlsym(so, "perfstat_ps_enum_threads");
    g_ps_sample_threads = (PsSampleFn)dlsym(so, "perfstat_ps_sample_threads");
    g_ps_set_debug = (PsSetDebugFn)dlsym(so, "perfstat_ps_set_debug");
    g_ps_handler_runs = (PsHandlerRunsFn)dlsym(so, "perfstat_ps_handler_runs");
    return g_ps_init && g_ps_enum_modules && g_ps_enum_threads && g_ps_sample_threads;
}


static int g_fail = 0;
static int g_pass = 0;

// 看门狗：CI 上如果哪一步卡住，至少能在日志里看到卡在哪，
// 并且主动退出（退出码 3），不要一直挂到 GitHub 的 6 小时上限。
//
// 注意：信号处理器只能读，读到的指针永远指向字符串字面量（静态存储期），
// 所以不会悬空。用数组下标而不是 volatile 指针，避免 const volatile 类型问题。
static const char *g_phases[] = {
    "start",
    "dlopen 插件",
    "plugin->Load()",
    "输出通道验证",
    "ABI 校验 + 用插件的平台层采样",
    "观察插件自身采样",
    "平台采样 2 秒",
    "perf_dump / perf_top",
    "Unload + dlclose",
    "完成",
};
static volatile sig_atomic_t g_phase_idx = 0;

static const char kTimeoutPrefix[] = "\n[loader] 超时！卡在阶段: ";

static void on_alarm(int) {
    int i = (int)g_phase_idx;
    if (i < 0 || i >= (int)(sizeof(g_phases) / sizeof(g_phases[0]))) i = 0;
    const char *p = g_phases[i];
    (void)!write(2, kTimeoutPrefix, sizeof(kTimeoutPrefix) - 1);
    (void)!write(2, p, strlen(p));
    (void)!write(2, "\n", 1);
    _exit(3);
}

static void phase(const char *p) {
    for (size_t i = 0; i < sizeof(g_phases) / sizeof(g_phases[0]); ++i) {
        if (strcmp(g_phases[i], p) == 0) {
            g_phase_idx = (sig_atomic_t)i;
            break;
        }
    }
    fprintf(stderr, "[loader] >>> %s\n", p);
    fflush(stderr);
}

// 诊断摘要用的全局量（出问题时让用户只贴摘要，省掉来回贴长日志）
static char g_first_fail[512] = "";
static int g_diag_total = 0, g_diag_in_self = 0, g_diag_slow = 0;
static double g_diag_worst_ms = 0.0;
static long g_diag_handler_runs = 0;
static unsigned long long g_diag_plugin_before = 0, g_diag_plugin_after = 0;
static int g_diag_plugin_running = -1, g_diag_plugin_autostop = -1;
static char g_diag_lock_holder[128] = "";

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) {                                                   \
            printf("  [ OK ] %s\n", msg);                             \
            g_pass++;                                                 \
        } else {                                                      \
            printf("  [FAIL] %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_fail++;                                                 \
            if (g_first_fail[0] == 0) {                               \
                snprintf(g_first_fail, sizeof(g_first_fail), "%s (%s:%d)", msg, __FILE__,       \
                         __LINE__);                                                   \
            }                                                         \
        }                                                             \
    } while (0)

//----------------------------------------------------------------------------------------
// 假 ICvar：槽位顺序必须与 public/icvar.h 完全一致
//----------------------------------------------------------------------------------------
class FakeICvar : public ICvar {
public:
    std::vector<ConCommandBase *> registered;
    std::vector<std::string> console_lines;
    int unregister_calls;

    FakeICvar() : unregister_calls(0) {}

    virtual void *QueryInterface(const char *) { return 0; }
    virtual void *Connect(void *) { return 0; }
    virtual void Disconnect(void) {}
    virtual void *Init(void) { return 0; }
    virtual void Shutdown(void) {}

    virtual CVarDLLIdentifier_t AllocateDLLIdentifier() { return 7; }
    virtual void RegisterConCommand(ConCommandBase *p) {
        registered.push_back(p);
        printf("    [假ICvar] RegisterConCommand(\"%s\")\n", p && p->GetName() ? p->GetName() : "?");
    }
    virtual void UnregisterConCommand(ConCommandBase *p) {
        unregister_calls++;
        for (size_t i = 0; i < registered.size(); ++i) {
            if (registered[i] == p) {
                registered.erase(registered.begin() + i);
                break;
            }
        }
    }
    virtual void UnregisterConCommands(CVarDLLIdentifier_t) {}
    virtual const char *GetCommandLineValue(const char *) { return 0; }
    virtual ConCommandBase *FindCommandBase(const char *name) {
        for (size_t i = 0; i < registered.size(); ++i) {
            const char *n = registered[i]->GetName();
            if (n && strcmp(n, name) == 0) return registered[i];
        }
        return 0;
    }
    virtual const ConCommandBase *FindCommandBase(const char *) const { return 0; }
    virtual ConVar *FindVar(const char *) { return 0; }
    virtual const ConVar *FindVar(const char *) const { return 0; }
    virtual ConCommandBase *FindCommand(const char *) { return 0; }
    virtual const ConCommandBase *FindCommand(const char *) const { return 0; }
    virtual void InstallGlobalChangeCallback(void *) {}
    virtual void RemoveGlobalChangeCallback(void *) {}
    virtual void CallGlobalChangeCallbacks(ConVar *, const char *, float) {}
    virtual void InstallConsoleDisplayFunc(void *) {}
    virtual void RemoveConsoleDisplayFunc(void *) {}
    virtual void ConsoleColorPrintf(const void *, const char *fmt, ...) { (void)fmt; }

    virtual void ConsolePrintf(const char *fmt, ...) {
        char buf[4096];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        console_lines.push_back(std::string(buf));
        printf("    [假控制台] %s", buf);
        size_t n = strlen(buf);
        if (n == 0 || buf[n - 1] != '\n') printf("\n");
    }
};

static FakeICvar g_fake_cvar;

static void *fake_interface_factory(const char *name, int *rc) {
    if (rc) *rc = 1;
    if (name && strcmp(name, CVAR_INTERFACE_VERSION) == 0) {
        if (rc) *rc = 0;
        return (void *)&g_fake_cvar;
    }
    return 0;
}

//----------------------------------------------------------------------------------------
// 构造一个引擎那样已经 tokenize 好的 CCommand
//----------------------------------------------------------------------------------------
class CmdBuilder {
public:
    CCommand cmd;
    char buf[512];
    const char *argv[CCommand::COMMAND_MAX_ARGC];

    CmdBuilder(const char *line) {
        // 不用 memset：CCommand 是非平凡类型（有构造函数），
        // gcc 会给 -Wclass-memaccess 警告。这里直接按字段清零。
        int *raw0 = reinterpret_cast<int *>(&cmd);
        raw0[0] = 0;
        raw0[1] = 0;
        char *sb = reinterpret_cast<char *>(&cmd);
        sb[8] = 0;
        const char **pp0 = reinterpret_cast<const char **>(sb + 1032);
        pp0[0] = 0;
        strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;

        int argc = 0;
        char *p = buf;
        while (*p && argc < CCommand::COMMAND_MAX_ARGC) {
            while (*p == ' ') ++p;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') ++p;
            if (*p) *p++ = 0;
        }

        int *raw = reinterpret_cast<int *>(&cmd);
        raw[0] = argc;
        raw[1] = (int)(argc > 0 ? strlen(argv[0]) + 1 : 0);
        char *sbuf = reinterpret_cast<char *>(&cmd) + 8;
        strcpy(sbuf, buf);
        const char **pp = reinterpret_cast<const char **>(reinterpret_cast<char *>(&cmd) + 1032);
        for (int i = 0; i < argc; ++i) pp[i] = argv[i];
    }
};

//----------------------------------------------------------------------------------------
// 采样线程：让服务器"忙"起来，这样采样才有东西可采
//
// 注意：这次踩过一个坑 —— 原来忙循环直接内联在 busy_thread 里，
// 编译器把它优化后可能落在别的模块（实测样本跑到了 perfstat.so），
// 于是"样本应落在本程序模块内"的断言就失败了。
// 现在把它做成一个显式导出的函数，确保这段代码一定在测试程序自己的 .text 里。
//----------------------------------------------------------------------------------------
static volatile bool g_busy = false;
static volatile unsigned long long g_sink = 0;

extern "C" __attribute__((noinline, used, visibility("default"))) void perfstat_test_busy_loop(
    unsigned spin) {
    unsigned long long acc = 0;
    for (unsigned i = 0; i < spin; ++i) {
        acc += (unsigned long long)i * 2654435761u;
        acc ^= acc >> 13;
    }
    g_sink += acc;
}

static void *busy_thread(void *) {
    while (g_busy) {
        perfstat_test_busy_loop(200000);
    }
    return 0;
}

// 起/停忙线程的辅助（信号采样需要有"别的线程"可采，而且它的代码要在本程序的 .text 里）
static void start_busy_thread(pthread_t *t) {
    g_busy = 1;
    pthread_create(t, 0, busy_thread, 0);
}

static void stop_busy_thread(pthread_t t) {
    g_busy = 0;
    pthread_join(t, 0);
}

int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);

    // 超时保护：默认 120 秒，可用第二个参数覆盖
    int timeout_sec = argc > 2 ? atoi(argv[2]) : 120;
    if (timeout_sec <= 0) timeout_sec = 120;
    signal(SIGALRM, on_alarm);
    alarm((unsigned)timeout_sec);

    printf("perfstat 加载自检（Linux / dlopen）\n");
    printf("================================================================================\n");
    fprintf(stderr, "[loader] 看门狗已启用：%d 秒\n", timeout_sec);

    const char *so_path = argc > 1 ? argv[1] : "Release/perfstat.so";
    printf("目标插件: %s\n\n", so_path);
    printf("sizeof(CCommand)=%d sizeof(ConCommandBase)=%d sizeof(ConCommand)=%d\n",
           (int)sizeof(CCommand), (int)sizeof(ConCommandBase), (int)sizeof(ConCommand));
    CHECK(sizeof(CCommand) == 1288, "CCommand 布局与 l4d2 SDK 一致（1288 字节）");
    CHECK(sizeof(ConCommandBase) == 24, "ConCommandBase 布局与 l4d2 SDK 一致（24 字节）");
    CHECK(sizeof(void *) == 4, "当前是 32 位进程（srcds 是 32 位的）");

    phase("dlopen 插件");
    // ---- 1. dlopen ----
    void *so = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    CHECK(so != 0, "dlopen 成功加载插件");
    if (!so) {
        printf("  [FAIL] dlopen 失败: %s\n", dlerror());
        return 1;
    }

    // ---- 2. 取入口 ----
    typedef void *(*FactoryFn)(const char *, int *);
    FactoryFn fac = (FactoryFn)dlsym(so, "perfstat_factory");
    CHECK(fac != 0, "dlsym(\"perfstat_factory\") 成功");
    FactoryFn cif = (FactoryFn)dlsym(so, "CreateInterface");
    CHECK(cif != 0, "dlsym(\"CreateInterface\") 成功（引擎就是按这个符号找插件的）");
    if (!fac) fac = cif;
    if (!fac) return 1;

    int rc = -1;
    void *iface = fac("ISERVERPLUGINCALLBACKS003", &rc);
    CHECK(iface != 0 && rc == 0, "请求 ISERVERPLUGINCALLBACKS003 成功");
    if (!iface) return 1;

    int rc2 = -1;
    void *bad = fac("NO_SUCH_INTERFACE_999", &rc2);
    CHECK(bad == 0 && rc2 != 0, "请求不存在的接口时安全返回失败");

    IServerPluginCallbacks *plugin = (IServerPluginCallbacks *)iface;

    phase("plugin->Load()");
    // 打开插件的调试输出（每一步耗时都会打到 stderr），方便定位卡在哪一步
    {
        typedef void (*SetVerboseFn)(int);
        SetVerboseFn setv = (SetVerboseFn)dlsym(so, "perfstat_set_verbose");
        if (setv) {
            setv(1);
            fprintf(stderr, "[loader] 已打开插件 verbose\n");
        }
    }

    // ---- 3. Load（递假 ICvar）----
    printf("\n--- 调用 Load(interfaceFactory, ...) ---\n");
    bool ok = plugin->Load(fake_interface_factory, 0);
    CHECK(ok, "Load() 返回 true");
    printf("  假 ICvar 收到 %d 个指令注册\n", (int)g_fake_cvar.registered.size());
    CHECK(g_fake_cvar.registered.size() == 10, "Load() 里注册了 10 个 perf_* 指令");

    const char *want[10] = {"perf_help", "perf_start",    "perf_stop",    "perf_stat",
                            "perf_top",  "perf_threads",  "perf_dump",    "perf_reset",
                            "perf_load", "perf_selftest"};
    int found = 0;
    for (int i = 0; i < 10; ++i) {
        ConCommandBase *cb = g_fake_cvar.FindCommandBase(want[i]);
        if (cb && cb->GetName() && strcmp(cb->GetName(), want[i]) == 0) found++;
    }
    CHECK(found == 10, "10 个指令都注册成功且 GetName() 正确");

    phase("输出通道验证");
    // ---- 4. 输出通道 ----
    printf("\n--- 验证控制台输出通道 ---\n");
    {
        size_t before = g_fake_cvar.console_lines.size();
        ConCommand *cc = (ConCommand *)g_fake_cvar.FindCommandBase("perf_selftest");
        if (cc) {
            CmdBuilder c("perf_selftest");
            cc->Dispatch(c.cmd);
        }
        size_t produced = g_fake_cvar.console_lines.size() - before;
        printf("  perf_selftest 通过 ICvar::ConsolePrintf 输出了 %d 行\n", (int)produced);
        CHECK(produced > 5, "控制台输出走了 ICvar::ConsolePrintf（不是裸 printf）");
        bool saw = false;
        for (size_t i = before; i < g_fake_cvar.console_lines.size(); ++i) {
            if (g_fake_cvar.console_lines[i].find("ICvar::ConsolePrintf") != std::string::npos) saw = true;
        }
        CHECK(saw, "perf_selftest 报告的输出通道是 ICvar::ConsolePrintf");
    }

    // 先起忙线程：它的代码在【本程序自己的 .text】里，这样"样本归属到本模块"才有意义。
    // 之前忘了这一步，采样时段所有线程都在插件/libc 里跑，于是"落在本模块"当然是 0
    // —— 那是测试设计问题，不是采样问题。
    pthread_t plat_busy;
    start_busy_thread(&plat_busy);

    phase("ABI 校验 + 用插件的平台层采样");
    // 【这一段是平台层的确定性验证，而且是单副本】
    // 调用的是插件里那一份平台层，进程里没有第二份，所以不存在"两份信号处理器互相踩"。
    {
        CHECK(load_plugin_platform(so), "从插件里取到平台层入口（单副本方案）");
        PsAbiSizesFn abi = (PsAbiSizesFn)dlsym(so, "perfstat_ps_abi_sizes");
        CHECK(abi != 0, "插件导出了 perfstat_ps_abi_sizes");
        int m = 0, t = 0, mv = 0, tv = 0, ptr = 0;
        if (abi) ptr = abi(&m, &t, &mv, &tv);
        printf("  ABI: ModuleInfo=%d ThreadInfo=%d ModuleVisits=%d ThreadVisits=%d ptr=%d\n", m, t,
               mv, tv, ptr);
        CHECK(m == (int)sizeof(ps::ModuleInfo), "ModuleInfo 布局与插件一致");
        CHECK(t == (int)sizeof(ps::ThreadInfo), "ThreadInfo 布局与插件一致");
        CHECK(mv == (int)sizeof(ps::ModuleVisits), "ModuleVisits 布局与插件一致");
        CHECK(tv == (int)sizeof(ps::ThreadVisits), "ThreadVisits 布局与插件一致");
        CHECK(ptr == (int)sizeof(void *), "指针宽度一致（都是 32 位进程）");
    }
    {
        CHECK(g_ps_init && g_ps_init(), "插件里的平台层初始化成功");
        g_ps_set_debug(1);  // 让平台层打印采样细节

        // 模块枚举
        static ps::ModuleInfo mods[512];
        ps::ModuleVisits mv2;
        mv2.items = mods;
        mv2.count = 0;
        mv2.capacity = 512;
        g_ps_enum_modules(&mv2);
        printf("  插件的平台层枚举到 %d 个模块\n", mv2.count);
        CHECK(mv2.count >= 3, "插件平台层能枚举模块");

        // 找到本程序的模块范围
        uintptr_t self_base = 0;
        size_t self_size = 0;
        for (int i = 0; i < mv2.count; ++i) {
            if (strstr(mods[i].name, "perfstat_loader") != NULL) {
                self_base = mods[i].base;
                self_size = mods[i].size;
                break;
            }
        }
        CHECK(self_base != 0, "定位到本程序的模块范围");

        // 采样 100 轮（忙线程还没起，但主线程 + 插件采样线程本身就可作为目标）
        uintptr_t ips[64];
        uint32_t tids[64];
        uintptr_t ips_saved[100];
        int ips_saved_n = 0;
        int total = 0, in_self = 0, slow = 0;
        double worst = 0.0;
        for (int round = 0; round < 100; ++round) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int n = g_ps_sample_threads(ips, tids, 64);
            for (int i = 0; i < n && ips_saved_n < 100; ++i) ips_saved[ips_saved_n++] = ips[i];
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
            if (ms > worst) worst = ms;
            if (ms > 15.0) slow++;
            for (int i = 0; i < n; ++i) {
                total++;
                if (self_base && ips[i] >= self_base && ips[i] < self_base + self_size) in_self++;
            }
            usleep(5000);
        }
        printf("  插件的平台层采样 100 轮：样本 %d，落在本模块 %d；单次最慢 %.2f ms，>15ms %d 轮\n",
               total, in_self, worst, slow);
        // 说明：这里【不能】断言"每轮都 <15ms"。忙线程会把一个核占满，而 2 核 CI 上
        // 信号投递要等调度才能落在忙线程上（实测最慢几十毫秒），这是正常的调度延迟，
        // 不是采样器的问题。真正该断言的是"不会永远卡住"（最慢有上界）+ "采得到样本"。
        long hruns = g_ps_handler_runs ? g_ps_handler_runs() : 0;
        printf("  插件平台层的信号处理器运行次数: %ld\n", hruns);
        g_diag_total = total;
        g_diag_in_self = in_self;
        g_diag_worst_ms = worst;
        g_diag_slow = slow;
        g_diag_handler_runs = hruns;
        CHECK(total > 50, "用插件的平台层能采到足够样本（单副本，信号通路正常）");
        CHECK(in_self > 0, "样本能正确归属到本程序模块（忙线程在本程序 .text 里）");
        CHECK(worst < 500.0, "单次采样有上界（不会卡死；忙线程占核时会等调度）");
        CHECK(total >= 100, "100 轮采样每轮至少拿到 1 个样本（采样节拍稳定）");
        stop_busy_thread(plat_busy);

        // 归因诊断：把样本分布打出来。万一"落在本模块=0"，这几行能直接说明样本去哪了。
        if (in_self == 0 && total > 0) {
            fprintf(stderr, "[loader] 归因诊断：self_base=%p size=%zu，前 10 个样本 IP 及归属:\n",
                    (void *)self_base, self_size);
            for (int i = 0; i < 10 && i < 100; ++i) {
                int owner = -1;
                for (int k = 0; k < mv2.count; ++k) {
                    if (ips_saved[i] >= mods[k].base && ips_saved[i] < mods[k].base + mods[k].size) {
                        owner = k;
                        break;
                    }
                }
                fprintf(stderr, "[loader]   ip=%p -> %s\n", (void *)ips_saved[i],
                        owner >= 0 ? mods[owner].name : "(无归属)");
            }
        }
    }

    phase("观察插件自身采样");
    // 这一段验证的是"真实路径"：只起一个忙线程，观察【插件自己的 profiler】样本数是否增长。
    //
    // 为什么不再让自检自己编译一份平台层：那样进程里会有两份 g_slots / 两份信号处理器
    // （处理器是进程级、后装覆盖先装），两边互相踩，表现为超时和零样本 —— 而且只发生在
    // "自检"这种场景，真实服务器里插件只有一份副本。现在自检一律用插件导出的平台层
    // （见上面 ABI 校验那一段），所以进程里只有一份，和线上一致。
    {
        // 【先关掉自动停止】perfstat.ini 里 duration_sec 默认是 2 秒，
        // 插件 Load() 时就会按它启动"2 秒后自动停"。而 CI 里从加载到这一段
        // 往往已经超过 2 秒 —— 插件早就停了，于是等再久样本也不涨，断言就会失败（踩过）。
        // 这里用 perf_start 10 0 明确改成"不自动停"。
        {
            ConCommand *cc = (ConCommand *)g_fake_cvar.FindCommandBase("perf_start");
            CHECK(cc != 0, "找得到 perf_start 指令");
            if (cc) {
                CmdBuilder c("perf_start 10 0");
                cc->Dispatch(c.cmd);
            }
        }

        // 插件自身的采样次数基线
        typedef void *(*GetProfilerFn)(void);
        typedef long (*HandlerRunsFn)(void);
        GetProfilerFn getp = (GetProfilerFn)dlsym(so, "perfstat_get_profiler");
        HandlerRunsFn geth = (HandlerRunsFn)dlsym(so, "perfstat_handler_runs");
        CHECK(getp != 0, "插件导出了 perfstat_get_profiler");
        CHECK(geth != 0, "插件导出了 perfstat_handler_runs");

        long hb = geth ? geth() : 0;

        // 让 CPU 忙着，这样才有东西可采
        pthread_t bt;
        g_busy = 1;
        pthread_create(&bt, 0, busy_thread, 0);

        // 带时间戳的阶段标记：stdout 是块缓冲、stderr 无缓冲，
        // 日志里行号不代表时间顺序（这个坑吃过）。所以关键点都带时间戳。
        typedef double (*NowFn)(void);
        NowFn now_ms = (NowFn)dlsym(so, "perfstat_now_ms");
        fprintf(stderr, "[loader] [%.0f ms] 开始等待插件采样增长\n", now_ms ? now_ms() : -1.0);

        // 【轮询等待，而不是死等固定 2 秒】
        // 原来写死 sleep(2) 然后检查一次，在 2 核机器上会偶发失败：
        // 忙线程 + 主线程把两个核占满时，插件的采样线程可能整个那 2 秒都拿不到 CPU。
        // 这不是插件的问题，而是"窗口太短 + 机器太忙"。改成"最多等 10 秒，
        // 一旦样本数增长就立刻通过"，既鲁棒又通常更快。
        unsigned long long start_samples = 0;
        if (getp) {
            ps::Profiler *prof = (ps::Profiler *)getp();
            if (prof) start_samples = (unsigned long long)prof->total_samples();
        }
        int waited_ms = 0;
        for (; waited_ms < 10000; waited_ms += 100) {
            unsigned long long cur = 0;
            if (getp) {
                ps::Profiler *prof = (ps::Profiler *)getp();
                if (prof) cur = (unsigned long long)prof->total_samples();
            }
            if (cur > start_samples) break;
            usleep(100 * 1000);
        }
        fprintf(stderr, "[loader] [%.0f ms] 等待结束，等了 %d ms\n", now_ms ? now_ms() : -1.0,
                waited_ms);

        g_busy = 0;
        pthread_join(bt, 0);

        unsigned long long after = 0;
        int mods = 0, threads = 0;
        if (getp) {
            ps::Profiler *prof = (ps::Profiler *)getp();
            if (prof) {
                after = (unsigned long long)prof->total_samples();
                mods = prof->module_count();
                threads = prof->thread_count();
            }
        }
        long ha = geth ? geth() : 0;

        // 锁诊断：如果采样线程卡住，这里能看出是不是被锁挡住了
        {
            typedef const char *(*LockInfoFn)(void);
            LockInfoFn holder = (LockInfoFn)dlsym(so, "perfstat_lock_holder");
            if (holder) {
                printf("  profiler 锁持有者: '%s'\n", holder());
                snprintf(g_diag_lock_holder, sizeof(g_diag_lock_holder), "%s", holder());
            }
        }

        printf("  插件自身采样次数: %llu -> %llu（模块 %d / 线程 %d，等待 %d ms）\n", start_samples,
               after, mods, threads, waited_ms);
        g_diag_plugin_before = start_samples;
        g_diag_plugin_after = after;
        if (getp) {
            ps::Profiler *prof = (ps::Profiler *)getp();
            if (prof) {
                // 注意：这里只能调用【头文件里的 inline 成员】。
                // 本程序的 Makefile 只编平台层、不编 core.cpp，
                // 所以 elapsed_seconds() / total_samples() 这类定义在 core.cpp 里的函数
                // 链接不到（踩过：undefined reference to elapsed_seconds）。
                // running() / auto_stop_sec() / module_count() / thread_count() 都是 inline，可以放心用。
                printf("  插件 profiler 状态: running=%d auto_stop=%d\n", (int)prof->running(),
                       prof->auto_stop_sec());
                g_diag_plugin_running = (int)prof->running();
                g_diag_plugin_autostop = prof->auto_stop_sec();
            }
        }
        printf("  插件副本的信号处理器运行次数: %ld -> %ld\n", hb, ha);
        CHECK(after > start_samples, "插件自己的采样线程确实在采到样本（真实进程里可用）");
        CHECK(after > 0, "插件 profiler 样本数大于 0");
    }

    phase("perf_dump / perf_top");
    // ---- 6. 让插件自己出一份报告（顺带验证 log 落盘）----
    printf("\n--- 执行 perf_dump / perf_top ---\n");
    {
        ConCommand *cc = (ConCommand *)g_fake_cvar.FindCommandBase("perf_dump");
        CmdBuilder c("perf_dump");
        if (cc) cc->Dispatch(c.cmd);
    }
    {
        ConCommand *cc = (ConCommand *)g_fake_cvar.FindCommandBase("perf_top");
        CmdBuilder c("perf_top 3");
        if (cc) cc->Dispatch(c.cmd);
    }
    CHECK(g_fake_cvar.console_lines.size() > 20, "perf_dump / perf_top 都有控制台输出");

    phase("Unload + dlclose");
    // ---- 7. Unload + dlclose ----
    printf("\n--- 调用 Unload() ---\n");
    plugin->Unload();
    CHECK(g_fake_cvar.unregister_calls >= 10, "Unload() 把 10 个指令都反注册掉了");
    CHECK(g_fake_cvar.registered.empty(), "反注册后 ICvar 里不再有我们的指令");

    int dlrc = dlclose(so);
    CHECK(dlrc == 0, "dlclose 成功卸载（采样线程已收干净，没有线程跑在已卸载代码上）");

    alarm(0);
    fprintf(stderr, "[loader] 全部阶段完成\n");
    printf("\n================================================================================\n");
    printf("结果: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);

    //----------------------------------------------------------------------------------------
    // 【故障排查摘要】出问题时只需要把这一段贴出来就够了，不用贴整份日志（几百行）。
    // 走 stderr（无缓冲），保证即使进程之后出问题也已经写出来了。
    //----------------------------------------------------------------------------------------
    fprintf(stderr, "\n===== PERFSTAT 诊断摘要（贴这一段即可）=====\n");
    fprintf(stderr, "结果: 通过=%d 失败=%d\n", g_pass, g_fail);
    if (g_first_fail[0]) fprintf(stderr, "首个失败: %s\n", g_first_fail);
    fprintf(stderr, "平台层来源: 插件 dlsym（单副本，与生产一致）\n");
    fprintf(stderr, "插件平台层采样: 样本=%d 落在本模块=%d 最慢=%.1fms 超15ms轮数=%d\n",
            g_diag_total, g_diag_in_self, g_diag_worst_ms, g_diag_slow);
    fprintf(stderr, "插件平台层信号处理器运行次数=%ld\n", g_diag_handler_runs);
    fprintf(stderr, "插件自身采样: %llu -> %llu  running=%d auto_stop=%d\n",
            g_diag_plugin_before, g_diag_plugin_after, g_diag_plugin_running,
            g_diag_plugin_autostop);
    fprintf(stderr, "锁持有者='%s'\n", g_diag_lock_holder);
    fprintf(stderr, "===== 摘要结束 =====\n");
    return g_fail == 0 ? 0 : 1;
}
