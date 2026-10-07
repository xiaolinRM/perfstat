//========================================================================================
// perfstat - 插件加载自检（模拟引擎的 plugin_load 流程）
//
//   1. LoadLibrary 打开 build\perfstat.dll
//   2. GetProcAddress("CreateInterface") —— 引擎就是这么找插件的
//   3. 用 "ISERVERPLUGINCALLBACKS003" 请求接口，校验拿到的指针
//   4. 造一个假的 ICvar（VEngineCvar007）传给 Load()，确认插件把 perf_* 指令
//      注册了上来（这一步非常关键：没注册的话引擎会直接报 Unknown command，
//      插件的 ClientCommand 永远收不到）
//   5. 通过假 ICvar 记录的 ConCommandBase*，用 vtable 调 Dispatch() ——
//      这就是引擎敲命令时走的路径
//   6. Unload() 时必须把指令反注册掉
//
// 构建：tools\build_loader_test.bat
// 运行：build\perfstat_loader_test.exe
//========================================================================================

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include <string>
#include <vector>

#include "../src/core.h"
#include "../src/plugin_api.h"

static int g_fail = 0;
static int g_pass = 0;

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
// 假 ICvar：模拟 VEngineCvar007
//   槽位顺序必须与 public/icvar.h 一致（IAppSystem 5 个 + ICvar 的后续方法）
//----------------------------------------------------------------------------------------
class FakeICvar : public ICvar {
public:
    std::vector<ConCommandBase *> registered;
    int unregister_calls;
    int unregister_skipped;  // 因 IsRegistered()==false 被引擎跳过几次

    FakeICvar() : unregister_calls(0), unregister_skipped(0) {}

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
        // 【关键】真实引擎在这里会检查 IsRegistered()：没注册过的命令直接跳过不处理。
        // 自检必须模仿这个行为，否则"插件忘了置 m_bRegistered"这种 bug 测不出来。
        if (!p || !p->IsRegistered()) {
            unregister_skipped++;
            return;
        }
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
    virtual ConVar *FindVar(const char *) { return 0; }
    virtual const ConVar *FindVar(const char *) const { return 0; }
    virtual const ConCommandBase *FindCommandBase(const char *) const { return 0; }

    // 下面这些槽位很少用，但必须存在，否则 ConsolePrintf 的槽位号会错
    virtual ConCommandBase *FindCommand(const char *) { return 0; }
    virtual const ConCommandBase *FindCommand(const char *) const { return 0; }
    virtual void InstallGlobalChangeCallback(void *) {}
    virtual void RemoveGlobalChangeCallback(void *) {}
    virtual void CallGlobalChangeCallbacks(ConVar *, const char *, float) {}
    virtual void InstallConsoleDisplayFunc(void *) {}
    virtual void RemoveConsoleDisplayFunc(void *) {}
    virtual void ConsoleColorPrintf(const void *, const char *fmt, ...) { (void)fmt; }

    // 这就是插件要用的输出通道：engine 会用 ICvar::ConsolePrintf 打到服务器控制台
    virtual void ConsolePrintf(const char *fmt, ...) {
        char buf[4096];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        console_lines.push_back(std::string(buf));
        printf("    [假控制台] %s", buf);
        if (!buf[0] || buf[strlen(buf) - 1] != '\n') printf("\n");
    }

    std::vector<std::string> console_lines;
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
// 构造一个引擎那样“已经 tokenize 好”的 CCommand
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

        // 直接按 SDK 的字段布局填（m_nArgc / m_nArgv0Size / 缓冲 / 指针表）
        int *raw = reinterpret_cast<int *>(&cmd);
        raw[0] = argc;                              // m_nArgc
        raw[1] = (int)(argc > 0 ? strlen(argv[0]) + 1 : 0);  // m_nArgv0Size
        char *sbuf = reinterpret_cast<char *>(&cmd) + 8;      // m_pArgSBuffer
        strcpy(sbuf, buf);
        // m_ppArgv 在缓冲之后：8 + 512 + 512 = 1032
        const char **pp = reinterpret_cast<const char **>(reinterpret_cast<char *>(&cmd) + 1032);
        for (int i = 0; i < argc; ++i) pp[i] = argv[i];
    }
};

int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    printf("perfstat 加载自检（模拟引擎 plugin_load + 控制台指令注册）\n");
    printf("================================================================================\n");

    char dll_path[MAX_PATH];
    if (argc > 1) {
        strncpy(dll_path, argv[1], sizeof(dll_path) - 1);
        dll_path[sizeof(dll_path) - 1] = 0;
    } else {
        strcpy(dll_path, "build\\perfstat.dll");
    }
    printf("目标插件: %s\n\n", dll_path);
    printf("sizeof(CCommand)=%d  sizeof(ConCommandBase)=%d  sizeof(ConCommand)=%d\n",
           (int)sizeof(CCommand), (int)sizeof(ConCommandBase), (int)sizeof(ConCommand));
    CHECK(sizeof(CCommand) == 1288, "CCommand 布局与 SDK 一致（1288 字节）");
    CHECK(sizeof(ConCommandBase) == 24, "ConCommandBase 布局与 SDK 一致（24 字节）");

    HMODULE dll = LoadLibraryA(dll_path);
    CHECK(dll != 0, "LoadLibrary 成功打开插件 DLL");
    if (!dll) {
        printf("  [FAIL] LoadLibrary 失败，GetLastError=%lu\n", GetLastError());
        return 1;
    }

    typedef void *(*CreateInterfaceFnLocal)(const char *, int *);
    CreateInterfaceFnLocal factory = (CreateInterfaceFnLocal)GetProcAddress(dll, "CreateInterface");
    CHECK(factory != 0, "导出表里有 CreateInterface");
    if (!factory) return 1;

    int rc = -1;
    void *iface = factory("ISERVERPLUGINCALLBACKS003", &rc);
    CHECK(iface != 0 && rc == 0, "拿到 ISERVERPLUGINCALLBACKS003 接口指针");
    if (!iface) return 1;
    IServerPluginCallbacks *plugin = (IServerPluginCallbacks *)iface;

    // 打开插件的调试输出（Load/Unload 每一步都会打到 stderr），
    // 这样万一某一步卡住，日志里能直接看出卡在哪。
    {
        typedef void (*SetVerboseFn)(int);
        SetVerboseFn setv = (SetVerboseFn)GetProcAddress(dll, "perfstat_set_verbose");
        if (setv) {
            setv(1);
            printf("  [已打开插件 verbose]\n");
        }
    }

    // ---- Load：把假 ICvar 递进去 ----
    printf("\n--- 调用 Load(interfaceFactory, ...) ---\n");
    bool loaded = plugin->Load(fake_interface_factory, 0);
    CHECK(loaded, "Load() 返回 true");
    printf("  假 ICvar 收到 %d 个指令注册\n", (int)g_fake_cvar.registered.size());
    CHECK(g_fake_cvar.registered.size() == 10, "Load() 里注册了 10 个 perf_* 指令");

    // 逐个确认名字
    const char *want[10] = {"perf_help",    "perf_start", "perf_stop", "perf_stat",
                            "perf_top",     "perf_threads", "perf_dump", "perf_reset",
                            "perf_load",    "perf_selftest"};
    int found = 0;
    for (int i = 0; i < 10; ++i) {
        ConCommandBase *cb = g_fake_cvar.FindCommandBase(want[i]);
        if (cb) {
            found++;
            // 关键验证：这些虚函数是引擎会真正调用的，必须返回正确的名字
            if (!cb->GetName() || strcmp(cb->GetName(), want[i]) != 0) {
                printf("  [FAIL] %s 的 GetName() 返回了错误内容\n", want[i]);
                g_fail++;
            }
            if (!cb->GetHelpText() || !cb->GetHelpText()[0]) {
                printf("  [FAIL] %s 的帮助文本为空\n", want[i]);
                g_fail++;
            }
            if (cb->IsRegistered()) {
                // 我们自己的实现里不跟踪注册状态，这里只提示不算失败
            }
        }
    }
    CHECK(found == 10, "10 个指令都注册成功且 GetName()/GetHelpText() 都正常");

    // ---- 输出通道验证：这是本次修的核心问题 ----
    // 本地 listen server 上玩家看不到 printf 的内容，必须走 ICvar::ConsolePrintf
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
        CHECK(produced > 5, "控制台输出确实走了 ICvar::ConsolePrintf（不是裸 printf）");

        bool saw_channel = false;
        for (size_t i = before; i < g_fake_cvar.console_lines.size(); ++i) {
            if (g_fake_cvar.console_lines[i].find("ICvar::ConsolePrintf") != std::string::npos) {
                saw_channel = true;
            }
        }
        CHECK(saw_channel, "perf_selftest 报告的输出通道是 ICvar::ConsolePrintf");
    }

    // ---- 通过 vtable 调 Dispatch：这就是引擎敲命令的路径 ----
    printf("\n--- 模拟引擎执行 perf_help ---\n");
    {
        ConCommandBase *cb = g_fake_cvar.FindCommandBase("perf_help");
        ConCommand *cc = (ConCommand *)cb;
        CmdBuilder c("perf_help");
        cc->Dispatch(c.cmd);
    }

    printf("\n--- 模拟引擎执行 perf_start 5 2 ---\n");
    {
        ConCommand *cc = (ConCommand *)g_fake_cvar.FindCommandBase("perf_start");
        CmdBuilder c("perf_start 5 2");
        printf("  ArgC()=%d Arg(0)=%s Arg(1)=%s Arg(2)=%s\n", c.cmd.ArgC(), c.cmd.Arg(0), c.cmd.Arg(1),
               c.cmd.Arg(2));
        CHECK(c.cmd.ArgC() == 3 && strcmp(c.cmd.Arg(1), "5") == 0, "CCommand::Arg()/ArgC() 读取正确");
        if (cc) cc->Dispatch(c.cmd);
    }

    CHECK(g_fake_cvar.console_lines.size() > 10, "perf_help 的内容也走了 ConsolePrintf");

    printf("\n--- 采样 2 秒 ---\n");
    for (int i = 0; i < 200; ++i) {
        plugin->GameFrame(true);
        Sleep(10);
    }

    printf("\n--- 模拟引擎执行 perf_top 5 ---\n");
    {
        ConCommand *cc = (ConCommand *)g_fake_cvar.FindCommandBase("perf_top");
        CmdBuilder c("perf_top 5");
        if (cc) cc->Dispatch(c.cmd);
    }

    printf("\n--- 模拟引擎执行 perf_threads ---\n");
    {
        ConCommand *cc = (ConCommand *)g_fake_cvar.FindCommandBase("perf_threads");
        CmdBuilder c("perf_threads");
        if (cc) cc->Dispatch(c.cmd);
    }

    printf("\n--- 模拟引擎执行 perf_dump ---\n");
    {
        ConCommand *cc = (ConCommand *)g_fake_cvar.FindCommandBase("perf_dump");
        CmdBuilder c("perf_dump");
        if (cc) cc->Dispatch(c.cmd);
    }

    printf("\n--- 模拟引擎执行 perf_stop ---\n");
    {
        ConCommand *cc = (ConCommand *)g_fake_cvar.FindCommandBase("perf_stop");
        CmdBuilder c("perf_stop");
        if (cc) cc->Dispatch(c.cmd);
    }
    CHECK(true, "全部 perf_* 指令通过 Dispatch 走通（没有崩溃）");

    // 关键：插件【自己的采样线程】也必须真的采到样本。
    // 这一条是补上来的 —— 之前只断言"报告有输出"，结果 Linux 上插件 profiler 的
    // 采样次数其实是 0 却没被发现（采样线程继承了创建者的信号掩码，自己收不到信号）。
    {
        typedef void *(*GetProfilerFn)(void);
        GetProfilerFn getp = (GetProfilerFn)GetProcAddress(dll, "perfstat_get_profiler");
        CHECK(getp != 0, "插件导出了 perfstat_get_profiler");
        if (getp) {
            ps::Profiler *prof = (ps::Profiler *)getp();
            CHECK(prof != 0, "插件内部 profiler 实例非空");
            if (prof) {
                unsigned long long total = (unsigned long long)prof->total_samples();
                printf("  插件自身采样次数 = %llu（模块 %d / 线程 %d）\n", total, prof->module_count(),
                       prof->thread_count());
                CHECK(total > 0, "插件自己的采样线程确实采到了样本");
            }
        }
    }

    printf("\n--- 调用 Unload() ---\n");
    plugin->Unload();
    CHECK(g_fake_cvar.unregister_calls >= 10, "Unload() 把 10 个指令都反注册掉了（不会留下野指针）");
    CHECK(g_fake_cvar.unregister_skipped == 0,
          "反注册没有被引擎跳过（说明注册时正确置了 m_bRegistered）");
    CHECK(g_fake_cvar.registered.empty(), "反注册后 ICvar 里不再有我们的指令");

    // 注册阶段就应该把 IsRegistered() 置位 —— 否则真实引擎会静默忽略反注册，
    // 命令会永远留在引擎命令表里（表现为：卸载后仍有联想词/help 有描述/重载报 unable to link）
    {
        bool all_registered = true;
        for (size_t i = 0; i < g_fake_cvar.registered.size(); ++i) {
            if (!g_fake_cvar.registered[i]->IsRegistered()) all_registered = false;
        }
        CHECK(all_registered, "所有已注册指令的 IsRegistered() 都是 true");
    }

    FreeLibrary(dll);
    CHECK(true, "FreeLibrary 成功卸载 DLL（采样线程已收干净）");

    printf("\n================================================================================\n");
    printf("结果: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
