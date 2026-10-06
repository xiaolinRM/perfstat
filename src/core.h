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

// 轻量自旋锁：采样线程写、主线程读，冲突极少
class SpinLock {
public:
    SpinLock() : m_flag(0) {}
    void lock() {
        while (PS_ATOMIC_SET(&m_flag, 1)) {
            while (PS_ATOMIC_GET(&m_flag)) {
            }
        }
    }
    void unlock() { PS_ATOMIC_SET(&m_flag, 0); }
    bool try_lock() { return PS_ATOMIC_SET(&m_flag, 1) == 0; }

private:
    volatile long m_flag;
};

class AutoLock {
public:
    explicit AutoLock(SpinLock &l) : m_l(l) { m_l.lock(); }
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
    uint64_t hot_capped;
    std::vector<HotSpot> hot_list;  // 每个不同偏移一条（偏移->命中数，线性查找 + 上限）

    // 内存统计（由平台层填充）
    size_t mem_mapped;
    size_t mem_private;
    size_t mem_other;

    ModuleStat()
        : base(0), size(0), is_main(false), sym_handle(0), hits(0), hot_capped(0),
          mem_mapped(0), mem_private(0), mem_other(0) {}
};

struct ThreadStat {
    uint32_t tid;
    std::string name;
    uint64_t hits;
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
    void apply_sample(uintptr_t ip, uint32_t tid);

private:
    struct Snapshot {
        struct Row {
            std::string name;
            std::string path;
            uintptr_t base;
            size_t size;
            bool is_main;
            uint64_t hits;
            size_t mem_mapped, mem_private, mem_other;
            std::vector<HotSpot> hot;
            uint64_t hot_capped;
            uintptr_t sym_handle;
        };
        std::vector<Row> rows;
        std::vector<ThreadStat> threads;
        uint64_t total;
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

    std::deque<ModuleStat> m_modules;
    std::vector<ThreadStat> m_threads;
    mutable SpinLock m_lock;
    uintptr_t *m_ip_buffer;
    uint32_t *m_tid_buffer;
    int m_ip_capacity;

    uint64_t m_total_samples;
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
