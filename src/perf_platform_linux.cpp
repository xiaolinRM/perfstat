//========================================================================================
// perfstat - Linux 平台实现
//
//   * 模块枚举      : /proc/self/maps 去重
//   * 线程采样      : 读出 /proc/self/task 下所有 tid，用 tgkill 给每个线程发一个
//                     实时信号；信号处理器里从 ucontext 取出被打断的 EIP 写回槽位，
//                     采样线程等待所有槽位回填。处理器内不解锁、不分配、无状态。
//   * 内存归因      : 解析 /proc/self/maps 的每一段，RSS 累加到对应模块；来自文件的
//                     映射段算 mapped，匿名段算 private，映射但未驻留的算 other。
//   * 符号解析      : 直接读 ELF 的 .symtab/.dynsym（不依赖 libbfd/addr2line），
//                     按 st_value 升序，用二分查找最近的符号。
//========================================================================================

#if defined(_WIN32)
#error "perf_platform_linux.cpp should only be compiled on Linux"
#endif

#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "platform.h"

#include <dlfcn.h>

// 【本文件自给自足】诊断用的相对时间戳（毫秒）。
//
// 为什么不用 core.cpp 里的 ps_dbg_now_ms：
// 离线自检的 Makefile 只编平台层、不编 core.cpp，引用外部函数会链接失败
// （这条约束已经踩过三次：g_perfstat_verbose_flag / elapsed_seconds / ps_dbg_now_ms）。
// 平台层只能依赖 C 运行库和自己，所以这里自带一个 static 版本。
namespace ps {

namespace {

static double ps_plat_now_ms() {
    static double base = 0.0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
    if (base == 0.0) base = t;
    return t - base;
}

//----------------------------------------------------------------------------------------
// 采样槽位
//
// 采样是"投递信号 -> 等信号处理器把被打断的指令指针写回来"。
// 这里有两个坑，都踩过：
//
// 坑 1（槽位下标放全局变量）：进程里存在两个采样者时（插件自己的采样线程 + 离线自检
//   直接调 ps_sample_threads），两边会互相改写同一个下标，某一方永远等不到回填。
//
// 坑 2（改成 thread_local，反而彻底坏了）：槽位是【跨线程】的通信单元 ——
//   采样线程写 state=1，信号处理器在【目标线程】上运行并回填。用 thread_local 之后，
//   处理器看的是目标线程那份（全空），采样线程轮询的是自己那份（永远等不到），
//   于是每一次采样都必然 20ms 超时、一个样本都拿不到。
//
// 正确答案分两步：
//   a) 槽位表是【全进程共享】的普通全局数组（跨线程可见），每个采样线程按 tid 独占一格；
//   b) 处理器【不靠扫表定位】，而是读一个全局的"当前请求槽位指针"直接写
//      —— 这样即使进程里有多份本文件副本、处理器和发起者来自不同副本，也依然正确。
//      （注意这个指针必须是【全局】而不是 thread_local，原因见它自己的注释。）
//----------------------------------------------------------------------------------------
const int kMaxSlots = 512;  // 槽位表总大小（够 512 个并发采样线程）

struct Slot {
    volatile int state;  // 0=空闲 1=已投递 2=已完成
    volatile unsigned long ip;
};

// 全进程共享（不能用 thread_local，原因见上）
static Slot g_slots[kMaxSlots];
// 本采样线程独占的槽位下标 + 是否已分配
static thread_local int g_slot_index = -1;

// 【关键】当前正在等待回填的槽位地址。
//
// 为什么用"指针"而不是让处理器自己去表里找：同一个进程里可能存在
// 【多份本文件的副本】（perfstat_srv.so 一份、离线自检程序一份），
// 每份各有自己的 g_slots 数组。信号处理器是进程级、后装覆盖先装的，
// 于是处理器是 A 副本的、发起采样的是 B 副本 —— A 的处理器无论如何都扫不到
// B 的槽位表（那是另一块内存）。传"地址"就没有这个问题：地址指向发起者的内存。
//
// 【绝对不能加 thread_local】
// 这是本文件第二个同类错误：处理器运行在【目标线程】上，读的是目标线程的 TLS。
// 如果这个指针是 thread_local，采样线程写的是自己那份、处理器读的是目标线程那份（恒为 0），
// 结果是处理器什么都不写、采样全部 20ms 超时、零样本。
//
// 用普通全局变量即可：采样是同一个采样线程串行发起的，
// "同一个采样线程同一时刻只有一个请求"这个约束没有变，
// 不同采样线程的请求时间窗互不重叠（各自 20ms 超时内必然结束并清零）。
static Slot *volatile g_pending_slot = 0;

int g_sig = -1;
bool g_inited = false;

// "尽快停止采样"标志（原子读写，采样线程与插件主线程并发访问）
static volatile int g_stop_sampling = 0;

// 平台层自己的诊断开关。不要引用插件入口的全局变量（自检不编 core.cpp，会链接失败）。
static volatile int g_ps_debug = 0;

// "只采样运行态线程"开关，默认开（见 platform.h 的详细说明）
static volatile int g_state_filter = 1;

// 诊断计数器：用来区分"信号没送到"和"处理器跑了但没找到槽位"
static volatile long g_handler_runs = 0;   // 处理器一共进了多少次
static volatile long g_handler_miss = 0;   // 进了处理器但窗口内没有 state==1 的槽位

//----------------------------------------------------------------------------------------
// 进程级采样互斥
//
// 【为什么必须有】采样要"投递信号 -> 等回填"，而 g_pending_slot 是全局单份的。
// 如果同一个进程里有【两个采样者】同时跑（例如插件自己的采样线程 + 宿主程序/自检
// 直接调 ps_sample_threads），两个采样者会互相覆盖对方的 pending 指针：
// A 设好 pending 正准备等回填，B 紧接着把它改成自己的槽位，于是 A 永远等不到 ——
// 表现为"偶尔有线程超时 20ms、采样数忽高忽低"，而且只在两个采样者并存时出现。
//
// 用它把采样串行化：同一时刻只有一个采样者在跑，另一个短暂自旋等待。
// 采样本身是毫秒级的，而且冲突很少，所以自旋是合适的。
//
// 注意：用 __sync_lock_test_and_set（PS_ATOMIC_SET）实现，和 SpinLock 一致；
// 这里不能用 g_pending_slot 那套，因为这是"谁有资格采样"的门。
//----------------------------------------------------------------------------------------
static volatile long g_sampling_lock = 0;

struct SamplingGuard {
    bool held;

    // 【不要用纯自旋】另一个采样者采样一轮可能要几十毫秒（忙线程占核时要等调度），
    // 纯自旋会把 CPU 全烧掉、还可能因为自旋预算耗尽而提前放弃，导致自己一轮都采不到。
    // 实测踩过：自检那段采样占锁约 5 秒，插件的采样线程自旋耗尽后放弃，
    // 结果是"插件自身采样 2 秒内 0 增长"这种间歇性失败。
    //
    // 所以改成"先自旋一小会儿，然后 sched_yield 让出 CPU"。
    // 等待上限给得很宽松（默认 30 秒），因为采样本身是毫秒级的，
    // 真等这么久只可能是另一个采样者被卡住，那时放弃也合理。
    explicit SamplingGuard(double max_wait_sec = 30.0) : held(false) {
        if (__sync_lock_test_and_set(&g_sampling_lock, 1) == 0) {
            held = true;
            return;
        }
        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        int spins = 0;
        for (;;) {
            if (__sync_add_and_fetch(&g_sampling_lock, 0) == 0) {
                if (__sync_lock_test_and_set(&g_sampling_lock, 1) == 0) {
                    held = true;
                }
                return;
            }
            // 先短自旋（冲突很快会过去），再让出 CPU
            if (++spins < 2000) continue;
            sched_yield();
            clock_gettime(CLOCK_MONOTONIC, &now);
            double waited = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
            if (waited > max_wait_sec) return;  // 放弃：返回 0 个样本，不阻塞调用方
        }
    }
    ~SamplingGuard() {
        if (held) __sync_lock_release(&g_sampling_lock);
    }
};

//----------------------------------------------------------------------------------------
// 信号处理器
//----------------------------------------------------------------------------------------
#if defined(__x86_64__)
#define PS_UC_IP(uc) ((unsigned long)(uc)->uc_mcontext.gregs[REG_RIP])
#elif defined(__i386__)
#define PS_UC_IP(uc) ((unsigned long)(uc)->uc_mcontext.gregs[REG_EIP])
#else
#define PS_UC_IP(uc) (0UL)
#endif

// 给当前采样线程分配一个独占槽位（第一次调用时算，之后复用）
static int slot_for_this_thread() {
    if (g_slot_index < 0) {
        unsigned long tid = (unsigned long)syscall(SYS_gettid);
        g_slot_index = (int)(tid % (unsigned long)kMaxSlots);
    }
    return g_slot_index;
}

// 信号处理器里怎么知道该写哪个槽位？
// 分两步：先看"发起采样的线程"自己的私有窗口，找不到再用 CAS 认领全表任意
// state==1 的槽位（应对"进程里有多份本文件副本"的情况，详见下面第 2 步的注释）。
// 采样线程同一时刻最多只有一个请求在飞（sample_tid_range 严格串行），所以不会认错。
void ps_signal_handler(int, siginfo_t *, void *vctx) {
    __atomic_add_fetch(&g_handler_runs, 1, __ATOMIC_RELAXED);
    ucontext_t *uc = (ucontext_t *)vctx;
    unsigned long ip = PS_UC_IP(uc);

    // 直接写"发起采样的那个线程"正在等的槽位：地址来自全局的 g_pending_slot，
    // 一定指向发起者的那张槽位表（即使处理器和发起者来自不同副本也没问题）。
    // 这里【不能】用 thread_local 读 —— 处理器运行在目标线程上，
    // 目标线程的 TLS 里没有发起者写下的指针。
    Slot *slot = (Slot *)__atomic_load_n((void *volatile *)&g_pending_slot, __ATOMIC_ACQUIRE);
    if (slot) {
        slot->ip = ip;
        __atomic_store_n(&slot->state, 2, __ATOMIC_RELEASE);
        return;
    }

    // 退路：理论上不该走到这里（pending 指针总该有值）。
    // 万一走到了，就退回"写本线程独占的那一格"这种老办法，尽量别丢样本。
    int own = slot_for_this_thread();
    if (__atomic_load_n(&g_slots[own].state, __ATOMIC_ACQUIRE) == 1) {
        g_slots[own].ip = ip;
        __atomic_store_n(&g_slots[own].state, 2, __ATOMIC_RELEASE);
        return;
    }

    __atomic_add_fetch(&g_handler_miss, 1, __ATOMIC_RELAXED);
}

//----------------------------------------------------------------------------------------
// 小工具
//----------------------------------------------------------------------------------------
const char *ps_basename(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

// 有界字符串拷贝，并保证 0 结尾。
// 不用 strncpy 是因为 gcc 的 -Wstringop-truncation 会对"源串可能更长"的情况报警，
// 而这个场景里我们本来就是故意截断的。
void ps_copy_cstr(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

//----------------------------------------------------------------------------------------
// 只保留"正在占用 CPU（运行态 R）"的线程
//
// 【为什么必须过滤】采样是"给线程投信号、取它当时的指令指针"。
// 但一个阻塞在 futex/epoll/nanosleep 里的空闲线程，它的指令指针恰好停在
// libc 的 syscall 包装里 —— 被采样到就会把这一票算给 libc/ntdll。
// 服务器上大量工作线程平时都在阻塞等待，于是 libc/ntdll 的占比被抬到 90% 以上，
// 而真正在跑的模块反而只有零点几个百分点，报告完全没有参考价值（实测如此）。
//
// 而 /proc/<tid>/stat 第 3 个字段就是线程状态：
//   R = running/runnable（正在用或被调度用 CPU）
//   S = 可中断睡眠（阻塞在 futex/nanosleep/read 等）
//   D = 不可中断睡眠
// 只采 R 才是"CPU 占用"的正确语义。
//
// 代价：每轮要读一遍 /proc/self/task/<tid>/stat。进程线程多时有点开销，
// 但换来的是准确的结果，值得。开销统计见 perfstat.ini 的 cpu_state_filter 说明。
//----------------------------------------------------------------------------------------
bool thread_is_running(int tid) {
    char path[128];
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(path, "r");
    if (!f) return false;  // 线程刚退出
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;
    buf[n] = 0;
    // stat 的第 2 个字段是 (comm)，里面可能带空格和括号，所以从最后一个 ')' 之后开始
    char *rp = strrchr(buf, ')');
    if (!rp || rp[1] == 0) return false;
    // 跳过 ') ' 之后就是 state 字段
    char *p2 = rp + 1;
    while (*p2 == ' ') p2++;
    return *p2 == 'R';
}

std::vector<int> list_tids() {
    std::vector<int> out;
    DIR *d = opendir("/proc/self/task");
    if (!d) return out;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int tid = atoi(e->d_name);
        if (tid > 0) out.push_back(tid);
    }
    closedir(d);
    return out;
}

void read_thread_name(int tid, char *out, size_t out_size) {
    out[0] = 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, out, out_size - 1);
    close(fd);
    if (n <= 0) {
        out[0] = 0;
        return;
    }
    out[n] = 0;
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = 0;
}

//----------------------------------------------------------------------------------------
// /proc/self/maps 解析
//----------------------------------------------------------------------------------------
struct MapEntry {
    unsigned long start;
    unsigned long end;
    unsigned long offset;
    unsigned long inode;
    char perms[5];
    bool has_file;
    std::string path;
    long rss_pages;  // -1 表示未知
};

bool parse_maps(std::vector<MapEntry> &out) {
    out.clear();
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // 形如：  f7f00000-f7f21000 r-xp 00000000 08:01 1234567    /lib/libc.so.6
        unsigned long start = 0, end = 0, offset = 0, inode = 0;
        int major = 0, minor = 0;
        char perms[8] = {0};
        char dev[32] = {0};
        int consumed = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %31s %lu %n", &start, &end, perms, &offset, dev, &inode,
                   &consumed) < 6) {
            continue;
        }
        (void)major;
        (void)minor;
        MapEntry e;
        e.start = start;
        e.end = end;
        e.offset = offset;
        e.inode = inode;
        memcpy(e.perms, perms, sizeof(e.perms));
        e.rss_pages = -1;
        const char *rest = line + consumed;
        while (*rest == ' ') ++rest;
        size_t rl = strlen(rest);
        while (rl > 0 && (rest[rl - 1] == '\n' || rest[rl - 1] == '\r')) rl--;
        if (rl > 0 && rest[0] == '/') {
            e.has_file = true;
            e.path.assign(rest, rl);
            // 去掉 " (deleted)"
            const char *del = " (deleted)";
            size_t dl = strlen(del);
            if (e.path.size() > dl && e.path.compare(e.path.size() - dl, dl, del) == 0) {
                e.path.resize(e.path.size() - dl);
            }
        } else {
            e.has_file = false;
            e.path.assign(rest, rl);
        }
        out.push_back(e);
    }
    fclose(f);

    // 补齐 smaps 里的 RSS（只有需要时才读，读取成本较高）
    return true;
}

void fill_smaps_rss(std::vector<MapEntry> &maps) {
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) return;
    char line[1024];
    long cur_rss_kb = -1;
    unsigned long cur_start = 0, cur_end = 0;
    bool in_header = false;
    bool started = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long s = 0, e = 0;
        if (sscanf(line, "%lx-%lx", &s, &e) == 2 && line[12] == ' ') {
            // 上一段结束，回填
            if (started) {
                for (size_t i = 0; i < maps.size(); ++i) {
                    if (maps[i].start == cur_start) {
                        maps[i].rss_pages = cur_rss_kb;
                        break;
                    }
                }
            }
            cur_start = s;
            cur_end = e;
            cur_rss_kb = 0;
            in_header = true;
            started = true;
            continue;
        }
        if (strncmp(line, "Rss:", 4) == 0) {
            cur_rss_kb = atol(line + 4);
            in_header = false;
        }
    }
    fclose(f);
    if (started) {
        for (size_t i = 0; i < maps.size(); ++i) {
            if (maps[i].start == cur_start) {
                maps[i].rss_pages = cur_rss_kb;
                break;
            }
        }
    }
    (void)cur_end;
    (void)in_header;
}

//----------------------------------------------------------------------------------------
// ELF 符号解析
//----------------------------------------------------------------------------------------
struct SymEntry {
    unsigned long value;  // 模块内偏移（针对 ET_DYN 已减去 p_vaddr）
    uint32_t name_index;
};

struct SymModule {
    unsigned long base;
    std::string path;
    std::vector<SymEntry> entries;
    std::vector<std::string> names;
};

std::vector<SymModule *> g_sym_modules;

bool read_whole_file(const char *path, std::vector<unsigned char> &out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return false;
    }
    out.resize((size_t)st.st_size);
    size_t got = 0;
    while (got < out.size()) {
        ssize_t n = read(fd, &out[got], out.size() - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    out.resize(got);
    return got > 0;
}

void add_symbols_from_table(const unsigned char *data, size_t size, const Elf32_Shdr &sym_sec,
                            const Elf32_Shdr &str_sec, unsigned long vaddr_bias, SymModule *mod,
                            int kind /*1=func 2=object*/) {
    if (sym_sec.sh_entsize == 0) return;
    if (sym_sec.sh_offset + sym_sec.sh_size > size) return;
    if (str_sec.sh_offset + str_sec.sh_size > size) return;
    const Elf32_Sym *syms = (const Elf32_Sym *)(data + sym_sec.sh_offset);
    const char *strs = (const char *)(data + str_sec.sh_offset);
    size_t count = sym_sec.sh_size / sym_sec.sh_entsize;
    for (size_t i = 0; i < count; ++i) {
        const Elf32_Sym &s = syms[i];
        unsigned char type = ELF32_ST_TYPE(s.st_info);
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_GNU_IFUNC) continue;
        if (s.st_shndx == SHN_UNDEF) continue;
        if (s.st_value == 0) continue;
        if (s.st_name == 0 || s.st_name >= str_sec.sh_size) continue;
        const char *nm = strs + s.st_name;
        size_t max_len = str_sec.sh_size - s.st_name;
        size_t len = strnlen(nm, max_len);
        if (len == 0 || len == max_len) continue;
        unsigned long value = s.st_value;
        if (value >= vaddr_bias) value -= vaddr_bias;
        else value = 0;
        SymEntry e;
        e.value = value;
        e.name_index = (uint32_t)mod->names.size();
        mod->names.push_back(std::string(nm, len));
        mod->entries.push_back(e);
        (void)kind;
    }
}

SymModule *load_elf_symbols(const char *path, uintptr_t base) {
    std::vector<unsigned char> file;
    if (!read_whole_file(path, file)) return 0;
    if (file.size() < sizeof(Elf32_Ehdr)) return 0;

    const unsigned char *data = &file[0];
    const size_t size = file.size();
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)data;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return 0;
    if (eh->e_ident[EI_CLASS] != ELFCLASS32) return 0;  // srcds 是 32 位
    if (eh->e_shoff == 0 || eh->e_shnum == 0) return 0;
    if ((size_t)eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf32_Shdr) > size) return 0;

    const Elf32_Shdr *sh = (const Elf32_Shdr *)(data + eh->e_shoff);
    const Elf32_Shdr *shstr = 0;
    if (eh->e_shstrndx < eh->e_shnum) shstr = &sh[eh->e_shstrndx];
    if (!shstr || shstr->sh_offset + shstr->sh_size > size) return 0;
    const char *shstrs = (const char *)(data + shstr->sh_offset);

    // 找 PT_LOAD 的最低 p_vaddr（用于把 st_value 转成模块内偏移）
    unsigned long vaddr_bias = 0;
    if (eh->e_phoff && eh->e_phnum &&
        (size_t)eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf32_Phdr) <= size) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(data + eh->e_phoff);
        bool found = false;
        for (int i = 0; i < eh->e_phnum; ++i) {
            if (ph[i].p_type != PT_LOAD) continue;
            if (!found || ph[i].p_vaddr < vaddr_bias) {
                vaddr_bias = ph[i].p_vaddr;
                found = true;
            }
        }
    }

    SymModule *mod = new SymModule();
    mod->base = base;
    mod->path = path;

    const Elf32_Shdr *symtab = 0, *strtab = 0, *dynsym = 0, *dynstr = 0;
    for (int i = 0; i < eh->e_shnum; ++i) {
        const char *name = (sh[i].sh_name < shstr->sh_size) ? shstrs + sh[i].sh_name : "";
        if (strcmp(name, ".symtab") == 0) {
            symtab = &sh[i];
            if (sh[i].sh_link < eh->e_shnum) strtab = &sh[sh[i].sh_link];
        } else if (strcmp(name, ".dynsym") == 0) {
            dynsym = &sh[i];
            if (sh[i].sh_link < eh->e_shnum) dynstr = &sh[sh[i].sh_link];
        }
    }

    if (symtab && strtab) add_symbols_from_table(data, size, *symtab, *strtab, vaddr_bias, mod, 1);
    if (dynsym && dynstr) add_symbols_from_table(data, size, *dynsym, *dynstr, vaddr_bias, mod, 1);

    if (mod->entries.empty()) {
        delete mod;
        return 0;
    }
    // 去重 + 排序（同一地址可能同时出现在 symtab 和 dynsym）
    std::stable_sort(mod->entries.begin(), mod->entries.end(),
                     [](const SymEntry &a, const SymEntry &b) { return a.value < b.value; });
    std::vector<SymEntry> uniq;
    uniq.reserve(mod->entries.size());
    for (size_t i = 0; i < mod->entries.size(); ++i) {
        if (!uniq.empty() && uniq.back().value == mod->entries[i].value) continue;
        uniq.push_back(mod->entries[i]);
    }
    mod->entries.swap(uniq);
    return mod;
}

}  // namespace

//----------------------------------------------------------------------------------------
// 生命周期
//----------------------------------------------------------------------------------------
bool ps_platform_init() {
    if (g_inited) return true;
    g_inited = true;

    // g_slots 是全局数组，静态存储期，本来就零初始化，这里不用管

    // 优先用 SIGRTMIN+2；若不可用则退到 SIGURG（SIGPROF 会被游戏/剖析器占用）
    int candidates[3] = {SIGRTMIN + 2, SIGRTMIN + 1, SIGURG};
    for (int i = 0; i < 3; ++i) {
        int sig = candidates[i];
        if (sig <= 0) continue;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = ps_signal_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        if (sigaction(sig, &sa, 0) == 0) {
            g_sig = sig;
            break;
        }
    }
    if (g_sig < 0) return false;

    if (g_ps_debug) {
        sigset_t cur;
        sigemptyset(&cur);
        pthread_sigmask(SIG_SETMASK, 0, &cur);
        fprintf(stderr, "[perfstat-hb] platform_init: sig=%d blocked_in_me=%d\n", g_sig,
                sigismember(&cur, g_sig) ? 1 : 0);
    }

    // 【这里绝对不要 pthread_sigmask(SIG_BLOCK, ...)】
    //
    // 踩过的坑：为了保证"采样线程自己不被这个信号打断"，最初在这里屏蔽了采样信号。
    // 但 POSIX 规定【新线程会继承创建者的信号掩码】，于是在插件初始化之后创建的线程
    // （引擎的网络线程、物理线程等等）全都把这个信号屏蔽掉了 —— 采样时 tgkill 投递
    // 的信号永远进不了那些线程的处理器，结果就是一个样本都采不到（而且不报错）。
    //
    // 现在不屏蔽了，安全性靠另外两点保证：
    //   * 采样时总是跳过"自己"（sample_tid_range 里比对 SYS_gettid），所以采样线程
    //     不会给自己发信号，也就不会打断自己；
    //   * 信号处理器本身是异步信号安全的（只扫本线程槽位表 + 写一个指针，不加锁、
    //     不分配内存），即使被投递到非目标线程也只是白跑一趟。
    return true;
}

void ps_platform_shutdown() {
    for (size_t i = 0; i < g_sym_modules.size(); ++i) delete g_sym_modules[i];
    g_sym_modules.clear();
}

//----------------------------------------------------------------------------------------
// 模块枚举
//----------------------------------------------------------------------------------------
void ps_enum_modules(ModuleVisits *out) {
    if (!out || !out->items) return;
    std::vector<MapEntry> maps;
    if (!parse_maps(maps)) return;

    std::vector<std::string> seen;
    bool first = true;
    for (size_t i = 0; i < maps.size(); ++i) {
        if (!maps[i].has_file || maps[i].path.empty()) continue;
        if (maps[i].inode == 0) continue;
        // 只保留可执行的映射段（避免把同名数据文件重复计入）
        if (maps[i].perms[2] != 'x') continue;

        bool dup = false;
        for (size_t k = 0; k < seen.size(); ++k) {
            if (seen[k] == maps[i].path) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        seen.push_back(maps[i].path);

        // 该模块的整体范围 = 所有同名段的最小 start / 最大 end
        unsigned long lo = maps[i].start, hi = maps[i].end;
        for (size_t k = 0; k < maps.size(); ++k) {
            if (maps[k].has_file && maps[k].path == maps[i].path) {
                if (maps[k].start < lo) lo = maps[k].start;
                if (maps[k].end > hi) hi = maps[k].end;
            }
        }

        ModuleInfo mi;
        memset(&mi, 0, sizeof(mi));
        mi.base = (uintptr_t)lo;
        mi.size = (size_t)(hi - lo);
        ps_copy_cstr(mi.path, sizeof(mi.path), maps[i].path.c_str());
        ps_copy_cstr(mi.name, sizeof(mi.name), ps_basename(mi.path));
        mi.is_main = first;
        first = false;
        if (out->count < out->capacity) out->items[out->count++] = mi;
    }

    // 主程序（srcds_linux / srcds_i686）在 maps 里通常带路径，若没有则补一条
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        bool found = false;
        for (size_t k = 0; k < seen.size(); ++k) {
            if (seen[k] == exe) {
                found = true;
                break;
            }
        }
        if (!found && out->count < out->capacity) {
            ModuleInfo mi;
            memset(&mi, 0, sizeof(mi));
            mi.base = 0;
            mi.size = 0;
            ps_copy_cstr(mi.path, sizeof(mi.path), exe);
            ps_copy_cstr(mi.name, sizeof(mi.name), ps_basename(mi.path));
            mi.is_main = (seen.empty());
            out->items[out->count++] = mi;
        }
    }

    // 按基址排序，方便上层做二分查找归因
    std::stable_sort(out->items, out->items + out->count,
                     [](const ModuleInfo &a, const ModuleInfo &b) { return a.base < b.base; });
}

//----------------------------------------------------------------------------------------
// 线程枚举
//----------------------------------------------------------------------------------------
void ps_enum_threads(ThreadVisits *out) {
    if (!out || !out->items) return;
    std::vector<int> tids = list_tids();
    for (size_t i = 0; i < tids.size(); ++i) {
        if (out->count >= out->capacity) break;
        ThreadInfo &ti = out->items[out->count];
        memset(&ti, 0, sizeof(ti));
        ti.tid = (uint32_t)tids[i];
        read_thread_name(tids[i], ti.name, sizeof(ti.name));
        out->count++;
    }
}

//----------------------------------------------------------------------------------------
// 采样
//----------------------------------------------------------------------------------------
// 对 tid_list[i0 .. i0+n) 这批线程逐个“投递信号 -> 等待处理器回填”。
// 必须串行处理：信号处理器是靠“扫描本线程槽位表里第一个已投递的格子”定位的，
// 批量投递会让多个处理器争抢同一格。
// 说明：能不能收到信号取决于【目标线程自己的信号掩码】，而掩码是 per-thread 的，
// 从采样线程改不了别人的掩码（POSIX 没有这种接口）。所以正确做法是：
//   * ps_platform_init 里【不要】屏蔽这个信号，让引擎之后创建的线程自然继承"未屏蔽"；
//   * 如果宿主自己屏蔽了，那这个线程就采不到 —— 属于无法绕过的限制，
//     采样时会因为 20ms 超时而跳过它，不影响其它线程。
static int sample_tid_range(const std::vector<int> &tid_list, int i0, int n, uintptr_t *ips,
                            uint32_t *out_tids, int max_ips) {
    const pid_t self = (pid_t)syscall(SYS_gettid);
    const pid_t pid = getpid();
    int count = 0;

    for (int k = 0; k < n && count < max_ips; ++k) {
        if (__atomic_load_n(&g_stop_sampling, __ATOMIC_ACQUIRE)) break;
        const int pos = i0 + k;
        if (pos < 0 || pos >= (int)tid_list.size()) break;
        const int tid = tid_list[pos];
        if (tid == self) continue;

        // 本线程独占的槽位（同一时刻只有这一个请求在飞）
        const int idx = slot_for_this_thread();

        __atomic_store_n(&g_slots[idx].ip, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_slots[idx].state, 1, __ATOMIC_RELEASE);
        // 告诉处理器"回填这一个槽位"（传地址，跨副本也正确）
        __atomic_store_n((void *volatile *)&g_pending_slot, (void *)&g_slots[idx],
                         __ATOMIC_RELEASE);

        int tk = (int)syscall(SYS_tgkill, pid, tid, g_sig);
        if (tk != 0 && g_ps_debug) {
            fprintf(stderr, "[perfstat-hb] tgkill(tid=%d) FAILED rc=%d sig=%d\n", tid, tk, g_sig);
        }
        if (tk != 0) {
            __atomic_store_n((void *volatile *)&g_pending_slot, (void *)0, __ATOMIC_RELEASE);
            __atomic_store_n(&g_slots[idx].state, 0, __ATOMIC_RELEASE);
            continue;
        }

        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        int poll_iters = 0;
        for (;;) {
            if (__atomic_load_n(&g_slots[idx].state, __ATOMIC_ACQUIRE) == 2) {
                unsigned long ip = __atomic_load_n(&g_slots[idx].ip, __ATOMIC_ACQUIRE);
                if (ip) {
                    ips[count] = (uintptr_t)ip;
                    if (out_tids) out_tids[count] = (uint32_t)tid;
                    count++;
                }
                break;
            }
            // 有人请求停止（插件正在卸载）就立刻放弃本轮，别再等满 20ms。
            // 这是"采样线程能迅速退出"的关键：线程多的时候一轮本来要几百毫秒。
            if (__atomic_load_n(&g_stop_sampling, __ATOMIC_ACQUIRE)) break;

            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
            if (ms > 20) {
                // 超时了：把这个线程号报出来。CI 上偶尔出现 tgkill 失败/无响应，
                // 没有这条日志就只能看到"某一轮之后没动静了"。
                // 不依赖 verbose：超时说明这个线程没响应信号，是异常情况
                {
                    static int dbg_to = 0;
                    if (dbg_to < 12) {
                        fprintf(stderr, "[perfstat-hb]   TIMEOUT tid=%d after %ld ms (%d polls)\n",
                                tid, ms, poll_iters);
                        dbg_to++;
                    }
                }
                break;  // 该线程屏蔽了这个信号，放弃
            }

            poll_iters++;
            // 轮询之间小睡一下，避免空转烧 CPU（原来是纯忙等）
            struct timespec tiny;
            tiny.tv_sec = 0;
            tiny.tv_nsec = 200000;  // 0.2ms
            nanosleep(&tiny, 0);
        }
        __atomic_store_n((void *volatile *)&g_pending_slot, (void *)0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_slots[idx].state, 0, __ATOMIC_RELEASE);
    }
    return count;
}

int ps_sample_threads(uintptr_t *ips, uint32_t *out_tids, int max_ips) {
    if (!g_inited) ps_platform_init();
    if (g_sig < 0 || !ips || max_ips <= 0) return 0;

    // 串行化：同一时刻只允许一个采样者（避免多个采样者互相覆盖 pending 槽位）
    SamplingGuard guard;
    if (!guard.held) return 0;

    static std::vector<int> tid_list;
    tid_list = list_tids();

    if (g_state_filter) {
        static std::vector<int> running;
        running.clear();
        running.reserve(tid_list.size());
        for (size_t i = 0; i < tid_list.size(); ++i) {
            if (thread_is_running(tid_list[i])) running.push_back(tid_list[i]);
        }
        if (running.empty()) return 0;
        tid_list.swap(running);
    }

    int n = (int)tid_list.size();
    if (n > max_ips) n = max_ips;
    return sample_tid_range(tid_list, 0, n, ips, out_tids, max_ips);
}

int ps_sample_threads_window(uintptr_t *ips, uint32_t *out_tids, int max_ips, int *cursor,
                             int window) {
    if (!g_inited) ps_platform_init();
    if (g_sig < 0 || !ips || max_ips <= 0) return 0;

    // 串行化（同上）
    SamplingGuard guard;
    if (!guard.held) return 0;

    static std::vector<int> tid_list;
    tid_list = list_tids();
    const int total = (int)tid_list.size();
    {
        static int dbg_calls = 0;
        // 每次窗口调用都记一下耗时：超过 1 秒就报（说明有线程一直不响应）
        static struct timespec dbg_t0;
        struct timespec dbg_t1;
        clock_gettime(CLOCK_MONOTONIC, &dbg_t1);
        if (dbg_calls > 0 && g_ps_debug) {
            long dms = (dbg_t1.tv_sec - dbg_t0.tv_sec) * 1000 +
                       (dbg_t1.tv_nsec - dbg_t0.tv_nsec) / 1000000;
            if (dms > 1000) {
                fprintf(stderr, "[perfstat-hb] WINDOW SLOW: previous call took %ld ms\n", dms);
            }
        }
        dbg_t0 = dbg_t1;
        if (g_ps_debug && (dbg_calls < 2 || dbg_calls % 500 == 0)) {
            fprintf(stderr,
                    "[perfstat-hb] [%.0f ms] window: tids=%d self=%d sig=%d handler_runs=%ld\n",
                    ps_plat_now_ms(), total, (int)syscall(SYS_gettid), g_sig,
                    (long)__atomic_load_n(&g_handler_runs, __ATOMIC_RELAXED));
        }
        dbg_calls++;
    }
    if (total <= 0) return 0;
    if (window <= 0 || window > max_ips) window = max_ips;

    // ★ 只保留运行态线程（见 thread_is_running 的说明）★
    // 这一步是让"CPU 占比"有意义的关键：否则阻塞线程会把 libc/ntdll 抬到 90%+。
    if (g_state_filter) {
        static std::vector<int> running;
        running.clear();
        running.reserve(tid_list.size());
        for (size_t i = 0; i < tid_list.size(); ++i) {
            if (thread_is_running(tid_list[i])) running.push_back(tid_list[i]);
        }
        if (running.empty()) return 0;  // 全都在睡：这一轮没有 CPU 占用可采
        tid_list.swap(running);
    }

    if (total <= window) {
        if (cursor) *cursor = 0;
        return sample_tid_range(tid_list, 0, total, ips, out_tids, max_ips);
    }

    const int start = cursor ? (*cursor % total) : 0;
    int n = window;
    if (start + n > total) n = total - start;  // 最后一段不足就只跑这么多
    const int got = sample_tid_range(tid_list, start, n, ips, out_tids, max_ips);
    if (cursor) *cursor = (start + window) % total;
    return got;
}

//----------------------------------------------------------------------------------------
// 内存归因
//----------------------------------------------------------------------------------------
void ps_walk_memory(ModuleLookupFn lookup, void *user, ModuleInfo *mods, int mod_count) {
    if (!mods || mod_count <= 0) return;

    std::vector<MapEntry> maps;
    if (!parse_maps(maps)) return;
    fill_smaps_rss(maps);

    const long page = sysconf(_SC_PAGESIZE);

    for (size_t i = 0; i < maps.size(); ++i) {
        const MapEntry &e = maps[i];
        int idx = lookup ? lookup((uintptr_t)e.start, user) : -1;
        if (idx < 0 || idx >= mod_count) continue;

        const size_t bytes = (size_t)(e.end - e.start);
        if (e.rss_pages < 0) {
            // 没读到 smaps：无法区分驻留，统一按“其它已提交”计
            mods[idx].other_bytes += bytes;
            continue;
        }
        size_t rss = (size_t)e.rss_pages * 1024;  // smaps 的 Rss 单位是 KB
        if (rss > bytes) rss = bytes;
        if (e.has_file) {
            mods[idx].mapped_bytes += rss;
        } else {
            mods[idx].private_bytes += rss;
        }
        mods[idx].other_bytes += (bytes - rss);
    }
    (void)page;
}

//----------------------------------------------------------------------------------------
// 符号解析
//----------------------------------------------------------------------------------------
uintptr_t ps_symbols_open(const char *path, uintptr_t base) {
    if (!path || !path[0]) return 0;
    for (size_t i = 0; i < g_sym_modules.size(); ++i) {
        if (g_sym_modules[i]->base == base) return (uintptr_t)g_sym_modules[i];
    }
    SymModule *m = load_elf_symbols(path, base);
    if (!m) return 0;
    g_sym_modules.push_back(m);
    return (uintptr_t)m;
}

void ps_symbols_close(uintptr_t handle) {
    if (!handle) return;
    SymModule *m = (SymModule *)handle;
    for (size_t i = 0; i < g_sym_modules.size(); ++i) {
        if (g_sym_modules[i] == m) {
            g_sym_modules.erase(g_sym_modules.begin() + i);
            break;
        }
    }
    delete m;
}

bool ps_symbols_get(uintptr_t handle, int index, uint32_t *rva, const char **name) {
    SymModule *m = (SymModule *)handle;
    if (!m || index < 0 || index >= (int)m->entries.size()) return false;
    if (rva) *rva = (uint32_t)m->entries[index].value;
    if (name) *name = m->names[m->entries[index].name_index].c_str();
    return true;
}

const char *ps_symbolize(uintptr_t handle, uint32_t rva, uint32_t *offset) {
    SymModule *m = (SymModule *)handle;
    if (offset) *offset = rva;
    if (!m || m->entries.empty()) return 0;
    int lo = 0, hi = (int)m->entries.size() - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m->entries[mid].value <= rva) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0) return 0;
    if (offset) *offset = (uint32_t)(rva - m->entries[best].value);
    return m->names[m->entries[best].name_index].c_str();
}

void ps_set_debug(int on) { __atomic_store_n(&g_ps_debug, on ? 1 : 0, __ATOMIC_RELEASE); }

void ps_set_cpu_state_filter(int on) {
    __atomic_store_n(&g_state_filter, on ? 1 : 0, __ATOMIC_RELEASE);
}
bool ps_get_cpu_state_filter(void) {
    return __atomic_load_n(&g_state_filter, __ATOMIC_ACQUIRE) != 0;
}

// 见 platform.h 的说明。Linux 这边用 dlopen(自己) 把引用计数 +1，
// 引擎随后的 dlclose 扣不掉，SO 就保持映射。需要 -ldl（已在链接参数里）。
bool ps_keep_module_mapped() {
    // 从 /proc/self/maps 里找出包含本函数的那个文件映射，拿到自己的路径
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    unsigned long self_addr = (unsigned long)(uintptr_t)&ps_keep_module_mapped;
    char line[1024];
    char path[768];
    path[0] = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo = 0, hi = 0;
        if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
        if (self_addr < lo || self_addr >= hi) continue;
        char *slash = strchr(line, '/');
        if (!slash) continue;
        // 去掉行尾换行
        char *nl = strchr(slash, '\n');
        if (nl) *nl = 0;
        size_t n = strlen(slash);
        if (n >= sizeof(path)) n = sizeof(path) - 1;
        memcpy(path, slash, n);
        path[n] = 0;
        break;
    }
    fclose(f);
    if (!path[0]) return false;
    void *h = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (h) {
        // RTLD_NOLOAD 只会在"已加载"时返回句柄并 +1 引用；我们刻意不 dlclose
        return true;
    }
    // RTLD_NOLOAD 不支持时退一步：正常 dlopen 一次（也会 +1 引用，且不释放）
    h = dlopen(path, RTLD_NOW);
    return h != 0;
}

// 诊断用：本副本的信号处理器一共运行了多少次。
// 自检拿它来判断"进程里实际生效的处理器是哪一份副本的"。
long ps_handler_run_count(void) {
    return (long)__atomic_load_n(&g_handler_runs, __ATOMIC_RELAXED);
}

// 线程累计 CPU 时间（ns）。见 platform.h 的说明。
// 优先读 /proc/<tid>/schedstat 第 1 个字段（内核直接给的运行时间，单位 ns，最准）；
// 拿不到就退回 /proc/<tid>/stat 的 utime+stime（单位是时钟滴答，精度差一些）。
unsigned long long ps_thread_cpu_time_ns(uint32_t tid) {
    char path[128];
    snprintf(path, sizeof(path), "/proc/self/task/%u/schedstat", tid);
    FILE *f = fopen(path, "r");
    if (f) {
        unsigned long long runtime = 0;
        int got = fscanf(f, "%llu", &runtime);
        fclose(f);
        if (got == 1) return runtime;
    }
    snprintf(path, sizeof(path), "/proc/self/task/%u/stat", tid);
    f = fopen(path, "r");
    if (!f) return 0;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = 0;
    char *rp = strrchr(buf, ')');
    if (!rp) return 0;
    int field = 3;
    char *tok = strtok(rp + 2, " ");
    unsigned long long utime = 0, stime = 0;
    while (tok) {
        if (field == 14) utime = strtoull(tok, 0, 10);
        if (field == 15) stime = strtoull(tok, 0, 10);
        field++;
        tok = strtok(0, " ");
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    return (utime + stime) * 1000000000ULL / (unsigned long long)hz;
}

void ps_prepare_sampling_thread(void) {
    if (!g_inited) ps_platform_init();
    if (g_sig < 0) return;
    // 创建采样线程的那个线程可能屏蔽了采样信号（掩码会被继承），这里把它解开，
    // 否则采样线程自己也收不到信号，就永远采不到东西。
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, g_sig);
    pthread_sigmask(SIG_UNBLOCK, &one, 0);
}

void ps_request_stop_sampling(void) { __atomic_store_n(&g_stop_sampling, 1, __ATOMIC_RELEASE); }

bool ps_stop_requested(void) { return __atomic_load_n(&g_stop_sampling, __ATOMIC_ACQUIRE) != 0; }

//----------------------------------------------------------------------------------------
// 杂项
//----------------------------------------------------------------------------------------
double ps_now_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void ps_get_local_time(int *y, int *mo, int *d, int *h, int *mi, int *s) {
    time_t t = time(0);
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (y) *y = tmv.tm_year + 1900;
    if (mo) *mo = tmv.tm_mon + 1;
    if (d) *d = tmv.tm_mday;
    if (h) *h = tmv.tm_hour;
    if (mi) *mi = tmv.tm_min;
    if (s) *s = tmv.tm_sec;
}

const char *ps_platform_name() { return "linux"; }

}  // namespace ps
