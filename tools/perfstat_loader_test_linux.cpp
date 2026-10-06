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
//     5. 直接驱动平台采样层（ps_sample_threads）与本机线程，验证真的能采到样本
//     6. Unload() 后确认指令被反注册；dlclose() 成功（说明采样线程收干净了）
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
static volatile const char *g_phase = "start";

static void on_alarm(int) {
    const char *p = g_phase;
    (void)!write(2, "\n[loader] 超时！卡在阶段: ", 32);
    (void)!write(2, p, strlen(p));
    (void)!write(2, "\n", 1);
    _exit(3);
}

static void phase(const char *p) {
    g_phase = p;
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
        memset(&cmd, 0, sizeof(cmd));
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
//----------------------------------------------------------------------------------------
static volatile bool g_busy = false;
static volatile unsigned long long g_sink = 0;

static void *busy_thread(void *) {
    while (g_busy) {
        unsigned long long acc = 0;
        for (unsigned i = 0; i < 1000000; ++i) acc += i * 2654435761u;
        g_sink += acc;
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

    phase("平台采样 2 秒");
    // ---- 5. 真实采样：直接驱动平台采样层 ----
    printf("\n--- 真实采样验证（2 秒）---\n");
    pthread_t bt;
    g_busy = true;
    pthread_create(&bt, 0, busy_thread, 0);

    static uintptr_t ips[256];
    static uint32_t tids[256];
    int total = 0, self_hits = 0;
    uintptr_t self_base = 0;
    size_t self_size = 0;
    {
        // 拿本测试程序自己的模块基址，作为"应命中的模块"
        ps::ModuleVisits mv;
        static ps::ModuleInfo mods[512];
        mv.items = mods;
        mv.count = 0;
        mv.capacity = 512;
        ps::ps_enum_modules(&mv);
        printf("  枚举到 %d 个模块\n", mv.count);
        CHECK(mv.count > 3, "模块枚举正常（本程序 + libc + ld ...）");
        char self[1024];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = 0;
            const char *bn = strrchr(self, '/');
            bn = bn ? bn + 1 : self;
            for (int i = 0; i < mv.count; ++i) {
                if (strcmp(mods[i].name, bn) == 0) {
                    self_base = mods[i].base;
                    self_size = mods[i].size;
                    break;
                }
            }
        }
        CHECK(self_base != 0, "找到本测试程序的模块基址（用于归因交叉验证）");

        for (int round = 0; round < 100; ++round) {
            int n2 = ps::ps_sample_threads(ips, tids, 256);
            if (round % 25 == 0) {
                fprintf(stderr, "[loader]   采样轮 %d，累计 %d\n", round, total);
                fflush(stderr);
            }
            for (int i = 0; i < n2; ++i) {
                total++;
                if (self_base && ips[i] >= self_base && ips[i] < self_base + self_size) self_hits++;
            }
            usleep(10000);
        }
    }
    g_busy = false;
    pthread_join(bt, 0);
    printf("  采到 %d 个样本，其中落在本程序模块内 %d 个\n", total, self_hits);
    CHECK(total > 50, "平台采样层能采到样本（信号采样通路正常）");
    CHECK(self_hits > 0, "样本能正确归属到模块（busy 线程在本程序 .text 里）");

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
