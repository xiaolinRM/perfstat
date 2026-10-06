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
// 运行：./build/perfstat_loader_test_linux <perfstat_srv.so 路径>
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
    "观察插件自身采样（2 秒）",
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

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) {                                                   \
            printf("  [ OK ] %s\n", msg);                             \
            g_pass++;                                                 \
        } else {                                                      \
            printf("  [FAIL] %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_fail++;                                                 \
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
// 编译器把它优化后可能落在别的模块（实测样本跑到了 perfstat_srv.so），
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

    const char *so_path = argc > 1 ? argv[1] : "Release/perfstat_srv.so";
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

    phase("观察插件自身采样（2 秒）");
    // 【重要设计决定】这一段【不再】由自检自己调用 ps::ps_sample_threads。
    //
    // 原因：自检和插件各编译了一份 perf_platform_linux.cpp（自检的 Makefile 只编平台层），
    // 于是进程里有两份 g_slots / 两份信号处理器，而信号处理器是进程级、后装覆盖先装的。
    // 自检自己发起采样时，实际跑的是插件副本的处理器、填的是插件副本的槽位表，
    // 自检却在自己的表里等 —— 必然超时。这个冲突只存在于"测试程序"这种场景，
    // 真实服务器里插件只有一份副本，不存在这个问题。
    //
    // 所以这里改为：只起一个忙线程，然后观察【插件自己的 profiler】样本数有没有增长。
    // 这正好是我们要验证的东西 —— 插件在真实进程里能不能正常采样。
    // 至于"信号采样通路本身对不对"，由 tools/perfstat_smoke_linux.cpp 负责
    // （那个程序里只有一份平台副本，是确定性的验证）。
    {
        // 【先关掉自动停止】perfstat.ini 里 duration_sec 默认是 2 秒，
        // 插件 Load() 时就会按它启动"2 秒后自动停"。而 CI 里从加载到这一段
        // 往往已经超过 2 秒 —— 插件早就停了，于是后面 sleep(2) 期间一个样本都不涨，
        // 断言就会失败（踩过）。这里用 perf_start 10 0 明确改成"不自动停"。
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

        unsigned long long before = 0;
        if (getp) {
            ps::Profiler *prof = (ps::Profiler *)getp();
            if (prof) before = (unsigned long long)prof->total_samples();
        }
        long hb = geth ? geth() : 0;

        // 让 CPU 忙着，这样才有东西可采
        pthread_t bt;
        g_busy = 1;
        pthread_create(&bt, 0, busy_thread, 0);

        sleep(2);

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

        printf("  插件自身采样次数: %llu -> %llu（模块 %d / 线程 %d）\n", before, after, mods,
               threads);
        if (getp) {
            ps::Profiler *prof = (ps::Profiler *)getp();
            if (prof) {
                printf("  插件 profiler 状态: running=%d auto_stop=%d elapsed=%.1fs\n",
                       (int)prof->running(), prof->auto_stop_sec(), prof->elapsed_seconds());
            }
        }
        printf("  插件副本的信号处理器运行次数: %ld -> %ld\n", hb, ha);
        CHECK(after > before, "插件自己的采样线程确实在采到样本（真实进程里可用）");
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
    return g_fail == 0 ? 0 : 1;
}
