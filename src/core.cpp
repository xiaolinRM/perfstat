//========================================================================================
// perfstat - 核心逻辑实现
//========================================================================================

#include "core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>

namespace ps {

// 每个模块最多记录多少个不同的热点偏移；超出后只累加 hot_capped
static const size_t kMaxHotPerModule = 4096;

//----------------------------------------------------------------------------------------
// 调试/诊断开关：打开后采样线程会往 stderr 打低频心跳，方便定位"采不到样本"这类问题。
//
// 这个定义放在 core.cpp 而不是插件入口，是因为离线自检会直接编译 core.cpp
// （它需要在没有引擎的情况下驱动 profiler）。如果定义放插件里，自检就会链接失败。
// 平台层用 extern "C" int g_perfstat_verbose_flag; 引用它。
//----------------------------------------------------------------------------------------
// 先声明成 C 链接（符号名不能被 C++ 修饰），再用普通定义 ——
// 直接写 `extern "C" int x = 0;` 会被 gcc 警告 "initialized and declared extern"。
extern "C" int g_perfstat_verbose_flag;

namespace {
inline bool verbose_on() { return g_perfstat_verbose_flag != 0; }
}

int g_perfstat_verbose_flag = 0;

Profiler::Profiler()
    : m_ip_buffer(0),
      m_tid_buffer(0),
      m_ip_capacity(0),
      m_total_samples(0),
      m_sample_errors(0),
      m_start_time(0),
      m_last_time(0),
      m_running(false),
      m_inited(false),
      m_interval_ms(10),
      m_auto_stop_sec(0),
      m_out(0),
      m_out_user(0) {}

void Profiler::set_sample_interval_ms(int ms) {
    if (ms < 1) ms = 1;
    if (ms > 1000) ms = 1000;
    m_interval_ms = ms;
}

double Profiler::elapsed_seconds() const {
    if (m_start_time <= 0) return 0.0;
    double end = m_running ? ps_now_seconds() : m_last_time;
    double d = end - m_start_time;
    return d > 0 ? d : 0.0;
}

void Profiler::refresh_modules_and_threads() {
    static const int kMaxModules = 1024;
    static const int kMaxThreads = 2048;
    static ModuleInfo module_buf[kMaxModules];
    static ThreadInfo thread_buf[kMaxThreads];

    ModuleVisits mv;
    mv.items = module_buf;
    mv.count = 0;
    mv.capacity = kMaxModules;
    ThreadVisits tv;
    tv.items = thread_buf;
    tv.count = 0;
    tv.capacity = kMaxThreads;

    ps_enum_modules(&mv);
    ps_enum_threads(&tv);

    AutoLock lk(m_lock, "refresh_threads");
    m_modules.clear();
    for (int i = 0; i < mv.count; ++i) {
        ModuleStat m;
        m.base = module_buf[i].base;
        m.size = module_buf[i].size;
        m.path = module_buf[i].path;
        m.name = module_buf[i].name;
        m.is_main = module_buf[i].is_main;
        m_modules.push_back(m);
    }
    std::stable_sort(m_modules.begin(), m_modules.end(),
                     [](const ModuleStat &a, const ModuleStat &b) { return a.base < b.base; });

    m_threads.clear();
    for (int i = 0; i < tv.count; ++i) {
        ThreadStat t;
        t.tid = thread_buf[i].tid;
        t.name = thread_buf[i].name;
        t.hits = 0;
        m_threads.push_back(t);
    }
}

void Profiler::init_once() {
    if (m_inited) return;
    m_inited = true;
    refresh_modules_and_threads();
    m_ip_capacity = (int)m_threads.size() + 32;
    if (m_ip_capacity < 64) m_ip_capacity = 64;
    if (m_ip_capacity > 4096) m_ip_capacity = 4096;
    m_ip_buffer = new uintptr_t[m_ip_capacity];
    m_tid_buffer = new uint32_t[m_ip_capacity];
    memset(m_tid_buffer, 0, sizeof(uint32_t) * m_ip_capacity);
}

void Profiler::set_running(bool run) {
    if (run == m_running) return;
    if (run) {
        init_once();
        // 刷新模块与线程列表：采样期间新加载的 DLL / 新创建的线程也要能看到
        refresh_modules_and_threads();
        m_start_time = ps_now_seconds();
        m_last_time = m_start_time;
        m_total_samples = 0;
        m_sample_errors = 0;
        m_running = true;
    } else {
        m_running = false;
        m_last_time = ps_now_seconds();
    }
}

void Profiler::reset() {
    bool was_running = m_running;
    if (was_running) set_running(false);
    {
        AutoLock lk(m_lock, "refresh_modules");
        for (size_t i = 0; i < m_modules.size(); ++i) {
            ModuleStat &m = m_modules[i];
            m.hits = 0;
            m.hot_capped = 0;
            m.hot_list.clear();
            if (m.sym_handle) {
                ps_symbols_close(m.sym_handle);
                m.sym_handle = 0;
            }
        }
        for (size_t i = 0; i < m_threads.size(); ++i) m_threads[i].hits = 0;
        m_total_samples = 0;
        m_sample_errors = 0;
        m_start_time = 0;
        m_last_time = 0;
    }
    if (was_running) set_running(true);
}

//----------------------------------------------------------------------------------------
// 平台层回调不再走函数指针，模块/线程枚举直接写入定长缓冲（见 refresh_modules_and_threads）
//----------------------------------------------------------------------------------------

//----------------------------------------------------------------------------------------
// 采样
//----------------------------------------------------------------------------------------
int Profiler::find_module_by_addr(uintptr_t addr) const {
    // 基址升序，二分找最后一个 base <= addr
    int lo = 0, hi = (int)m_modules.size() - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m_modules[mid].base <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0) return -1;
    const ModuleStat &m = m_modules[best];
    if (addr < m.base + m.size) return best;
    return -1;
}

void Profiler::apply_sample(uintptr_t ip, uint32_t tid) {
    // 诊断：只在 verbose 下打点，而且每 100000 次才打一行（性能影响可忽略）。
    // 用来确认"采样回来的 IP 到底有没有进到统计里"。
    if (verbose_on()) {
        static unsigned long long dbg_apply_calls = 0;
        static unsigned long long dbg_apply_ip0 = 0;
        dbg_apply_calls++;
        if (!ip) dbg_apply_ip0++;
        if (dbg_apply_calls == 1 || dbg_apply_calls % 100000 == 0) {
            fprintf(stderr, "[perfstat-hb] apply_sample: calls=%llu ip_zero=%llu\n", dbg_apply_calls,
                    dbg_apply_ip0);
        }
    }

    AutoLock lk(m_lock, "apply_sample");
    if (!ip) {
        m_sample_errors++;
        return;
    }
    if (tid) {
        bool found = false;
        for (size_t i = 0; i < m_threads.size(); ++i) {
            if (m_threads[i].tid == tid) {
                m_threads[i].hits++;
                found = true;
                break;
            }
        }
        if (!found) {
            // 采样期间新创建的线程：补进列表，这样线程明细不会漏
            ThreadStat t;
            t.tid = tid;
            t.hits = 1;
            m_threads.push_back(t);
        }
    }
    int idx = find_module_by_addr(ip);
    m_total_samples++;
    if (idx < 0) return;  // 落在模块之外（例如 JIT / 已卸载模块）
    ModuleStat &m = m_modules[idx];
    m.hits++;
    uint32_t rva = (uint32_t)(ip - m.base);
    // 热点直方图：先线性找，找不到则追加（有上限）
    for (size_t k = 0; k < m.hot_list.size(); ++k) {
        if (m.hot_list[k].rva == rva) {
            m.hot_list[k].hits++;
            return;
        }
    }
    if (m.hot_list.size() < kMaxHotPerModule) {
        HotSpot h;
        h.rva = rva;
        h.hits = 1;
        m.hot_list.push_back(h);
    } else {
        m.hot_capped++;
    }
}

// 每轮最多实际挂起多少个线程。线程很多（几百个）时，一轮全抓会让单次停顿变长，
// 所以这里做“滑动窗口”：每轮只抓 window 个线程，轮转覆盖，整体节拍仍按 interval_ms。
static const int kSampleWindow = 24;

// 由 perfstat.cpp 导出：离线自检打开 verbose 时置 1
extern "C" int g_perfstat_verbose_flag;

void Profiler::sampler_loop() {
    int cursor = 0;
    int dbg_round = 0;
    if (verbose_on()) {
        fprintf(stderr, "[perfstat-hb] sampler_loop enter: running=%d cap=%d threads=%d modules=%d\n",
                (int)m_running, m_ip_capacity, (int)m_threads.size(), (int)m_modules.size());
    }
    // 每轮都检查 ps_stop_requested()：插件卸载时会请求停止，
    // 这样采样线程能在毫秒级退出，而不是把当前这一轮跑完（可能几百毫秒）。
    while (m_running && !ps_stop_requested()) {
        double t0 = ps_now_seconds();

        double t_s0 = ps_now_seconds();
        int n = ps_sample_threads_window(m_ip_buffer, m_tid_buffer, m_ip_capacity, &cursor,
                                         kSampleWindow);
        double t_s1 = ps_now_seconds();
        // 单次采样耗时异常必须报出来（这里【不依赖 verbose】）：
        // 正常应该只有毫秒级，一旦到了百毫秒以上就是严重问题，
        // 否则只会看到"某一轮之后就没动静了"，很容易误判成采样线程挂了。
        if ((t_s1 - t_s0) > 0.2) {
            fprintf(stderr, "[perfstat-hb] SLOW 采样窗口: %.0f ms (n=%d, 线程=%d)\n",
                    (t_s1 - t_s0) * 1000.0, n, (int)m_threads.size());
        }
        // 低频心跳：只在 verbose 下每 100 轮打一行。
        // 这条日志能把"采样线程根本没进循环"和"进了循环但一个样本都拿不到"区分开。
        if (verbose_on() && (dbg_round < 2 || dbg_round % 500 == 0)) {
            fprintf(stderr,
                    "[perfstat-hb] sampler round %d: n=%d total=%llu running=%d threads=%d\n",
                    dbg_round, n, (unsigned long long)m_total_samples, (int)m_running,
                    (int)m_threads.size());
        }
        dbg_round++;
        for (int i = 0; i < n; ++i) apply_sample(m_ip_buffer[i], m_tid_buffer[i]);

        double t1 = ps_now_seconds();
        double used = t1 - t0;
        double want = m_interval_ms / 1000.0;
        if (used < want) {
            ps_sleep_ms((int)((want - used) * 1000.0 + 0.5));
        }
    }
}

//----------------------------------------------------------------------------------------
// 快照
//----------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------
// 内存归因
//----------------------------------------------------------------------------------------
namespace {

// 内存遍历用的上下文：模块基址快照 + 数量。
// 回调在放锁状态下运行，所以这里用完全独立的一份数据，不碰 Profiler 的任何成员。
struct MemWalkCtx {
    ModuleInfo *mods;  // 输入：模块快照（按基址升序）
    int count;         // 输入：模块数量
};

int mem_lookup(uintptr_t addr, void *user) {
    MemWalkCtx *ctx = (MemWalkCtx *)user;
    int best = -1;
    for (int i = 0; i < ctx->count; ++i) {
        if (ctx->mods[i].base <= addr) {
            best = i;
        } else {
            break;  // 已按基址升序
        }
    }
    if (best < 0) return -1;
    if (addr < ctx->mods[best].base + ctx->mods[best].size) return best;
    return -1;
}

}  // namespace

void Profiler::collect_memory_locked() {
    // 调用本函数时【已经持有】m_lock（名字里的 locked 就是这个意思）。
    //
    // 【关键：慢操作必须放锁】ps_walk_memory 在 Linux 上要读 /proc/self/smaps
    // 并遍历全部映射段，大进程上可能耗时数百毫秒到数秒。如果持着 m_lock 做这件事，
    // 采样线程就会在 apply_sample() 里一直等锁 —— 表现是"采样线程在跑、但样本数不涨"，
    // 而且不报任何错（踩过：自检里 total_samples 一直是 0、apply_sample 从未被调用）。
    //
    // 所以流程是：持锁取一份模块表快照 -> 放锁 -> 遍历内存 -> 再持锁合并结果。
    static ModuleInfo snapshot[1024];
    static ModuleInfo result[1024];

    if (m_modules.empty()) return;
    if ((int)m_modules.size() > 1024) return;
    const int count = (int)m_modules.size();

    for (int i = 0; i < count; ++i) {
        // 快照：回调里绝对不能再碰 Profiler（更不能碰锁），所以用独立的一份数据
        snapshot[i] = ModuleInfo();
        snapshot[i].base = m_modules[i].base;
        snapshot[i].size = m_modules[i].size;
        snapshot[i].is_main = m_modules[i].is_main;

        result[i] = ModuleInfo();
        result[i].base = m_modules[i].base;
        result[i].size = m_modules[i].size;
    }

    MemWalkCtx ctx;
    ctx.mods = snapshot;
    ctx.count = count;

    m_lock.unlock();  // ---- 放锁，做慢操作 ----
    ps_walk_memory(&mem_lookup, &ctx, result, count);
    m_lock.lock();
    m_lock.set_owner("report/collect_memory");  // ---- 重新加锁，合并结果 ----

    for (int i = 0; i < count && i < (int)m_modules.size(); ++i) {
        m_modules[i].mem_mapped = result[i].mapped_bytes;
        m_modules[i].mem_private = result[i].private_bytes;
        m_modules[i].mem_other = result[i].other_bytes;
    }
}

void Profiler::take_snapshot(Snapshot &s) {
    AutoLock lk(m_lock, "take_snapshot");

    s.rows.clear();
    s.rows.reserve(m_modules.size());
    for (size_t i = 0; i < m_modules.size(); ++i) {
        const ModuleStat &m = m_modules[i];
        Snapshot::Row r;
        r.name = m.name;
        r.path = m.path;
        r.base = m.base;
        r.size = m.size;
        r.is_main = m.is_main;
        r.hits = m.hits;
        r.mem_mapped = m.mem_mapped;
        r.mem_private = m.mem_private;
        r.mem_other = m.mem_other;
        r.hot_capped = m.hot_capped;
        r.sym_handle = m.sym_handle;
        r.hot = m.hot_list;
        s.rows.push_back(r);
    }
    s.threads = m_threads;
    s.total = m_total_samples;
    s.errors = m_sample_errors;
    s.start = m_start_time;
    s.last = m_last_time;
    s.running = m_running;
    s.interval_ms = m_interval_ms;
    s.module_entries = (int)m_modules.size();
}

//----------------------------------------------------------------------------------------
// 报告
//----------------------------------------------------------------------------------------
void Profiler::emit(const char *fmt, ...) {
    if (!m_out) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_out(m_out_user, buf);
}

static void make_short(const std::string &in, char *out, size_t out_size) {
    size_t n = in.size();
    if (n >= out_size) n = out_size - 1;
    memcpy(out, in.c_str(), n);
    out[n] = 0;
}

void Profiler::report(OutFn out, void *user, const Options &opt) {
    // 注意顺序：必须先收集内存，再取快照。
    // 之前是先 take_snapshot() 再 collect_memory_locked()，于是内存数据永远晚一拍
    // 才写进快照，报告里那一列永远是 0。
    if (!opt.no_memory) {
        AutoLock lk(m_lock, "report/collect_memory");
        collect_memory_locked();
    }
    Snapshot s;
    take_snapshot(s);

    m_out = out;
    m_out_user = user;

    uint64_t total = s.total;
    double elapsed = 0.0;
    if (s.start > 0) {
        double end = s.running ? ps_now_seconds() : s.last;
        elapsed = end - s.start;
    }
    if (elapsed < 0.0001) elapsed = 0.0001;

    // ---- 排序：CPU ----
    std::vector<Snapshot::Row *> by_cpu;
    by_cpu.reserve(s.rows.size());
    for (size_t i = 0; i < s.rows.size(); ++i) by_cpu.push_back(&s.rows[i]);
    std::stable_sort(by_cpu.begin(), by_cpu.end(),
                     [](Snapshot::Row *a, Snapshot::Row *b) { return a->hits > b->hits; });

    // ---- 排序：内存 ----
    std::vector<Snapshot::Row *> by_mem;
    by_mem.reserve(s.rows.size());
    for (size_t i = 0; i < s.rows.size(); ++i) by_mem.push_back(&s.rows[i]);
    std::stable_sort(by_mem.begin(), by_mem.end(), [](Snapshot::Row *a, Snapshot::Row *b) {
        return (a->mem_mapped + a->mem_private + a->mem_other) >
               (b->mem_mapped + b->mem_private + b->mem_other);
    });
    std::vector<int> mem_rank(s.rows.size(), 0);
    for (size_t i = 0; i < by_mem.size(); ++i) {
        size_t idx = (size_t)(by_mem[i] - &s.rows[0]);
        mem_rank[idx] = (int)i + 1;
    }

    size_t total_mapped = 0, total_private = 0, total_other = 0;
    for (size_t i = 0; i < s.rows.size(); ++i) {
        total_mapped += s.rows[i].mem_mapped;
        total_private += s.rows[i].mem_private;
        total_other += s.rows[i].mem_other;
    }

    emit("================================================================================\n");
    if (opt.title && opt.title[0]) {
        emit(" perfstat  %s\n", opt.title);
    } else {
        emit(" perfstat  服务器性能分析报告\n");
    }
    emit("--------------------------------------------------------------------------------\n");
    int y, mo, d, h, mi, sec;
    ps_get_local_time(&y, &mo, &d, &h, &mi, &sec);
    emit(" 时间         : %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, sec);
    emit(" 平台         : %s   (采样方式: 逐线程挂起取指令指针)\n", ps_platform_name());
    emit(" 采样间隔     : %d ms     采样次数: %llu     采样漏掉/无效: %lld\n", s.interval_ms,
         (unsigned long long)total, s.errors);
    emit(" 统计时长     : %.1f 秒   实际采样频率: %.1f 次/秒\n", elapsed,
         total > 0 ? (double)total / elapsed : 0.0);
    emit(" 模块数量     : %d        线程数量: %d\n", s.module_entries, (int)s.threads.size());
    emit(" 说明         : 采样是【统计抽样】，不是精确计时；时长越长、间隔越小越准。\n");
    emit("                百分比 = 该模块命中采样数 / 总采样数（≈ 占满一个 CPU 核心的比例）。\n");
    emit("                未命中任何模块的采样不参与百分比，因此各列之和会略小于 100%%。\n");
    emit("--------------------------------------------------------------------------------\n");

    // ---- 表头 ----
    emit(" #  CPU%%   MODULE                          MEM(MB)  mem排序   内存构成\n");
    emit("--------------------------------------------------------------------------------\n");

    char namebuf[64];
    for (size_t i = 0; i < by_cpu.size(); ++i) {
        Snapshot::Row &r = *by_cpu[i];
        size_t mem_total = r.mem_mapped + r.mem_private + r.mem_other;
        if (r.hits == 0 && mem_total == 0) continue;  // 完全没数据的模块不显示
        make_short(r.name, namebuf, 32);
        double pct = total > 0 ? (double)r.hits * 100.0 / (double)total : 0.0;
        emit("%2d %5.2f  %-32s %8.2f  #%-4d map %.1f / priv %.1f / other %.1f\n", (int)i + 1, pct,
             namebuf, (double)mem_total / (1024.0 * 1024.0), mem_rank[(int)i],
             (double)r.mem_mapped / (1024.0 * 1024.0), (double)r.mem_private / (1024.0 * 1024.0),
             (double)r.mem_other / (1024.0 * 1024.0));
    }

    emit("--------------------------------------------------------------------------------\n");
    emit(" 内存合计     : 文件映射 %.1f MB + 私有提交 %.1f MB + 其它 %.1f MB = %.1f MB\n",
         (double)total_mapped / (1024.0 * 1024.0), (double)total_private / (1024.0 * 1024.0),
         (double)total_other / (1024.0 * 1024.0),
         (double)(total_mapped + total_private + total_other) / (1024.0 * 1024.0));
    emit(" 内存列含义   : map=DLL/SO 文件本身占的驻留内存; priv=进程私有提交(堆/栈/运行时分配),\n");
    emit("                注意 priv 是【按地址区间归属】的估算, 引擎的全局分配器都在主程序名下。\n");

    // ---- 热点展开 ----
    if (opt.show_hot) {
        emit("================================================================================\n");
        emit(" 热点明细（每个模块内部最热的函数/偏移，Top %d，至少 %d 次采样）\n", opt.top,
             opt.min_hits);
        emit("--------------------------------------------------------------------------------\n");
        int shown_modules = 0;
        for (size_t i = 0; i < by_cpu.size() && shown_modules < 12; ++i) {
            Snapshot::Row &r = *by_cpu[i];
            if (r.hits == 0) continue;
            shown_modules++;
            emit("\n[%s]  基址=%p  大小=%.1f MB  CPU=%.2f%%  采样=%llu\n",
                 r.path.empty() ? r.name.c_str() : r.path.c_str(), (void *)r.base,
                 (double)r.size / (1024.0 * 1024.0),
                 total > 0 ? (double)r.hits * 100.0 / (double)total : 0.0,
                 (unsigned long long)r.hits);

            std::vector<HotSpot> hot = r.hot;
            std::stable_sort(hot.begin(), hot.end(),
                             [](const HotSpot &a, const HotSpot &b) { return a.hits > b.hits; });
            int printed = 0;
            for (size_t k = 0; k < hot.size() && printed < opt.top; ++k) {
                if ((int)hot[k].hits < opt.min_hits) break;
                uint32_t off = 0;
                const char *sym = r.sym_handle ? ps_symbolize(r.sym_handle, hot[k].rva, &off) : 0;
                double pct_of_module =
                    r.hits > 0 ? (double)hot[k].hits * 100.0 / (double)r.hits : 0.0;
                if (sym && sym[0]) {
                    emit("    %6.2f%% of module   %-56s %s+0x%X (rva 0x%X)\n", pct_of_module, sym,
                         r.name.c_str(), (unsigned)off, (unsigned)hot[k].rva);
                } else {
                    emit("    %6.2f%% of module   %s+0x%X\n", pct_of_module, r.name.c_str(),
                         (unsigned)hot[k].rva);
                }
                printed++;
            }
            if (r.hot_capped) {
                emit("    (另有 %llu 次采样落在未记录的热点上，直方图已满)\n",
                     (unsigned long long)r.hot_capped);
            }
        }
        if (shown_modules == 0) emit(" (还没有采到数据，请先执行 perf_start)\n");
    }

    // ---- 线程明细 ----
    if (opt.show_threads) {
        emit("================================================================================\n");
        emit(" 线程明细（按 CPU 降序）\n");
        emit("--------------------------------------------------------------------------------\n");
        std::vector<ThreadStat> th = s.threads;
        std::stable_sort(th.begin(), th.end(),
                         [](const ThreadStat &a, const ThreadStat &b) { return a.hits > b.hits; });
        emit(" #  CPU%%   TID      THREADNAME\n");
        int n = 0;
        for (size_t i = 0; i < th.size() && n < 40; ++i) {
            if (th[i].hits == 0) continue;
            emit("%2d %5.2f  %-8u %s\n", n + 1,
                 total > 0 ? (double)th[i].hits * 100.0 / (double)total : 0.0, th[i].tid,
                 th[i].name.empty() ? "(未命名)" : th[i].name.c_str());
            n++;
        }
        if (n == 0) emit(" (还没有采到数据)\n");
    }

    emit("================================================================================\n");
    m_out = 0;
    m_out_user = 0;
}

}  // namespace ps
