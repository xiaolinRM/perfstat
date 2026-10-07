//========================================================================================
// perfstat - 核心逻辑（与平台无关）
//   * 模块（DLL/SO）表维护
//   * 采样归因：把采样到的指令地址 -> (模块, 模块内偏移)
//   * 热点统计：每个模块内部的 Top N 偏移
//   * 报告生成（控制台 / 日志文件）
//========================================================================================

#ifndef PERFSTAT_CORE_H
#define PERFSTAT_CORE_H

#include <stdint.h>
#include <stdio.h>

#include <deque>
#include <string>
#include <vector>

#include "compat.h"
#include "platform.h"

namespace ps {

// 调试开关（定义在 core.cpp，插件和离线自检都会链接它）
extern "C" int g_perfstat_verbose_flag;

// 统一诊断时间戳（毫秒，进程启动起算）。定义在 perfstat.cpp（插件入口）。
//
// 为什么需要它：CI 日志里 stdout 是块缓冲（进程退出才刷）、stderr 无缓冲，
// 两者交错后【行号完全不能代表时间顺序】—— 这个项目里因为误读顺序浪费过好几轮。
// 有了时间戳，先后就不用猜了。
extern "C" double ps_dbg_now_ms(void);

// 轻量自旋锁：采样线程写、主线程读，冲突极少
//
// 【诊断能力】自旋锁最大的风险是"某一边忘了放锁"或"持锁做了慢操作"，
// 表现是另一个线程永久卡住、而且不报任何错（这个项目里踩过不止一次）。
// 所以这里带着持有者记录：一旦有线程等待超过 50ms，就把"谁握着锁"打到 stderr。
class SpinLock {
public:
    SpinLock() : m_flag(0), m_owner("") {}
    void lock() {
        if (PS_ATOMIC_SET(&m_flag, 1) == 0) return;
        // 有人在等 —— 记录等待者，并在等太久时把持有者报出来
        m_waiter = m_owner;
        int spins = 0;
        while (PS_ATOMIC_SET(&m_flag, 1)) {
            while (PS_ATOMIC_GET(&m_flag)) {
                if (++spins == 2000000) {
                    // 大致对应几十毫秒。只报一次，避免刷屏。
                    static volatile int reported = 0;
                    if (g_perfstat_verbose_flag && !reported) {
                        reported = 1;
                        fprintf(stderr,
                                "[perfstat-hb] LOCK STUCK: owner='%s' waiter='%s' (waited ~%dM spins)\n",
                                m_owner, m_waiter, spins / 1000000);
                    }
                }
            }
        }
    }
    void unlock() {
        m_owner = "";
        PS_ATOMIC_SET(&m_flag, 0);
    }
    bool try_lock() {
        if (PS_ATOMIC_SET(&m_flag, 1) == 0) return true;
        return false;
    }
    // 调用者加锁成功后标记"我是谁"（AutoLock 会调用）
    void set_owner(const char *who) { m_owner = who; }
    const char *owner() const { return m_owner; }
    const char *waiter() const { return m_waiter; }

private:
    volatile long m_flag;
    const char *m_owner;
    const char *m_waiter;
};

class AutoLock {
public:
    explicit AutoLock(SpinLock &l, const char *who = "?") : m_l(l) {
        m_l.lock();
        m_l.set_owner(who);
    }
    ~AutoLock() { m_l.unlock(); }

private:
    SpinLock &m_l;
};

// 某个模块内部的一个热点（模块内偏移 + 采样数）
struct HotSpot {
    uint32_t rva;
    uint64_t hits;
};

struct ModuleStat {
    // 静态信息
    std::string path;
    std::string name;
    uintptr_t base;
    size_t size;
    bool is_main;
    uintptr_t sym_handle;

    // 运行期统计
    uint64_t hits;  // 该模块命中的采样数
    // 加权命中：按线程在两次采样之间真实消耗的 CPU 时间累加。
    // CPU% 用 weight_ns / total_weight_ns 算（见 apply_sample 的注释）。
    unsigned long long weight_ns;
    uint64_t hot_capped;
    std::vector<HotSpot> hot_list;  // 每个不同偏移一条（偏移->命中数，线性查找 + 上限）

    // 内存统计（由平台层填充）
    size_t mem_mapped;
    size_t mem_private;
    size_t mem_other;

    ModuleStat()
        : base(0), size(0), is_main(false), sym_handle(0), hits(0), weight_ns(0), hot_capped(0),
          mem_mapped(0), mem_private(0), mem_other(0) {}
};

struct ThreadStat {
    uint32_t tid;
    std::string name;
    uint64_t hits;
    // 该线程的【加权 CPU 时间】（ns 累计，按采样时测到的真实 CPU 增量累加）。
    // 用它做 CPU% 的分母，才不会让"大量空闲线程把系统库抬到 98%"。
    unsigned long long cpu_ns;
    ThreadStat() : tid(0), hits(0), cpu_ns(0) {}
};

// 输出目的地：控制台（由插件提供）或日志文件
typedef void (*OutFn)(void *user, const char *text);

class Profiler {
public:
    Profiler();

    // ---- 控制 ----
    void set_sample_interval_ms(int ms);
    int sample_interval_ms() const { return m_interval_ms; }
    void set_running(bool run);
    bool running() const { return m_running; }
    void reset();  // 清空所有统计（保留配置）
    bool inited() const { return m_inited; }
    void init_once();  // 首次采样前建立模块/线程基线

    // 自动停止（0 表示不自动停止）
    void set_auto_stop_sec(int sec) { m_auto_stop_sec = sec; }
    int auto_stop_sec() const { return m_auto_stop_sec; }
    bool should_auto_stop() const {
        return m_running && m_auto_stop_sec > 0 && elapsed_seconds() >= (double)m_auto_stop_sec;
    }

    // ---- 采样线程主循环（由 plugin 创建线程调用，直到 stop 返回）----
    // 内部会周期性检查 ps_stop_requested()，保证插件卸载时能迅速退出。
    void sampler_loop();

    // ---- 采样数据 ----
    uint64_t total_samples() const { return m_total_samples; }
    int thread_count() const { return (int)m_threads.size(); }
    int module_count() const { return (int)m_modules.size(); }
    double elapsed_seconds() const;
    long long sample_errors() const { return m_sample_errors; }

    // ---- 报告 ----
    struct Options {
        int top;           // 每个模块列出前几个热点
        bool show_hot;     // 是否展开热点
        bool show_threads; // 是否列出线程明细
        bool no_memory;    // true = 跳过内存遍历（大进程上遍历会停顿几百毫秒）
        int min_hits;      // 热点显示门槛
        const char *title; // 报告标题（可为空）
    };
    void report(OutFn out, void *user, const Options &opt);

    // 供平台层内存遍历使用
    int find_module_by_addr(uintptr_t addr) const;

    // 把一个采样到的指令地址计入统计（采样线程调用）
    // weight_ns = 该线程自上次采样以来真实消耗的 CPU 时间（ns）。
    // 见 platform.h / ps_thread_cpu_time_ns 的说明：用它做加权归因，
    // 空闲线程权重≈0，不会再把 ntdll/libc 抬到 90%+。
    void apply_sample(uintptr_t ip, uint32_t tid, unsigned long long weight_ns = 0);

private:
    struct Snapshot {
        struct Row {
            std::string name;
            std::string path;
            uintptr_t base;
            size_t size;
            bool is_main;
            uint64_t hits;
            // 加权命中（按线程真实 CPU 时间累计），CPU% 用它算
            unsigned long long weight_ns;
            size_t mem_mapped, mem_private, mem_other;
            std::vector<HotSpot> hot;
            uint64_t hot_capped;
            uintptr_t sym_handle;
        };
        std::vector<Row> rows;
        std::vector<ThreadStat> threads;
        uint64_t total;
        unsigned long long total_weight_ns;  // 所有样本的权重之和
        long long errors;
        double start, last;
        bool running;
        int interval_ms;
        int module_entries;
    };
    void take_snapshot(Snapshot &s);
    void emit(const char *fmt, ...);
    void refresh_modules_and_threads();
    void collect_memory_locked();  // 调用前必须已经持有 m_lock

public:
    // 诊断用：当前握着统计锁的是谁（空串=没人持有）
    const char *lock_holder() const { return m_lock.owner(); }
    const char *lock_waiter() const { return m_lock.waiter(); }

private:

    std::deque<ModuleStat> m_modules;
    std::vector<ThreadStat> m_threads;
    mutable SpinLock m_lock;
    uintptr_t *m_ip_buffer;
    uint32_t *m_tid_buffer;
    int m_ip_capacity;

    uint64_t m_total_samples;
    unsigned long long m_total_weight_ns;

    // 每个线程"上次读到的累计 CPU 时间"，用来算增量（见 sampler_loop 里的说明）
    struct CpuLast {
        uint32_t tid;
        unsigned long long ns;
    };
    std::vector<CpuLast> m_cpu_last;
    long long m_sample_errors;
    double m_start_time;
    double m_last_time;
    volatile bool m_running;
    bool m_inited;
    int m_interval_ms;
    int m_auto_stop_sec;

    OutFn m_out;
    void *m_out_user;
};

// 平台层 -> core 的数据通道定型在 platform.h（ModuleVisits / ThreadVisits）

}  // namespace ps

#endif  // PERFSTAT_CORE_H
