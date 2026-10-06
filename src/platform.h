//========================================================================================
// perfstat - 平台抽象层接口
//
// 上层（profiler / plugin）只依赖这里的抽象；Windows 与 Linux 的具体实现分别在
// perf_platform_win32.cpp / perf_platform_linux.cpp 中。
//========================================================================================

#ifndef PERFSTAT_PLATFORM_H
#define PERFSTAT_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

namespace ps {

// 一个模块（.dll / .so / 主程序）在内存中的映射信息
struct ModuleInfo {
    uintptr_t base;      // 基址
    size_t size;         // 映射大小（虚拟）
    char path[520];      // 完整路径
    char name[160];      // 文件名（用于显示）
    bool is_main;        // 是否主程序
    size_t mapped_bytes;  // 内存统计：文件映射（等价于磁盘上的 DLL 自身）
    size_t private_bytes; // 内存统计：私有提交（堆/栈/运行时分配）
    size_t other_bytes;   // 内存统计：其它已提交（未驻留等）
};

struct ThreadInfo {
    uint32_t tid;   // 线程 ID（进程内唯一，用它做线程身份即可）
    char name[64];  // 线程名（拿不到时为空）
    int priority;   // 优先级（辅助判断）
};

// 枚举结果的收集容器：平台层直接往里写，core 层再搬运到自己的统计表
struct ModuleVisits {
    ModuleInfo *items;
    int count;
    int capacity;
};

struct ThreadVisits {
    ThreadInfo *items;
    int count;
    int capacity;
};

// ---- 生命周期 ----------------------------------------------------------------
bool ps_platform_init();  // 进程级初始化（装信号处理器等），失败返回 false
void ps_platform_shutdown();

// ---- 枚举 --------------------------------------------------------------------
void ps_enum_modules(ModuleVisits *out);  // 遍历本进程已加载模块
void ps_enum_threads(ThreadVisits *out);  // 遍历本进程线程

// ---- 采样 --------------------------------------------------------------------
// 请求"尽快结束正在进行的这一次采样"。
//
// 为什么需要它：Linux 的采样要逐个线程"投递信号 -> 等回填"，每个线程最多等 20ms。
// 如果某个线程屏蔽了这个信号（宿主进程自己的选择），那一轮就会白等满超时。
// 线程多的时候一轮能到几百毫秒，插件卸载时就会觉得"采样线程怎么都不退"。
// 调用这个函数后，正在进行的采样会在下一个检查点立刻返回（通常在毫秒级），
// 采样线程也就能很快退出，而不是被强行 detach 掉（detach 会导致退出时段错误）。
void ps_request_stop_sampling(void);

// 采样线程应该在采样过程中间或周期性检查它；true 表示该尽快退出。
bool ps_stop_requested(void);

// 打开/关闭平台层的诊断输出（verbose 模式下会往 stderr 打采样细节）。
//
// 注意：这是【平台层自己的】开关，跟插件入口无关 —— 因为离线自检只编译
// perf_platform_*.cpp，不编译 core.cpp；如果平台层直接引用插件里的全局变量，
// 自检就会链接失败（踩过一次）。
void ps_set_debug(int on);

// 诊断用：本副本的信号处理器累计运行次数（Linux 有意义，Windows 恒为 0）
long ps_handler_run_count(void);

//----------------------------------------------------------------------------------------
// "卸载后仍然留在内存里"
//
// 【为什么需要它】plugin_unload 会让引擎把我们的 DLL/SO 从进程里 unmapped。
// 之后只要引擎侧还残留任何一个指向我们内存的指针（ConCommand 对象、名字字符串、
// 命令链表节点……），它一被访问就是"访问未映射内存" → 崩溃。
// 实测症状：plugin_unload 之后不崩，但在客户端输入框里【打一个字】就崩，
// 崩溃点在 gameui.dll，读 NULL（输入框每次按键都会遍历命令表做补全）。
//
// 注意：引擎的插件接口【没有】"卸载时通知引擎清理引用"的机制，
// 所以插件侧唯一可靠的自保办法就是让模块别真的被卸掉。
//
// 做法（不 hook 任何引擎代码，安全可逆）：
//   - Windows: GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) 给本模块 +1 引用，
//              于是引擎随后调用的 FreeLibrary 扣不掉我们，模块保持映射。
//   - Linux:   dlopen(自己) +1 引用计数，dlclose 同理扣不掉。
//
// 代价：卸载后代码/数据仍占几百 KB 常驻内存。插件卸载后进程通常马上要结束
// （换图/关服），这点代价换来的是"不再崩溃"。
//
// ps_keep_module_mapped() 返回 true 表示"已经成功让自己留存"（或本来就已留存）。
//----------------------------------------------------------------------------------------
bool ps_keep_module_mapped();

// 采样线程启动后第一件事就该调用它。
//
// 作用：把采样信号从"本线程"的信号掩码里解开。
// 因为线程会继承创建者的掩码，而创建采样线程的主线程很可能屏蔽了这个信号
// （例如离线自检程序），不解开的话这个采样线程自己就永远采不到东西。
// Linux 上是 pthread_sigmask；Windows 用挂起方式取上下文，这里是空实现。
void ps_prepare_sampling_thread(void);

// 抓取一批线程“当前正在执行的指令地址”，写入 ips[]，线程 ID 写入 tids[]，
// 返回实际抓到的数量。调用者随后自行做归因；平台层不持有 profiler 状态。
int ps_sample_threads(uintptr_t *ips, uint32_t *tids, int max_ips);

// 滑动窗口版：只抓窗口内的线程，cursor 跨轮次推进（线程很多时避免单次停顿过长）
int ps_sample_threads_window(uintptr_t *ips, uint32_t *tids, int max_ips, int *cursor, int window);

// ---- 内存 --------------------------------------------------------------------
// 遍历本进程虚拟地址空间，把每一页按“归属模块”直接累加到传入的模块表里。
// core 提供匹配函数：给定地址返回模块下标，-1 表示不属于任何模块。
typedef int (*ModuleLookupFn)(uintptr_t addr, void *user);
void ps_walk_memory(ModuleLookupFn lookup, void *user, ModuleInfo *mods, int mod_count);

// ---- 符号解析 ----------------------------------------------------------------
// 为某个模块解析导出表/符号表，返回一个不透明句柄（失败返回 0）。
uintptr_t ps_symbols_open(const char *path, uintptr_t base);
// 关闭句柄（插件卸载时调用）
void ps_symbols_close(uintptr_t handle);
// 取第 i 条符号；返回 false 表示越界。name 内存由平台层持有。
bool ps_symbols_get(uintptr_t handle, int index, uint32_t *rva, const char **name);
// 给定模块内偏移，返回最接近的符号名（可能为空）；offset 输出相对该符号的偏移。
const char *ps_symbolize(uintptr_t handle, uint32_t rva, uint32_t *offset);

// ---- 杂项 --------------------------------------------------------------------
double ps_now_seconds();  // 单调时钟（秒）
void ps_get_local_time(int *y, int *mo, int *d, int *h, int *mi, int *s);
const char *ps_platform_name();  // "win32" / "linux"

}  // namespace ps

#endif  // PERFSTAT_PLATFORM_H
