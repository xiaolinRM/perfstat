//========================================================================================
// perfstat - 插件入口
//
//   * 实现 IServerPluginCallbacks（22 个回调），导出 CreateInterface，
//     可以直接用 plugin_load / plugin_unload 加载卸载，不依赖 metamod / sourcemod。
//   * 提供控制台指令：
//        perf_help            显示帮助
//        perf_start [ms] [秒] 开始采样（ms=采样间隔，秒=到点自动停止并出报告）
//        perf_stop            停止采样
//        perf_stat [top]      在服务器控制台打印【模块排名】报告
//        perf_top  [top]      在服务器控制台打印报告 + 热点明细
//        perf_threads         附加线程维度明细
//        perf_dump [文件名]   把完整报告写入 logs/perfstat_*.log 并打印路径
//        perf_reset           清空统计重新开始
//        perf_load            重新读取 perfstat.ini
//
//   附带：同目录/game 目录下的 perfstat.ini 可以设置默认参数（见 config 段）。
//========================================================================================

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "core.h"
#include "plugin_api.h"

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "perfstat.h"

// ICvar 指针（在下方“控制台指令注册”一节里定义；这里提前声明以便输出通道使用）
static ICvar *g_pCvar = 0;

//----------------------------------------------------------------------------------------
// 调试开关
//
// 打开后 Load/Unload 的每一步都会往 stderr 打时间戳，用来定位"卸载卡住"这类问题。
// 引擎那边不会打开它（也不该在控制台里输出英文调试信息）；
// 只有离线加载自检在跑之前会把它打开。
//----------------------------------------------------------------------------------------
bool g_perfstat_verbose = false;

// 平台层/核心层用的统一诊断开关（定义在 core.cpp，那里离线自检也会链接）
extern "C" int g_perfstat_verbose_flag;

// 每行都带一个相对 Load 开始时刻的毫秒时间戳，
// 这样一眼就能看出"哪一步花了多久"、"卡在哪一步"。
static double g_vlog_t0 = -1.0;

static void vlog(const char *fmt, ...) {
    if (!g_perfstat_verbose) return;
    if (g_vlog_t0 < 0) g_vlog_t0 = ps::ps_now_seconds();
    double ms = (ps::ps_now_seconds() - g_vlog_t0) * 1000.0;

    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[perfstat-hb %8.1f ms] ", ms);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
}

// 导出给离线自检用（名字带前后缀，避免和别的插件撞）
extern "C" PS_DLL_EXPORT void perfstat_set_verbose(int on) {
    g_perfstat_verbose = (on != 0);
    g_perfstat_verbose_flag = (on != 0);
}

// 离线自检用它来读"插件自己的采样线程到底采了多少样本"。
// 直接 dlsym("ps::g_profiler") 是不可靠的（那是变量，mangled 名字依赖编译器），
// 所以明确导出一个函数。
extern "C" PS_DLL_EXPORT void *perfstat_get_profiler(void) { return (void *)ps::g_profiler; }

//----------------------------------------------------------------------------------------
// 控制台输出
//
// 为什么不用 printf：在本地 listen server 里，游戏进程的 C 运行时 stdout 并不接到
// 游戏控制台，所以玩家在自己的控制台里什么都看不到（但服务器控制台窗口/日志里有）。
// 正确做法是走引擎自己的输出通道，按优先级依次尝试：
//
//   1) ICvar::ConsolePrintf  —— 最可靠。注册了 ConsoleDisplayFunc 的实现会把文字
//                              送到服务器控制台（listen server 下玩家也能看到）
//   2) tier0!ConMsg          —— host 进程里 tier0 一定已加载，走引擎 spew 输出
//   3) fputs(stdout)         —— 兜底（纯命令行 srcds / 重定向到文件时有用）
//----------------------------------------------------------------------------------------
namespace {

bool g_quiet = false;
int g_console_channel = 0;  // 1=ICvar::ConsolePrintf 2=ConMsg 3=stdout
ICvar *g_cvar_for_output = 0;
ConMsgFn g_conmsg = 0;
bool g_output_probed = false;

void probe_output_channels() {
    if (g_output_probed) return;
    g_output_probed = true;

    g_cvar_for_output = g_pCvar;
    if (g_cvar_for_output) {
        g_console_channel = 1;
        return;
    }

    // host 进程里 tier0 一定在；模块名各平台不同，都试一遍
    static const char *kTier0[] = {"tier0.dll", "tier0_s.dll", "libtier0_srv.so",
                                   "libtier0.so", "tier0.so", 0};
    static const char *kFuncs[] = {"ConMsg", "Msg", 0};
    for (int i = 0; kTier0[i] && !g_conmsg; ++i) {
#if defined(_WIN32)
        HMODULE m = GetModuleHandleA(kTier0[i]);
        if (!m) continue;
        for (int k = 0; kFuncs[k] && !g_conmsg; ++k) {
            g_conmsg = (ConMsgFn)GetProcAddress(m, kFuncs[k]);
        }
#else
        void *m = dlopen(kTier0[i], RTLD_NOW | RTLD_NOLOAD);
        if (!m) continue;
        for (int k = 0; kFuncs[k] && !g_conmsg; ++k) {
            g_conmsg = (ConMsgFn)dlsym(m, kFuncs[k]);
        }
#endif
    }
    if (g_conmsg) {
        g_console_channel = 2;
        return;
    }
    g_console_channel = 3;
}

const char *console_channel_name() {
    switch (g_console_channel) {
        case 1: return "ICvar::ConsolePrintf";
        case 2: return "tier0!ConMsg";
        case 3: return "stdout (兜底)";
        default: return "(未探测)";
    }
}

void console_out(const char *text) {
    if (g_quiet || !text || !text[0]) return;
    probe_output_channels();

    if (g_console_channel == 1 && g_cvar_for_output) {
        // ConsolePrintf 是 printf 风格，这里 text 里可能有 % 号，用 %s 转发最安全
        g_cvar_for_output->ConsolePrintf("%s", text);
        return;
    }
    if (g_console_channel == 2 && g_conmsg) {
        g_conmsg("%s", text);
        return;
    }
    // 兜底：换行符在部分 Windows 控制台下需要 \r\n 才能正确刷新
    fputs(text, stdout);
    fflush(stdout);
}

// 边打印边攒进字符串，方便同时写文件
struct CaptureOut {
    std::string *sink;
    bool to_console;
};

void capture_out(void *user, const char *text) {
    CaptureOut *c = (CaptureOut *)user;
    if (c->sink) c->sink->append(text);
    if (c->to_console) console_out(text);
}

void line_out(void *user, const char *text) {
    (void)user;
    console_out(text);
}

//----------------------------------------------------------------------------------------
// 路径工具
//----------------------------------------------------------------------------------------
std::string exe_dir() {
#if defined(_WIN32)
    char buf[1024];
    DWORD n = GetModuleFileNameA(0, buf, sizeof(buf) - 1);
    if (n == 0) return std::string(".");
    buf[n] = 0;
    std::string s(buf);
    size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? std::string(".") : s.substr(0, p);
#else
    char buf[2048];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string(".");
    buf[n] = 0;
    std::string s(buf);
    size_t p = s.find_last_of('/');
    return p == std::string::npos ? std::string(".") : s.substr(0, p);
#endif
}

std::string cwd_dir() {
    char buf[2048];
#if defined(_WIN32)
    if (_getcwd(buf, sizeof(buf)) == 0) return std::string(".");
#else
    if (getcwd(buf, sizeof(buf)) == 0) return std::string(".");
#endif
    return std::string(buf);
}

bool file_exists(const std::string &p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool dir_exists(const std::string &p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

void make_dir(const std::string &p) {
#if defined(_WIN32)
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0755);
#endif
}

std::string join_path(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    char last = a[a.size() - 1];
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

// 找一个合适的日志目录：优先 <exe目录>/logs，其次 <cwd>/logs
std::string resolve_log_dir() {
    std::vector<std::string> candidates;
    candidates.push_back(join_path(exe_dir(), "logs"));
    candidates.push_back(join_path(cwd_dir(), "logs"));
    candidates.push_back(exe_dir());
    candidates.push_back(cwd_dir());
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (dir_exists(candidates[i])) return candidates[i];
    }
    std::string d = join_path(cwd_dir(), "logs");
    make_dir(d);
    if (dir_exists(d)) return d;
    return cwd_dir();
}

std::string timestamp_name() {
    int y, mo, d, h, mi, s;
    ps::ps_get_local_time(&y, &mo, &d, &h, &mi, &s);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", y, mo, d, h, mi, s);
    return std::string(buf);
}

//----------------------------------------------------------------------------------------
// 采样线程
//----------------------------------------------------------------------------------------
volatile bool g_thread_stop = false;
volatile bool g_thread_alive = false;

void sampler_entry() {
    g_thread_alive = true;
    // 第一步：确保本线程能收到采样信号（掩码是继承来的，可能被创建者屏蔽了）
    ps::ps_prepare_sampling_thread();
    while (!g_thread_stop) {
        if (ps::g_profiler && ps::g_profiler->running()) {
            ps::g_profiler->sampler_loop();
        } else {
#if defined(_WIN32)
            Sleep(50);
#else
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 50 * 1000 * 1000;
            nanosleep(&ts, 0);
#endif
        }
    }
    g_thread_alive = false;
}

#if defined(_WIN32)
unsigned __stdcall sampler_entry_win(void *) {
    sampler_entry();
    return 0;
}
HANDLE g_thread_handle = 0;
#else
void *sampler_entry_posix(void *) {
    sampler_entry();
    return 0;
}
pthread_t g_thread = 0;
#endif

bool start_sampler_thread() {
    g_thread_stop = false;
#if defined(_WIN32)
    if (g_thread_handle) return true;
    uintptr_t h = _beginthreadex(0, 0, sampler_entry_win, 0, 0, 0);
    if (!h) return false;
    g_thread_handle = (HANDLE)h;
    // 采样线程优先级调低一点，尽量不抢服务器线程的 CPU
    SetThreadPriority(g_thread_handle, THREAD_PRIORITY_BELOW_NORMAL);
    // 给采样线程起个名字，方便在报告里认出来
    typedef HRESULT(WINAPI * SetThreadDescriptionFn)(HANDLE, PCWSTR);
    static SetThreadDescriptionFn fn = 0;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (k32) fn = (SetThreadDescriptionFn)GetProcAddress(k32, "SetThreadDescription");
    }
    if (fn) {
        const wchar_t *nm = L"perfstat-sampler";
        fn(g_thread_handle, nm);
    }
    return true;
#else
    if (g_thread) return true;
    if (pthread_create(&g_thread, 0, sampler_entry_posix, 0) != 0) return false;
    return true;
#endif
}

// 必须在 DLL 卸载前把采样线程收干净，否则线程会跑在已卸载的代码上直接崩服。
//
// 【为什么不能"超时就放弃等待（detach）"】
// 踩过的坑：上一版超时后直接 pthread_detach 就走人，结果那个游离线程继续跑，
// 进程退出时它还在已卸载的代码里 —— CI 上就是 Segmentation fault (exit 139)。
// 所以现在的策略是：
//   1. 先调 ps_request_stop_sampling()，让正在进行的采样在毫秒级内放弃本轮
//      （否则线程多的机器上一轮要几百毫秒，看起来就像"线程不肯退"）；
//   2. 再耐心等它退出。Linux 上用 tryjoin 轮询（能检测到"真的退了"），
//      超时了只告警、**不 detach**，避免制造野线程；
//   3. 等到了就 join，彻底回收。
void stop_sampler_thread() {
    g_thread_stop = true;
    ps::ps_request_stop_sampling();
    vlog("stop_sampler: stop requested, waiting for sampler thread");
#if defined(_WIN32)
    if (g_thread_handle) {
        DWORD w = WaitForSingleObject(g_thread_handle, 10000);
        vlog("stop_sampler: WaitForSingleObject -> %lu (0=signaled, 258=timeout)", (unsigned long)w);
        if (w == WAIT_TIMEOUT) {
            console_out("[perfstat] 警告: 采样线程未能在 10 秒内退出（不会强行分离，"
                        "避免野线程在卸载后崩服）\n");
        }
        CloseHandle(g_thread_handle);
        g_thread_handle = 0;
    }
#else
    if (g_thread) {
        // pthread_join 没有超时版本。用 tryjoin 轮询：只有真的等到线程结束才继续，
        // 这样绝不会在采样线程还活着的时候去 dlclose。
        bool joined = false;
        for (int waited_ms = 0; waited_ms < 10000; waited_ms += 10) {
            int r = pthread_tryjoin_np(g_thread, 0);
            if (r == 0) {
                joined = true;
                vlog("stop_sampler: sampler thread exited after ~%d ms", waited_ms);
                break;
            }
            // 走到这里说明线程还没退（EBUSY），继续等
            ps_sleep_ms(10);
        }
        if (!joined) {
            vlog("stop_sampler: TIMEOUT after 10s, sampler thread still alive");
            console_out("[perfstat] 警告: 采样线程未能在 10 秒内退出（不会强行分离，"
                        "避免野线程在卸载后崩服）\n");
        }
        g_thread = 0;
    }
#endif
    vlog("stop_sampler: done");
}

//----------------------------------------------------------------------------------------
// 配置
//----------------------------------------------------------------------------------------
struct Config {
    int sample_ms;
    int auto_stop_sec;
    int console_top;    // perf_stat 打印多少个模块
    int hot_top;        // 热点显示条数
    int min_hits;       // 热点门槛
    bool show_hot;      // 默认是否展开热点
    bool show_threads;  // 默认是否显示线程
    bool auto_start;    // 插件加载后是否自动开始采样
    int auto_dump_sec;  // 自动落盘周期（0=关闭）
    std::string auto_dump_path;

    Config()
        : sample_ms(10),
          auto_stop_sec(0),
          console_top(25),
          hot_top(8),
          min_hits(2),
          show_hot(false),
          show_threads(false),
          auto_start(true),
          auto_dump_sec(0) {}
};

Config g_config;

int parse_int(const char *s, int def) {
    if (!s || !s[0]) return def;
    return atoi(s);
}

bool parse_bool(const char *s, bool def) {
    if (!s || !s[0]) return def;
    if (!strcmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcasecmp(s, "on"))
        return true;
    if (!strcmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "no") || !strcasecmp(s, "off"))
        return false;
    return def;
}

void load_config_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == 0 || *p == '\r' || *p == '\n') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p;
        char *val = eq + 1;
        size_t kl = strlen(key);
        while (kl > 0 && (key[kl - 1] == ' ' || key[kl - 1] == '\t')) key[--kl] = 0;
        while (*val == ' ' || *val == '\t') ++val;
        size_t vl = strlen(val);
        while (vl > 0 &&
               (val[vl - 1] == '\n' || val[vl - 1] == '\r' || val[vl - 1] == ' ' || val[vl - 1] == '\t'))
            val[--vl] = 0;

        if (!strcasecmp(key, "sample_ms"))
            g_config.sample_ms = parse_int(val, g_config.sample_ms);
        else if (!strcasecmp(key, "duration_sec"))
            g_config.auto_stop_sec = parse_int(val, g_config.auto_stop_sec);
        else if (!strcasecmp(key, "console_top"))
            g_config.console_top = parse_int(val, g_config.console_top);
        else if (!strcasecmp(key, "hotspot_top"))
            g_config.hot_top = parse_int(val, g_config.hot_top);
        else if (!strcasecmp(key, "hotspot_min_hits"))
            g_config.min_hits = parse_int(val, g_config.min_hits);
        else if (!strcasecmp(key, "show_hotspot"))
            g_config.show_hot = parse_bool(val, g_config.show_hot);
        else if (!strcasecmp(key, "show_threads"))
            g_config.show_threads = parse_bool(val, g_config.show_threads);
        else if (!strcasecmp(key, "auto_start"))
            g_config.auto_start = parse_bool(val, g_config.auto_start);
        else if (!strcasecmp(key, "auto_dump_sec"))
            g_config.auto_dump_sec = parse_int(val, g_config.auto_dump_sec);
        else if (!strcasecmp(key, "auto_dump_path"))
            g_config.auto_dump_path = val;
    }
    fclose(f);
}

std::string config_path() {
    std::vector<std::string> candidates;
    candidates.push_back(join_path(cwd_dir(), "perfstat.ini"));
    candidates.push_back(join_path(exe_dir(), "perfstat.ini"));
    candidates.push_back(join_path(join_path(exe_dir(), "cfg"), "perfstat.ini"));
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (file_exists(candidates[i])) return candidates[i];
    }
    return std::string();
}

}  // namespace

//----------------------------------------------------------------------------------------
// 控制台指令注册（走 ICvar::RegisterConCommand）
//
// 为什么必须注册：引擎的 Cmd_ExecuteString 是先查自己的命令表再决定调用谁。
// 没注册过的名字会直接报 "Unknown command"，插件的 ClientCommand 根本收不到。
// 引擎里这件事本来是 ConVar_Register() 干的，插件没有 tier1，所以这里自己实现
// 一个最小访问器，把注册动作转给 ICvar::RegisterConCommand。
//----------------------------------------------------------------------------------------
// （g_pCvar 已在文件上方提前声明，输出通道也要用它）

// ConCommandBase 的静态成员（SDK 里由 tier1 定义，这里自己给一份）
ConCommandBase *ConCommandBase::s_pConCommandBases = 0;
IConCommandBaseAccessor *ConCommandBase::s_pAccessor = 0;

class PerfstatCVarAccessor : public IConCommandBaseAccessor {
public:
    virtual bool RegisterConCommandBase(ConCommandBase *pVar) {
        if (!g_pCvar || !pVar) return false;
        g_pCvar->RegisterConCommand(pVar);
        return true;
    }
};

namespace {
PerfstatCVarAccessor g_cvar_accessor;
ConCommandBase *g_cmd_head = 0;  // 自己维护一份链表，方便卸载时反注册
int g_cmd_count = 0;

// 指令回调：统一转发给插件实例的 ClientCommand（和引擎走的是同一条逻辑）
void ps_cmd_dispatch(const CCommand &cmd) { ps::g_perfstat.ClientCommand(0, cmd); }

ConCommand *perf_register_command(const char *name, const char *help) {
    ConCommand *cmd = new ConCommand(name, ps_cmd_dispatch, help, 0);
    cmd->SetNext(g_cmd_head);
    g_cmd_head = cmd;
    g_cmd_count++;
    if (g_pCvar) g_pCvar->RegisterConCommand(cmd);
    return cmd;
}

void perf_unregister_commands() {
    vlog("unregister: begin (%d commands)", g_cmd_count);
    if (g_pCvar) {
        for (ConCommandBase *p = g_cmd_head; p; p = p->GetNext()) {
            vlog("unregister: UnregisterConCommand('%s')", p->GetName() ? p->GetName() : "?");
            g_pCvar->UnregisterConCommand(p);
        }
    }
    vlog("unregister: UnregisterConCommand done");
    ConCommandBase *p = g_cmd_head;
    while (p) {
        ConCommandBase *next = p->GetNext();
        vlog("unregister: delete '%s'", p->GetName() ? p->GetName() : "?");
        delete p;
        p = next;
    }
    g_cmd_head = 0;
    g_cmd_count = 0;
    vlog("unregister: end");
}

void ps_register_console_commands() {
    if (!g_pCvar || g_cmd_count > 0) return;

    perf_register_command("perf_help", "perfstat: 显示全部指令与读表说明");
    perf_register_command("perf_start", "perfstat: perf_start [采样间隔ms] [时长秒]");
    perf_register_command("perf_stop", "perfstat: 停止采样（数据保留）");
    perf_register_command("perf_stat", "perfstat: 打印模块 CPU/内存排名表");
    perf_register_command("perf_top", "perfstat: 排名表 + 模块内热点函数");
    perf_register_command("perf_threads", "perfstat: 报告 + 线程维度明细");
    perf_register_command("perf_dump", "perfstat: 把完整报告写入 logs 目录");
    perf_register_command("perf_reset", "perfstat: 清空统计并重新开始");
    perf_register_command("perf_load", "perfstat: 重新读取 perfstat.ini");
    perf_register_command("perf_selftest", "perfstat: 自检（输出通道/采样/落盘）");

    char buf[128];
    snprintf(buf, sizeof(buf), "[perfstat] 已注册 %d 个控制台指令\n", g_cmd_count);
    console_out(buf);
}

}  // namespace

//----------------------------------------------------------------------------------------
// 插件
//----------------------------------------------------------------------------------------
namespace ps {
Profiler *g_profiler = 0;
PerfStatPlugin g_perfstat;

PerfStatPlugin::PerfStatPlugin()
    : m_last_auto_dump(0), m_dumped_once(false) {}

bool PerfStatPlugin::Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn) {
    g_vlog_t0 = ps::ps_now_seconds();
    vlog("Load: begin");
    ps_platform_init();
    vlog("Load: platform init done");
    g_profiler = new Profiler();
    g_profiler->set_sample_interval_ms(g_config.sample_ms);
    g_profiler->set_auto_stop_sec(g_config.auto_stop_sec);
    g_profiler->init_once();

    std::string cfg = config_path();
    if (!cfg.empty()) {
        load_config_file(cfg);
        g_profiler->set_sample_interval_ms(g_config.sample_ms);
        g_profiler->set_auto_stop_sec(g_config.auto_stop_sec);
    }

    start_sampler_thread();
    vlog("Load: sampler thread started");

    // 取 ICvar 并注册控制台指令（不注册的话引擎会当成 Unknown command 直接丢掉）
    if (interfaceFactory) {
        g_pCvar = (ICvar *)interfaceFactory(CVAR_INTERFACE_VERSION, 0);
    }
    if (g_pCvar) {
        ps_register_console_commands();
    } else {
        console_out("[perfstat] 警告: 拿不到 " CVAR_INTERFACE_VERSION
                    " 接口，perf_* 指令将无法注册。\n");
        console_out("[perfstat]       此时可以改用 cfg/perfstat.ini 配置采样，无需输入指令。\n");
    }

    if (g_config.auto_start) {
        g_profiler->set_running(true);
    }
    m_last_auto_dump = ps::ps_now_seconds();

    console_out("================================================================================\n");
    console_out(" perfstat v" PERFSTAT_VERSION " 已加载（纯引擎插件，不依赖 metamod / sourcemod）\n");
    console_out("--------------------------------------------------------------------------------\n");
    console_out(" 功能: 采样统计本服务器进程内部每个 DLL/SO 占用的 CPU 与内存，并从大到小排序\n");
    console_out(" 指令: perf_help 查看全部指令；perf_stat 查看模块排名；perf_top 查看热点函数\n");
    console_out("       perf_dump 输出报告到 logs 目录；perf_start [间隔ms] [秒] 开始采样\n");
    console_out(" 提示: 采样是统计抽样，跑 1 分钟以上结论才可靠；不采样时几乎没有额外开销\n");
    if (!cfg.empty()) {
        char buf[600];
        snprintf(buf, sizeof(buf), " 配置: 已读取 %s\n", cfg.c_str());
        console_out(buf);
    }
    char buf[256];
    snprintf(buf, sizeof(buf), " 状态: %s (采样间隔 %d ms)\n",
             g_profiler->running() ? "已自动开始采样" : "未开始采样，请执行 perf_start",
             g_profiler->sample_interval_ms());
    console_out(buf);
    console_out("================================================================================\n");
    vlog("Load: end");
    return true;
}

void PerfStatPlugin::Unload(void) {
    vlog("Unload: begin");
    stop_sampler_thread();
    vlog("Unload: sampler thread stopped");
    perf_unregister_commands();
    vlog("Unload: console commands unregistered");
    if (g_pCvar) {
        g_pCvar = 0;
    }
    if (g_profiler) {
        g_profiler->set_running(false);
        vlog("Unload: profiler stopped");
        delete g_profiler;
        g_profiler = 0;
        vlog("Unload: profiler deleted");
    }
    ps_platform_shutdown();
    vlog("Unload: platform shutdown done");
    console_out("[perfstat] 已卸载\n");
    vlog("Unload: end");
}

void PerfStatPlugin::Pause(void) {
    if (g_profiler) g_profiler->set_running(false);
}

void PerfStatPlugin::UnPause(void) {
    if (g_profiler) g_profiler->set_running(true);
}

const char *PerfStatPlugin::GetPluginDescription(void) {
    return "perfstat v" PERFSTAT_VERSION " - in-process CPU / memory profiler per module";
}

void PerfStatPlugin::GameFrame(bool) {
    if (!g_profiler) return;
    if (g_profiler->should_auto_stop()) {
        g_profiler->set_running(false);
        console_out("[perfstat] 到达设定时长，已自动停止采样。执行 perf_stat / perf_dump 查看结果\n");
        if (!g_config.auto_dump_path.empty()) {
            std::string dummy;
            write_report_file(g_config.auto_dump_path, dummy);
        }
    }
    if (g_config.auto_dump_sec > 0) {
        double now = ps_now_seconds();
        if (now - m_last_auto_dump >= (double)g_config.auto_dump_sec) {
            m_last_auto_dump = now;
            std::string dummy;
            write_report_file(std::string(), dummy);
        }
    }
}

//----------------------------------------------------------------------------------------
// 报告输出
//----------------------------------------------------------------------------------------
bool PerfStatPlugin::write_report_file(const std::string &explicit_path, std::string &out_path) {
    if (!g_profiler) return false;

    std::string path = explicit_path;
    if (path.empty()) {
        path = join_path(resolve_log_dir(), "perfstat-" + timestamp_name() + ".log");
    }

    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        char buf[600];
        snprintf(buf, sizeof(buf), "[perfstat] 无法写入日志文件: %s\n", path.c_str());
        console_out(buf);
        return false;
    }

    CaptureOut cap;
    cap.sink = new std::string();
    cap.to_console = false;
    Profiler::Options opt;
    opt.top = g_config.hot_top;
    opt.show_hot = true;
    opt.show_threads = true;
    opt.no_memory = false;
    opt.min_hits = g_config.min_hits;
    opt.title = 0;
    g_profiler->report(capture_out, &cap, opt);

    fwrite(cap.sink->data(), 1, cap.sink->size(), f);
    fclose(f);
    delete cap.sink;

    out_path = path;
    char buf[700];
    snprintf(buf, sizeof(buf), "[perfstat] 报告已写入: %s\n", path.c_str());
    console_out(buf);
    return true;
}

//----------------------------------------------------------------------------------------
// 控制台指令
//----------------------------------------------------------------------------------------
void PerfStatPlugin::cmd_selftest() {
    console_out("================================================================================\n");
    console_out(" perfstat 自检\n");
    console_out("--------------------------------------------------------------------------------\n");
    {
        char buf[400];
        snprintf(buf, sizeof(buf),
                 " 插件版本      : %s\n"
                 " 输出通道      : %s   (1=ICvar::ConsolePrintf 2=tier0!ConMsg 3=stdout)\n"
                 " ICvar 指针    : %p\n",
                 PERFSTAT_VERSION, console_channel_name(), (void *)g_pCvar);
        console_out(buf);
    }
    console_out(" 如果你能看到这段文字，说明控制台输出通道是通的。\n");
    if (g_profiler) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 " 采样状态      : %s\n"
                 " 采样间隔      : %d ms\n"
                 " 已采样本      : %llu\n"
                 " 模块数/线程数 : %d / %d\n"
                 " 实际采样频率  : %.1f 次/秒\n",
                 g_profiler->running() ? "正在采样" : "未采样",
                 g_profiler->sample_interval_ms(),
                 (unsigned long long)g_profiler->total_samples(), g_profiler->module_count(),
                 g_profiler->thread_count(),
                 g_profiler->elapsed_seconds() > 0
                     ? (double)g_profiler->total_samples() / g_profiler->elapsed_seconds()
                     : 0.0);
        console_out(buf);
    }
    {
        std::string path;
        if (write_report_file(std::string(), path)) {
            char buf[700];
            snprintf(buf, sizeof(buf), " 落盘测试      : 成功 -> %s\n", path.c_str());
            console_out(buf);
        } else {
            console_out(" 落盘测试      : 失败（日志目录不可写）\n");
        }
    }
    console_out("================================================================================\n");
}

void PerfStatPlugin::cmd_help() {
    console_out("================================================================================\n");
    console_out(" perfstat 指令说明\n");
    console_out("--------------------------------------------------------------------------------\n");
    console_out(" perf_start [间隔ms] [时长秒]\n");
    console_out("     开始采样。间隔默认 10ms（越小越精细、开销越大，建议 5~50）。\n");
    console_out("     例如: perf_start 10 60   表示 10ms 采一次，跑 60 秒后自动停止。\n");
    console_out(" perf_stop            停止采样（数据保留，可继续 perf_stat 查看）\n");
    console_out(" perf_stat [行数]     在控制台打印模块排名表（默认 25 行）\n");
    console_out(" perf_top  [行数]     同上，并展开每个模块内部最热的函数/偏移\n");
    console_out(" perf_threads         打印报告并附加“线程维度”明细（谁在烧 CPU）\n");
    console_out(" perf_dump [路径]     把完整报告写入日志文件（默认 logs/perfstat-时间.log）\n");
    console_out(" perf_reset           清空统计并立刻重新开始采样\n");
    console_out(" perf_load            重新读取 perfstat.ini\n");
    console_out(" perf_selftest        自检：确认控制台输出通道、采样、落盘是否正常\n");
    console_out("--------------------------------------------------------------------------------\n");
    console_out(" 怎么读懂结果:\n");
    console_out("   CPU%   = 该模块命中采样数 / 总采样数，近似“占满一个 CPU 核心的比例”。\n");
    console_out("            srcds 主循环在 engine.dll / server.dll 里，它们通常是第一名。\n");
    console_out("   MEM    = 该模块相关内存。map 是 DLL 文件映射的驻留内存，\n");
    console_out("            priv 是按地址区间估算的私有提交（堆/栈/运行时分配），\n");
    console_out("            引擎的全局分配器都记在主程序名下，所以主程序的 priv 通常最大。\n");
    console_out("   热点   = 具体是哪个导出函数/偏移最吃 CPU，能直接定位到优化目标。\n");
    console_out("--------------------------------------------------------------------------------\n");
    console_out(" 采样原理: 定时逐个线程 SuspendThread/信号中断，读取当前指令指针(EIP)，\n");
    console_out("           按地址落在哪个模块归属统计；统计抽样，不是精确计时。\n");
    console_out("================================================================================\n");
}

void PerfStatPlugin::cmd_start(int argc, const char **argv) {
    if (!g_profiler) return;
    int ms = argc >= 2 ? parse_int(argv[1], g_config.sample_ms) : g_config.sample_ms;
    int sec = argc >= 3 ? parse_int(argv[2], g_config.auto_stop_sec) : g_config.auto_stop_sec;
    g_profiler->set_sample_interval_ms(ms);
    g_profiler->set_auto_stop_sec(sec);
    g_profiler->set_running(true);
    char buf[300];
    snprintf(buf, sizeof(buf), "[perfstat] 已开始采样：间隔 %d ms%s\n", g_profiler->sample_interval_ms(),
             sec > 0 ? "，到时间自动停止" : "");
    console_out(buf);
    if (sec > 0) {
        snprintf(buf, sizeof(buf), "[perfstat] 预计 %d 秒后自动停止；期间可用 perf_stat 随时查看\n",
                 sec);
        console_out(buf);
    }
}

void PerfStatPlugin::cmd_stop() {
    if (!g_profiler) return;
    bool was = g_profiler->running();
    g_profiler->set_running(false);
    if (was) {
        char buf[200];
        snprintf(buf, sizeof(buf), "[perfstat] 已停止采样，共 %llu 次采样，用时 %.1f 秒\n",
                 (unsigned long long)g_profiler->total_samples(), g_profiler->elapsed_seconds());
        console_out(buf);
    } else {
        console_out("[perfstat] 当前并没有在采样\n");
    }
}

void PerfStatPlugin::cmd_stat(int top, bool hot, bool threads, bool no_memory) {
    if (!g_profiler) return;
    if (!g_profiler->inited()) g_profiler->init_once();
    Profiler::Options opt;
    opt.top = g_config.hot_top;
    opt.show_hot = hot;
    opt.show_threads = threads;
    opt.no_memory = no_memory;
    opt.min_hits = g_config.min_hits;
    opt.title = 0;
    (void)top;
    g_profiler->report(line_out, 0, opt);

    if (g_profiler->total_samples() == 0) {
        console_out("[perfstat] 提示: 还没有采到数据，请先执行 perf_start\n");
    }
}

void PerfStatPlugin::cmd_dump(int argc, const char **argv) {
    std::string path = argc >= 2 ? std::string(argv[1]) : std::string();
    std::string out;
    if (write_report_file(path, out)) {
        // 控制台只提示路径，完整内容在文件里
    }
}

void PerfStatPlugin::cmd_reset() {
    if (!g_profiler) return;
    g_profiler->reset();
    g_profiler->set_running(true);
    console_out("[perfstat] 统计已清空，重新开始采样\n");
}

void PerfStatPlugin::cmd_load() {
    std::string cfg = config_path();
    if (cfg.empty()) {
        console_out("[perfstat] 没有找到 perfstat.ini（可放在服务器根目录或 cfg 目录）\n");
        return;
    }
    load_config_file(cfg);
    if (g_profiler) {
        g_profiler->set_sample_interval_ms(g_config.sample_ms);
        g_profiler->set_auto_stop_sec(g_config.auto_stop_sec);
    }
    char buf[600];
    snprintf(buf, sizeof(buf), "[perfstat] 已重新读取配置: %s\n", cfg.c_str());
    console_out(buf);
}

//----------------------------------------------------------------------------------------
// 控制台指令分发（引擎回调）
//----------------------------------------------------------------------------------------
static int arg_int(const CCommand &args, int index, int def) {
    if (args.ArgC() <= index) return def;
    const char *s = args.Arg(index);
    if (!s || !s[0]) return def;
    return atoi(s);
}

static bool cmd_is(const CCommand &args, const char *name) {
    const char *c = args.Arg(0);
    if (!c) return false;
#if defined(_WIN32)
    return _stricmp(c, name) == 0;
#else
    return strcasecmp(c, name) == 0;
#endif
}

PLUGIN_RESULT PerfStatPlugin::ClientCommand(edict_t *, const CCommand &args) {
    if (!g_profiler) return PLUGIN_CONTINUE;

    // 收集参数（最多 4 个）
    const char *argv[4] = {0, 0, 0, 0};
    int argc = args.ArgC();
    if (argc > 4) argc = 4;
    for (int i = 0; i < argc; ++i) argv[i] = args.Arg(i);

    if (cmd_is(args, "perf_help") || cmd_is(args, "perfstat") || cmd_is(args, "perf")) {
        cmd_help();
        return PLUGIN_CONTINUE;
    }
    if (cmd_is(args, "perf_start")) {
        cmd_start(argc, argv);
        return PLUGIN_CONTINUE;
    }
    if (cmd_is(args, "perf_stop")) {
        cmd_stop();
        return PLUGIN_CONTINUE;
    }
    if (cmd_is(args, "perf_stat")) {
        cmd_stat(arg_int(args, 1, g_config.console_top), false, false);
        return PLUGIN_CONTINUE;
    }
    if (cmd_is(args, "perf_top")) {
        cmd_stat(arg_int(args, 1, g_config.console_top), true, false);
        return PLUGIN_CONTINUE;
    }
    if (cmd_is(args, "perf_threads")) {
        cmd_stat(arg_int(args, 1, g_config.console_top), true, true);
        return PLUGIN_CONTINUE;
    }
    if (cmd_is(args, "perf_dump")) {
        cmd_dump(argc, argv);
        return PLUGIN_CONTINUE;
    }
    if (cmd_is(args, "perf_reset")) {
        cmd_reset();
        return PLUGIN_CONTINUE;
    }
    if (cmd_is(args, "perf_load")) {
        cmd_load();
        return PLUGIN_CONTINUE;
    }
    if (cmd_is(args, "perf_selftest")) {
        cmd_selftest();
        return PLUGIN_CONTINUE;
    }
    if (strncmp(args.Arg(0), "perf", 4) == 0) {
        cmd_help();
    }
    return PLUGIN_CONTINUE;
}

//----------------------------------------------------------------------------------------
// 接口导出
//----------------------------------------------------------------------------------------
// （g_perfstat 实例已在文件上方注册指令那段里定义，这里不再重复定义）

}  // namespace ps

#if !defined(PERFSTAT_USE_SDK)
// SDK 里这两件事由 tier1 的 interface.cpp 完成；本插件不链接 hl2sdk，所以自己做一份，
// 行为与官方实现完全一致（单向链表注册 + 按名字查找）。加 /DPERFSTAT_USE_SDK 时跳过。
namespace {
InterfaceReg *g_InterfaceRegHead = 0;
}

InterfaceReg::InterfaceReg(InstantiateInterfaceFn fn, const char *pName)
    : m_CreateFn(fn), m_pName(pName), m_pNext(g_InterfaceRegHead) {
    g_InterfaceRegHead = this;
}

PS_DLL_EXPORT void *CreateInterface(const char *pName, int *pReturnCode) {
    if (pReturnCode) *pReturnCode = 1;  // IFACE_FAILED
    if (!pName) return 0;
    for (InterfaceReg *pCur = g_InterfaceRegHead; pCur; pCur = pCur->m_pNext) {
        if (pCur->m_pName && strcmp(pCur->m_pName, pName) == 0 && pCur->m_CreateFn) {
            if (pReturnCode) *pReturnCode = 0;  // IFACE_OK
            return pCur->m_CreateFn();
        }
    }
    return 0;
}

// 额外的显式工厂导出。
// 引擎只会用 CreateInterface，这个函数是给“离线加载自检”和需要 dlsym 的场合用的
// （Linux 的 .so 在 dlopen 时不保证 __g_Create... 静态注册对象一定被链接进来，
//   用这个显式符号最稳）。
PS_DLL_EXPORT void *perfstat_factory(const char *pName, int *pReturnCode) {
    if (pReturnCode) *pReturnCode = 1;
    if (!pName || strcmp(pName, INTERFACEVERSION_ISERVERPLUGINCALLBACKS) != 0) return 0;
    if (pReturnCode) *pReturnCode = 0;
    return (void *)static_cast<IServerPluginCallbacks *>(&ps::g_perfstat);
}
#endif

PERFSTAT_EXPOSE_PLUGIN(PerfStatPlugin, ps::g_perfstat);
