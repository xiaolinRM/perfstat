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

// 核心层的诊断开关（定义在 core.cpp；那里离线自检也会链接）
extern "C" int g_perfstat_verbose_flag;

// 定义在 core.cpp（core.h 里也有声明，但 perfstat.cpp 不包含 core.h）
extern "C" double ps_dbg_now_ms(void);


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
    ps::ps_set_debug(on != 0);  // 平台层的诊断输出（它自持标志，不依赖我们的全局变量）
}

// 离线自检用它来读"插件自己的采样线程到底采了多少样本"。
// 直接 dlsym("ps::g_profiler") 是不可靠的（那是变量，mangled 名字依赖编译器），
// 所以明确导出一个函数。
extern "C" PS_DLL_EXPORT void *perfstat_get_profiler(void) { return (void *)ps::g_profiler; }

// 自检用它来看"这台进程里，插件副本的信号处理器到底跑了多少次"。
// 如果一直是 0，说明生效的是别的副本的处理器（多副本场景下的关键证据）。
extern "C" PS_DLL_EXPORT long perfstat_handler_runs(void) {
    return ps::ps_handler_run_count();
}

// 锁诊断：返回"谁握着 profiler 的锁"（返回空串表示没人持有）。
// 采样线程卡住时用它判断是不是被锁挡住了。
extern "C" PS_DLL_EXPORT const char *perfstat_lock_holder(void) {
    return ps::g_profiler ? ps::g_profiler->lock_holder() : "";
}

// 自检用它给自己的关键点打时间戳，这样一次日志就能自证先后顺序
// （stdout 块缓冲 + stderr 无缓冲，行号不代表时间顺序）。
extern "C" PS_DLL_EXPORT double perfstat_now_ms(void) { return ps_dbg_now_ms(); }

//========================================================================================
// 平台层导出（给离线自检程序用）
//
// 【为什么必须导出】离线自检原来自己编译一份 perf_platform_linux.cpp，于是进程里同时
// 存在【两份平台层副本】（自检一份、插件一份）。而信号处理器是进程级、后装覆盖先装的，
// g_slots / g_sig 却是每份各自的 —— 两边互相踩，表现是采样超时、样本数为 0，
// 而且只在 Linux 上出现、极难定位。这个冲突打了三次补丁都没根治。
//
// 现在改成：自检【不再自己编译平台层】，一律通过 dlsym 调用插件里的这一份。
// 于是进程里只有一份平台层 —— 和生产环境（真实服务器上插件也只有一份）完全一致，
// 自检测的就是真实路径。这个函数组就是把平台层暴露出去的唯一入口。
//
// 传的都是 platform.h 里的 POD 结构（uintptr_t/size_t/char[]/bool），跨 .so 安全。
//========================================================================================
extern "C" PS_DLL_EXPORT int perfstat_ps_init(void) { return ps::ps_platform_init() ? 1 : 0; }
extern "C" PS_DLL_EXPORT void perfstat_ps_shutdown(void) { ps::ps_platform_shutdown(); }
extern "C" PS_DLL_EXPORT void perfstat_ps_enum_modules(void *out) {
    ps::ps_enum_modules((ps::ModuleVisits *)out);
}
extern "C" PS_DLL_EXPORT void perfstat_ps_enum_threads(void *out) {
    ps::ps_enum_threads((ps::ThreadVisits *)out);
}
extern "C" PS_DLL_EXPORT int perfstat_ps_sample_threads(uintptr_t *ips, uint32_t *tids,
                                                        int max_ips) {
    return ps::ps_sample_threads(ips, tids, max_ips);
}
extern "C" PS_DLL_EXPORT int perfstat_ps_sample_window(uintptr_t *ips, uint32_t *tids, int max_ips,
                                                       int *cursor, int window) {
    return ps::ps_sample_threads_window(ips, tids, max_ips, cursor, window);
}
extern "C" PS_DLL_EXPORT void perfstat_ps_request_stop(void) { ps::ps_request_stop_sampling(); }
extern "C" PS_DLL_EXPORT int perfstat_ps_stop_requested(void) {
    return ps::ps_stop_requested() ? 1 : 0;
}
extern "C" PS_DLL_EXPORT void perfstat_ps_prepare_thread(void) {
    ps::ps_prepare_sampling_thread();
}
extern "C" PS_DLL_EXPORT void perfstat_ps_set_debug(int on) { ps::ps_set_debug(on); }
extern "C" PS_DLL_EXPORT long perfstat_ps_handler_runs(void) {
    return ps::ps_handler_run_count();
}
extern "C" PS_DLL_EXPORT double perfstat_ps_now_seconds(void) { return ps::ps_now_seconds(); }
extern "C" PS_DLL_EXPORT const char *perfstat_ps_platform_name(void) {
    return ps::ps_platform_name();
}
extern "C" PS_DLL_EXPORT uintptr_t perfstat_ps_symbols_open(const char *path, uintptr_t base) {
    return ps::ps_symbols_open(path, base);
}
extern "C" PS_DLL_EXPORT void perfstat_ps_symbols_close(uintptr_t h) { ps::ps_symbols_close(h); }
extern "C" PS_DLL_EXPORT int perfstat_ps_symbols_get(uintptr_t h, int index, uint32_t *rva,
                                                     const char **name) {
    return ps::ps_symbols_get(h, index, rva, name) ? 1 : 0;
}
extern "C" PS_DLL_EXPORT const char *perfstat_ps_symbolize(uintptr_t h, uint32_t rva,
                                                           uint32_t *offset) {
    return ps::ps_symbolize(h, rva, offset);
}

// ABI 校验：自检用它确认自己那边的 platform.h 布局和插件这边一致。
// 布局不一致的话传指针会直接越界，必须先拦住。
extern "C" PS_DLL_EXPORT int perfstat_ps_abi_sizes(int *mod, int *thr, int *mv, int *tv) {
    if (mod) *mod = (int)sizeof(ps::ModuleInfo);
    if (thr) *thr = (int)sizeof(ps::ThreadInfo);
    if (mv) *mv = (int)sizeof(ps::ModuleVisits);
    if (tv) *tv = (int)sizeof(ps::ThreadVisits);
    return (int)sizeof(void *);
}

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

//----------------------------------------------------------------------------------------
// 输出通道探测 —— 【顺序很重要】
//
// 实测结论（L4D2 专用服务器，Windows 和 Linux 都一样）：
//   ICvar::ConsolePrintf 调用成功、返回值正常，但文字【既不上服务器控制台，
//   也不进 console.log】。而 tier0 的 ConMsg 是引擎自己打印控制台信息用的函数
//   （引擎的 echo / 启动日志都走它），所以它一定能把文字送到控制台。
//
// 另一位使用者的反馈也印证了："ICvar::ConsolePrintf 在 L4D2 Linux 服务端上不输出，
// 改用 tier0!ConMsg 就正常了"。
//
// 因此优先级调整为：ConMsg（最可靠） > ConsolePrintf（listen server 上可用） > stdout。
//----------------------------------------------------------------------------------------
void probe_output_channels() {
    if (g_output_probed) return;
    g_output_probed = true;

    // 1) 首选 tier0!ConMsg —— 引擎自己打印控制台就是用它，各平台/各种服务器都可靠。
    //    模块名各平台不同，都试一遍。
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
        g_cvar_for_output = g_pCvar;  // 仍留着，用于注册显示回调等辅助动作
        return;
    }

    // 2) 退而求其次：ICvar::ConsolePrintf（listen server 上有效）
    g_cvar_for_output = g_pCvar;
    if (g_cvar_for_output) {
        g_console_channel = 1;
        return;
    }

    // 3) 兜底：stdout
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



//----------------------------------------------------------------------------------------
// 诊断文件日志
//
// 【为什么需要】在专用服务器（srcds.exe -condebug）上实测：命令能执行、插件在
// plugin_print 里也能看到，但 ICvar::ConsolePrintf 输出的文字【完全不出现在 console.log】。
// 这种情况下没法靠"控制台输出"定位，所以加一条直接写文件的诊断通道：
// 每次输出、走的哪条通道、命令有没有被调用，全部记到 PERFSTAT_DEBUG_LOG 指定的文件。
//
// 用环境变量 PERFSTAT_DEBUG_LOG 控制（值=日志路径）；不设就完全不产生文件，不影响运行。
//----------------------------------------------------------------------------------------
static FILE *g_dbg_file = 0;
static bool g_dbg_inited = false;

static void dbg_init() {
    if (g_dbg_inited) return;
    g_dbg_inited = true;
#if defined(_WIN32)
    char path[1024];
    DWORD n = GetEnvironmentVariableA("PERFSTAT_DEBUG_LOG", path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) return;
    g_dbg_file = fopen(path, "wb");
#else
    const char *path = getenv("PERFSTAT_DEBUG_LOG");
    if (!path || !path[0]) return;
    g_dbg_file = fopen(path, "wb");
#endif
}

// 诊断日志默认关闭（只有设了 PERFSTAT_DEBUG_LOG 才会产生文件）。
// 它记录的是"每条输出走了哪条通道"这类内部细节，属于排障用，日常应保持安静。
void dbg_log(const char *fmt, ...) {
    dbg_init();
    if (!g_dbg_file) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_dbg_file, fmt, ap);
    va_end(ap);
    fputc('\n', g_dbg_file);
    fflush(g_dbg_file);  // 崩溃也要保住已写内容
}


void dbg_log(const char *fmt, ...);

//----------------------------------------------------------------------------------------
// 控制台显示回调
//
// 【为什么要注册它】实测：Windows 专用服务器（srcds.exe）上 ICvar::ConsolePrintf
// 调用成功、返回值正常，但输出【完全不出现在 console.log 和服务器控制台】——
// 插件看起来"没反应"。而 ICvar::InstallConsoleDisplayFunc 是引擎提供的正规扩展点：
// 注册之后，引擎所有控制台输出都会流经我们的回调，我们把它写进日志文件即可。
//
// 这样即使引擎那边 ConsolePrintf 不显示，我们也能拿到完整输出。
//----------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------
// 用户可见的输出文件
//
// 【为什么需要】实测：Windows 专用服务器上 ICvar::ConsolePrintf 调用成功、返回值正常，
// 但文字【完全不进 console.log，也不上服务器控制台】。引擎自己的 echo 能进 console.log，
// 说明它的控制台输出走的是别的路径。
//
// 结论：在专用服务器上"靠引擎控制台显示插件输出"这条路不可靠。
// 所以这里加一个【我们自己的输出文件】：所有 console_out 的文字都追加进去。
// 用户 tail 这个文件就能看到全部输出，完全不受引擎控制台行为影响。
//
// 路径与 perfstat.dll 同目录：addons/perfstat-out.log
//----------------------------------------------------------------------------------------
#if defined(_WIN32)
#define PS_PATH_SEP '\\\\'
#else
#define PS_PATH_SEP '/'
#endif

static FILE *g_out_file = 0;
static bool g_out_inited = false;
static char g_out_path[1200] = {0};

// 插件自己所在的目录（定义在下面，这里先声明，output 文件初始化要用）
std::string module_dir();

// 输出文件放在插件自己所在目录（即 addons/）下的 perfstat-out.log。
// 用 module_dir() 而不是工作目录：模块目录才是"用户放插件的地方"，两个平台一致，
// 也不受服务端启动方式（srcds_run 脚本 / 直接跑 srcds_linux）影响。
static void out_file_init() {
    if (g_out_inited) return;
    g_out_inited = true;

    std::string md = module_dir();
    if (!md.empty()) {
        snprintf(g_out_path, sizeof(g_out_path), "%s%cperfstat-out.log", md.c_str(), PS_PATH_SEP);
        g_out_file = fopen(g_out_path, "ab");
        if (g_out_file) return;
    }
    // 兜底：工作目录
    char cwd[900];
#if defined(_WIN32)
    DWORD n = GetCurrentDirectoryA(sizeof(cwd), cwd);
    if (n == 0 || n >= sizeof(cwd)) return;
#else
    if (!getcwd(cwd, sizeof(cwd))) return;
#endif
    snprintf(g_out_path, sizeof(g_out_path), "%s%cperfstat-out.log", cwd, PS_PATH_SEP);
    g_out_file = fopen(g_out_path, "ab");
}

const char *out_file_path() { return g_out_path; }

void out_file_write(const char *text) {
    out_file_init();
    if (!g_out_file || !text) return;
    fputs(text, g_out_file);
    fflush(g_out_file);
}

class PerfstatDisplayFunc : public IConsoleDisplayFunc {
public:
    virtual void ColorPrint(const void *clr, const char *pMessage) {
        (void)clr;
        Print(pMessage);
    }
    // 注意：【不要】在这里写 out_file。
    // 引擎会把我们 ConsolePrintf 的文字回送到这里，而 console_out 已经直接写过一次了 ——
    // 两边都写会导致每条输出重复两遍（踩过）。
    virtual void Print(const char *pMessage) { (void)pMessage; }
    virtual void DPrint(const char *pMessage) { (void)pMessage; }
};

PerfstatDisplayFunc g_display_func;
bool g_display_installed = false;

void console_out(const char *text) {
    if (g_quiet || !text || !text[0]) return;
    probe_output_channels();

    dbg_log("[console_out] channel=%d (%s) text=%s", g_console_channel, console_channel_name(),
            text);
    // 双保险：不管引擎那边显不显示，用户文件里一定有
    out_file_write(text);

    // 先试着注册显示回调（只成功一次；失败也不影响其他通道）
    if (!g_display_installed && g_cvar_for_output) {
        g_cvar_for_output->InstallConsoleDisplayFunc(&g_display_func);
        g_display_installed = true;
        dbg_log("[console_out] InstallConsoleDisplayFunc -> %p", (void *)&g_display_func);
    }

    if (g_console_channel == 1 && g_cvar_for_output) {
        // ConsolePrintf 是 printf 风格，text 里可能有 % 号，用 %s 转发最安全
        g_cvar_for_output->ConsolePrintf("%s", text);
        return;
    }
    if (g_console_channel == 2 && g_conmsg) {
        g_conmsg("%s", text);
        return;
    }
    // 兜底：部分 Windows 控制台需要 \r\n 才能正确刷新
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

// 插件自己所在的目录（"和插件放在一起"的真正含义）。
//
// 【为什么不能用 exe_dir()】服务端进程是 srcds_linux / srcds.exe，它的目录是游戏根目录，
// 不是 addons/。实测：Linux 上把 perfstat.ini 放进 addons/，插件却跑去根目录找，
// 于是配置永远读不到（横幅里连"配置:"那一行都没有）—— 排查了一轮才定位到。
//
// 用"本模块自身的路径"才准确：
//   Linux  : dladdr(本模块内某个函数) -> dli_fname 即 /path/to/addons/perfstat.so
//   Windows: GetModuleHandleExW(FROM_ADDRESS) 拿本 DLL 句柄，再 GetModuleFileNameW
std::string module_dir() {
#if defined(_WIN32)
    HMODULE self = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(const void *)&module_dir, &self) ||
        !self) {
        return std::string();
    }
    wchar_t wbuf[2048];
    DWORD n = GetModuleFileNameW(self, wbuf, (DWORD)(sizeof(wbuf) / sizeof(wbuf[0]) - 1));
    if (n == 0) return std::string();
    wbuf[n] = 0;
    char buf[4096];
    int m = WideCharToMultiByte(CP_ACP, 0, wbuf, -1, buf, (int)sizeof(buf) - 1, 0, 0);
    if (m <= 0) return std::string();
    buf[m] = 0;
    std::string s(buf);
    size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? std::string() : s.substr(0, p);
#else
    Dl_info info;
    if (!dladdr((void *)(const void *)&module_dir, &info) || !info.dli_fname) {
        return std::string();
    }
    std::string s(info.dli_fname);
    size_t p = s.find_last_of('/');
    return p == std::string::npos ? std::string() : s.substr(0, p);
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
    dbg_log("[sampler] 线程已启动");
    // 第一步：确保本线程能收到采样信号（掩码是继承来的，可能被创建者屏蔽了）
    ps::ps_prepare_sampling_thread();
    dbg_log("[sampler] prepare 完成 stop=%d running=%d profiler=%p",
            (int)ps::ps_stop_requested(), ps::g_profiler ? (int)ps::g_profiler->running() : -1,
            (void *)ps::g_profiler);
    while (!g_thread_stop) {
        if (ps::g_profiler && ps::g_profiler->running()) {
            dbg_log("[sampler] 进入 sampler_loop");
            ps::g_profiler->sampler_loop();
            dbg_log("[sampler] sampler_loop 返回");
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
    dbg_log("[sampler] start_sampler_thread 调用");
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
    bool keep_mapped;   // 卸载后是否刻意让本模块留在内存（默认 1，见下面说明）
    bool cpu_state_filter;  // 只统计"正在占用 CPU"的线程（默认 1）

    Config()
        : sample_ms(10),
          auto_stop_sec(0),
          console_top(25),
          hot_top(8),
          min_hits(2),
          show_hot(false),
          show_threads(false),
          auto_start(true),
          auto_dump_sec(0),
          keep_mapped(true),
          cpu_state_filter(true) {}
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
        else if (!strcasecmp(key, "keep_mapped"))
            g_config.keep_mapped = parse_bool(val, g_config.keep_mapped);
        else if (!strcasecmp(key, "cpu_state_filter"))
            g_config.cpu_state_filter = parse_bool(val, g_config.cpu_state_filter);
        else if (!strcasecmp(key, "auto_dump_sec"))
            g_config.auto_dump_sec = parse_int(val, g_config.auto_dump_sec);
        else if (!strcasecmp(key, "auto_dump_path"))
            g_config.auto_dump_path = val;
    }
    fclose(f);
}

std::string config_path() {
    // 【顺序很重要】插件自己的目录（addons/）优先 —— 插件和配置放一起最省心。
    // 用 module_dir() 而不是 exe_dir()：后者的"exe"是 srcds 进程，位于游戏根目录，
    // 在 Linux 上会导致 addons/perfstat.ini 永远读不到（实测踩过）。
    std::vector<std::string> candidates;
    std::string md = module_dir();
    if (!md.empty()) {
        candidates.push_back(join_path(md, "perfstat.ini"));            // addons/perfstat.ini
        candidates.push_back(join_path(join_path(md, "cfg"), "perfstat.ini"));  // addons/cfg/
    }
    candidates.push_back(join_path(cwd_dir(), "perfstat.ini"));         // 服务器根目录（兼容旧用法）
    candidates.push_back(join_path(exe_dir(), "perfstat.ini"));         // 兜底
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
// ---------------------------------------------------------------------------------------
// 前置声明：ps_cmd_dispatch 用得到（它在下面、早于 namespace ps 的定义处）
// ---------------------------------------------------------------------------------------
namespace ps {
class Profiler;
extern Profiler *g_profiler;
class PerfStatPlugin;
extern PerfStatPlugin g_perfstat;
}

// 供 core.cpp 使用：把诊断日志暴露成 ps 命名空间下的接口
namespace ps {
void dbg_log(const char *fmt, ...);
void dbg_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ::dbg_log("%s", buf);
}
}  // namespace ps

static int ps_seh_filter(unsigned int code) {
    if (code == 0xC0000005 /*ACCESS_VIOLATION*/ || code == 0xC000001D /*ILLEGAL_INSTRUCTION*/ ||
        code == 0xC0000094 /*INT_DIVIDE_BY_ZERO*/ || code == 0xC00000FD /*STACK_OVERFLOW*/) {
        return 1;  // EXCEPTION_EXECUTE_HANDLER
    }
    return 0;  // EXCEPTION_CONTINUE_SEARCH：别的异常交回给引擎
}

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
        // 【必须】置位，否则引擎的 UnregisterConCommand 会因为 IsRegistered()==false
        // 而静默跳过，命令就永远留在引擎命令表里（详见 plugin_api.h 里 SetRegistered 的说明）
        pVar->SetRegistered(true);
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// profiler 实例指针 + 命令回调（放在全局作用域，早于下面的匿名命名空间）
//
// ps_cmd_dispatch 要作为回调传给 ConCommand，必须在命令注册代码之前可见；
// 同时它要用 g_profiler 做空指针防护 —— 卸载后 / 重载中的窗口里直接忽略命令调用，
// 避免访问未就绪的 profiler 状态而崩溃
// （实测：unload 后 reload 再执行 perf_top，崩在 perfstat.dll 内、读地址 0x02）。
//
// g_profiler 的真实定义在下面的 namespace ps 里（因为 ClientCommand 在 ps 内），
// 这里只做声明。
// ---------------------------------------------------------------------------------------
void ps_call_report_to_string(void *prof, void *sink, const void *opt, int *ok) {
#if defined(_MSC_VER)
    __try {
        ((ps::Profiler *)prof)->report(capture_out, sink, *(const ps::Profiler::Options *)opt);
        *ok = 1;
    } __except (ps_seh_filter(GetExceptionCode())) {
        *ok = 0;
    }
#else
    ((ps::Profiler *)prof)->report(capture_out, sink, *(const ps::Profiler::Options *)opt);
    *ok = 1;
#endif
}

// 把真正的调用隔离出来（同样是为了规避 C2712：ps_cmd_dispatch 里有 __try）
static void ps_seh_guarded_dispatch(const CCommand &cmd) {
    ps::g_perfstat.ClientCommand(0, cmd);
}

#if defined(_MSC_VER)
static void ps_cmd_dispatch(const CCommand &cmd) {
    if (!ps::g_profiler) return;  // 卸载后 / 重载中：忽略这次调用
    __try {
        ps_seh_guarded_dispatch(cmd);
    } __except (ps_seh_filter(GetExceptionCode())) {
        // 访问违例被捕获，保证不把游戏带崩；同时打印诊断方便继续定位
        fprintf(stderr,
                "[perfstat] 警告: 执行控制台指令时发生访问违例（已捕获，未崩溃）。"
                "这通常是 unload 后重新 load 的边界状态导致的，建议重新开服以保证状态干净。\n");
        fflush(stderr);
    }
}
#else
// Linux 没有 SEH；posix 下信号采样的访问违例会直接终止进程，这里只做空指针防护
static void ps_cmd_dispatch(const CCommand &cmd) {
    if (!ps::g_profiler) return;
    ps_seh_guarded_dispatch(cmd);
}
#endif

namespace {
PerfstatCVarAccessor g_cvar_accessor;


//----------------------------------------------------------------------------------------
// 【重要】命令指针存在自己的数组里，绝对不要用 SetNext/GetNext 维护"我们自己的链表"。
//
// 原因：ConCommandBase::m_pNext 是【引擎的字段】。ICvar::RegisterConCommand 会用它把
// 命令挂进引擎的全局命令链表（还会改写指针）。如果我们注册前先自己 SetNext 串一遍，
// 引擎随后就会覆盖这条链 —— 于是卸载时按 GetNext() 遍历根本走不全，
// 表现为"部分命令没被反注册、卸载后还能联想/help"（实测踩过）。
//
// 用数组就没这个问题：注册了哪些命令我们自己清楚，遍历完全不依赖引擎怎么改写 m_pNext。
//----------------------------------------------------------------------------------------
static const int kMaxCommands = 64;

//----------------------------------------------------------------------------------------
// 【跨重载复用同一批命令对象】
//
// 为什么不能每次 Load 都 new 一批：卸载时我们刻意【不释放】这些对象（引擎侧可能仍持有
// 指针），而引擎的命令表里可能还留着上一代的命令。如果重载时又 new 一批同名的，
// 引擎里就会同时存在新旧两代同名命令：
//   * 新命令的 m_pszName 指向 DSO 里的字符串字面量（一直有效）
//   * 旧命令的 m_pszHelpString 等指针在重载后可能已经失效
// 引擎遍历/链接这些命令时就会踩到坏指针（实测症状：unload 后 reload，执行
// perf_stat / perf_top / perf_selftest 崩溃，崩溃指令在 perfstat.dll 内部读 0x02）。
//
// 改成"按名字查池子、命中就复用同一个对象"后，整个进程生命周期里每条命令只有一个对象，
// 引擎无论持有哪一代的引用都指向同一个有效对象 —— 从根上消除这类悬空/重复问题。
//
// 注意：命令回调是模块内的静态函数，DSO 因为 keep_mapped 一直映射着，所以老对象调用
// 老回调也没问题；而静态数据是同一个，所以老对象同样能看到重置后的 g_profiler。
//----------------------------------------------------------------------------------------
struct CmdSlot {
    ConCommand *cmd;
    const char *name;  // 指向字符串字面量，生命周期 = DSO 生命周期
};
static CmdSlot g_cmd_pool[kMaxCommands];
ConCommand *g_cmd_list[kMaxCommands];  // 本次 Load 注册的命令（卸载时按它反注册）
int g_cmd_count = 0;

ConCommand *perf_register_command(const char *name, const char *help) {
    // 1) 先查池子：这条命令之前注册过就直接复用（不 new、不改名字）
    ConCommand *cmd = 0;
    for (int i = 0; i < kMaxCommands; ++i) {
        if (g_cmd_pool[i].cmd && g_cmd_pool[i].name &&
            strcmp(g_cmd_pool[i].name, name) == 0) {
            cmd = g_cmd_pool[i].cmd;
            break;
        }
    }
    // 2) 没找到才新建，并放进池子
    if (!cmd) {
        cmd = new ConCommand(name, ps_cmd_dispatch, help, 0);
        for (int i = 0; i < kMaxCommands; ++i) {
            if (!g_cmd_pool[i].cmd) {
                g_cmd_pool[i].cmd = cmd;
                g_cmd_pool[i].name = name;
                break;
            }
        }
    }
    if (g_cmd_count < kMaxCommands) {
        g_cmd_list[g_cmd_count] = cmd;
        g_cmd_count++;
    }
    if (g_pCvar) {
        g_pCvar->RegisterConCommand(cmd);
        // 注册成功后必须置位，否则反注册会被引擎静默忽略（详见 plugin_api.h 的说明）
        cmd->SetRegistered(true);
    }
    return cmd;
}

//----------------------------------------------------------------------------------------
// 反注册控制台指令
//
// 【重要】这里【故意不 delete】命令对象，也刻意留下名字字符串。这是有意为之，不是漏写。
//
// 为什么：Source 引擎的 ConCommandBase 是靠对象自带的 s_pNext 串成链表的，而且
// 客户端输入框在每次按键时都会遍历这条链做补全/高亮。也就是说即使我们调用了
// UnregisterConCommand，引擎侧仍可能持有（或即将再次访问）这些对象的指针/名字。
//
// 实测症状（本地客户端）：plugin_unload 之后不崩，但在输入框里【随便打一个字】立刻崩溃
// —— 因为按键触发补全，补全去读已经被 free 掉的 ConCommand，是 use-after-free。
//
// 另外原来的写法还有第二个 bug：边遍历边 delete，而 next = p->GetNext() 是从
// 【即将/已经释放】的对象里读出来的，本身就不安全。
//
// 所以现在：只反注册、不释放。这点内存（10 条命令，几百字节）就是插件唯一的"泄漏"，
// 而插件卸载后进程通常马上就要结束（换图/关服），代价可以忽略。
// 宁可留着，也不能把悬空指针交给引擎 —— 这是所有引擎插件的通行做法。
//----------------------------------------------------------------------------------------
void perf_unregister_commands() {
    vlog("unregister: begin (%d commands)", g_cmd_count);
    int failed = 0;
    if (g_pCvar) {
        for (int i = 0; i < g_cmd_count; ++i) {
            ConCommandBase *p = g_cmd_list[i];
            if (!p) continue;
            bool was = p->IsRegistered();
            // 顺序很重要：先反注册（此时 IsRegistered() 必须仍是 true，
            // 否则引擎会认为"没注册过"而直接跳过），成功后再把标志清掉。
            vlog("unregister: [%d] '%s' registered=%d -> UnregisterConCommand", i,
                 p->GetName() ? p->GetName() : "?", (int)was);
            g_pCvar->UnregisterConCommand(p);
            p->SetRegistered(false);
            // 反注册后引擎应当能再找到它（FindCommandBase 返回 0 才算真的移除了）
            if (g_pCvar->FindCommandBase(p->GetName()) == p) {
                failed++;
                vlog("unregister: [%d] '%s' 反注册后引擎仍能找到它！", i,
                     p->GetName() ? p->GetName() : "?");
            }
        }
    }
    if (failed > 0) {
        vlog("unregister: 有 %d 条命令反注册后仍留在引擎命令表里", failed);
    } else {
        vlog("unregister: 全部 %d 条命令都已从引擎命令表移除", g_cmd_count);
    }
    vlog("unregister: UnregisterConCommand done");

    // 只把我们这边的记录清掉，让重载时可以重新注册；对象内存【不释放】。
    // （引擎侧可能仍持有这些对象的指针，见上面的说明）
    for (int i = 0; i < g_cmd_count; ++i) g_cmd_list[i] = 0;
    g_cmd_count = 0;
    vlog("unregister: end (命令对象内存刻意保留，避免引擎侧 use-after-free)");
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
// g_profiler 的唯一定义（上面的 ps_cmd_dispatch 通过前置声明使用它）
Profiler *g_profiler = 0;

PerfStatPlugin g_perfstat;

PerfStatPlugin::PerfStatPlugin()
    : m_last_auto_dump(0), m_dumped_once(false) {}

// 采样线程定时触发的自动落盘（在采样线程上下文里执行，注意别做重活）
static void ps_auto_dump_cb(void *, const char *explicit_path) {
    if (!g_profiler) return;
    std::string out;
    g_perfstat.write_report_file(explicit_path ? std::string(explicit_path) : std::string(), out);
}

bool PerfStatPlugin::Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn) {
    g_vlog_t0 = ps::ps_now_seconds();
    vlog("Load: begin");
    // 安装异常诊断（VEH）：万一访问违例，日志会打出当时所处的报告阶段，便于定位
    ps::ps_install_crash_diag();
    ps_platform_init();
    // 应用"只统计运行态线程"设置（默认开；这是让 CPU 占比有意义的关键，详见 platform.h）
    ps_set_cpu_state_filter(g_config.cpu_state_filter ? 1 : 0);
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

    // 自动落盘交给采样线程定时检查（引擎不调用 GameFrame，见 set_auto_dump 注释）
    g_profiler->set_auto_dump(g_config.auto_dump_sec, g_config.auto_dump_path);
    g_profiler->set_auto_dump_cb(ps_auto_dump_cb, 0);

    start_sampler_thread();
    vlog("Load: sampler thread started");

    // 取 ICvar 并注册控制台指令（不注册的话引擎会当成 Unknown command 直接丢掉）
    if (interfaceFactory) {
        g_pCvar = (ICvar *)interfaceFactory(CVAR_INTERFACE_VERSION, 0);
        dbg_log("[Load] QueryInterface('%s') -> %p", CVAR_INTERFACE_VERSION, (void *)g_pCvar);
    } else {
        dbg_log("[Load] interfaceFactory 为空！");
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
    // 专用服务器（srcds.exe）上实测 ICvar::ConsolePrintf 的文字不会出现在 console.log，
    // 所以所有指令输出都会同时写进这个文件 —— 必须在横幅里说清楚，否则用户会以为"没反应"。
    {
        char obuf[1300];
        snprintf(obuf, sizeof(obuf), " 输出: 提示若控制台不显示，请看 %s\n", out_file_path());
        console_out(obuf);
    }
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

    //----------------------------------------------------------------------------------------
    // 最后一步：把本模块"锁"在内存里（见 platform.h 里 ps_keep_module_mapped 的详细说明）。
    //
    // 必须放在最后：前面该清理的都已经清理完了，此时再做"留存"才不影响卸载流程本身。
    //
    // 为什么非做不可：引擎的插件接口【没有】"卸载时通知引擎清理引用"的机制。
    // 一旦模块被 unmapped，引擎侧任何残留指针（ConCommand、名字字符串、链表节点）
    // 被访问就是崩溃。实测症状是 plugin_unload 之后在客户端输入框打一个字就崩，
    // 崩溃点 gameui.dll、读 NULL。
    //
    // 想恢复"真正卸载"（会有上述崩溃风险）就把 perfstat.ini 里 keep_mapped 设成 0。
    //----------------------------------------------------------------------------------------
    if (g_config.keep_mapped) {
        bool kept = ps_keep_module_mapped();
        vlog("Unload: keep_mapped=%d (%s)", (int)kept,
             kept ? "模块保留在内存，避免引擎残留指针引发崩溃"
                  : "失败！引擎会真正卸载本模块，之后可能有悬空指针风险");
    } else {
        vlog("Unload: keep_mapped=0，按配置真正卸载（引擎残留指针可能导致崩溃）");
    }

    console_out("[perfstat] 已卸载\n");
    vlog("Unload: end");
}

void PerfStatPlugin::Pause(void) {
    dbg_log("[callback] Pause");
    if (g_profiler) g_profiler->set_running(false);
}

void PerfStatPlugin::UnPause(void) {
    dbg_log("[callback] UnPause");
    if (g_profiler) g_profiler->set_running(true);
}

const char *PerfStatPlugin::GetPluginDescription(void) {
    return "perfstat v" PERFSTAT_VERSION " - in-process CPU / memory profiler per module";
}

void PerfStatPlugin::GameFrame(bool) {
    // 无条件心跳：每约 5 秒记一次"帧回调还在不在、采样数涨没涨"。
    // 服务端排查用（默认不产生文件，只有设了 PERFSTAT_DEBUG_LOG 才写）。
    {
        static int frames = 0;
        static double last_hb = 0;
        frames++;
        double now_hb = ps::ps_now_seconds();
        if (last_hb == 0) last_hb = now_hb;
        if (now_hb - last_hb >= 5.0) {
            last_hb = now_hb;
            dbg_log("[GameFrame] frames=%d profiler=%p running=%d stop=%d total=%llu",
                    frames, (void *)g_profiler, g_profiler ? (int)g_profiler->running() : -1,
                    (int)ps::ps_stop_requested(),
                    g_profiler ? (unsigned long long)g_profiler->total_samples() : 0ULL);
        }
    }
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

    //----------------------------------------------------------------------------------------
    // 【先生成内容，最后才开文件】
    //
    // 为什么顺序很重要：原来是 fopen -> report(...) -> fwrite -> fclose。
    // 如果 report() 中途出错（访问违例被 ps_cmd_dispatch 的 SEH 捕获），
    // 执行流会直接跳到异常处理器，后面的 fwrite/fclose 永远执行不到 —— 结果是
    // 【日志文件被创建了、内容是空的、而且句柄一直开着】，表现为文件被
    // left4dead2.exe 占用、无法删除（实测踩过）。
    //
    // 改成"先把报告生成到内存字符串，全部成功后再一次性写盘"，就不会再出现
    // "建了空文件还占着" 的情况：要么写成功，要么根本不创建文件。
    //----------------------------------------------------------------------------------------
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

    int ok = 0;
    ps_call_report_to_string(g_profiler, &cap, &opt, &ok);
    if (!ok) {
        delete cap.sink;
        console_out("[perfstat] 生成报告时发生访问违例（已跳过本次落盘）\n");
        return false;  // 注意：此时【还没有创建文件】，不会留下被占用的空文件
    }

    {
        FILE *f = fopen(path.c_str(), "wb");
        if (!f) {
            delete cap.sink;
            char buf[600];
            snprintf(buf, sizeof(buf), "[perfstat] 无法写入日志文件: %s\n", path.c_str());
            console_out(buf);
            return false;
        }
        if (!cap.sink->empty()) fwrite(cap.sink->data(), 1, cap.sink->size(), f);
        fclose(f);  // 这里一定会执行到（上面没有可能抛出的调用）
    }
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
    dbg_log("[ClientCommand] argc=%d arg0='%s' g_profiler=%p g_pCvar=%p", args.ArgC(),
            (args.ArgC() > 0 && args.Arg(0)) ? args.Arg(0) : "?", (void *)g_profiler,
            (void *)g_pCvar);
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
