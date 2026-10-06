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

namespace ps {

namespace {

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
// 正确做法：槽位表是【全进程共享】的普通全局数组，但给每个采样线程分配一段【私有区间】
// （按 tid 算窗口起点），这样既跨线程可见，又不会两个采样者互相踩。
//----------------------------------------------------------------------------------------
const int kSlotsPerThread = 16;  // 每个采样线程独占的槽位数
const int kMaxSlots = 4096;      // 槽位表总大小（够 256 个并发采样者）

struct Slot {
    volatile int state;  // 0=空闲 1=已投递 2=已完成
    volatile unsigned long ip;
};

// 全进程共享（不能用 thread_local，原因见上）
static Slot g_slots[kMaxSlots];
// 每个采样线程自己的窗口起点 + 窗口内游标
static thread_local int g_slot_window = -1;
static thread_local int g_slot_cursor = 0;

int g_sig = -1;
bool g_inited = false;

// "尽快停止采样"标志（原子读写，采样线程与插件主线程并发访问）
static volatile int g_stop_sampling = 0;

// 平台层自己的诊断开关。不要引用插件入口的全局变量（自检不编 core.cpp，会链接失败）。
static volatile int g_ps_debug = 0;

// 诊断计数器：用来区分"信号没送到"和"处理器跑了但没找到槽位"
static volatile long g_handler_runs = 0;   // 处理器一共进了多少次
static volatile long g_handler_miss = 0;   // 进了处理器但窗口内没有 state==1 的槽位

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

// 给当前采样线程分配一段槽位窗口（第一次调用时算，之后复用）
static int slot_window_base() {
    if (g_slot_window < 0) {
        unsigned long tid = (unsigned long)syscall(SYS_gettid);
        int window = (int)(tid % (unsigned long)(kMaxSlots / kSlotsPerThread));
        g_slot_window = window * kSlotsPerThread;
    }
    return g_slot_window;
}

// 信号处理器里怎么知道该写哪个槽位？
// 分两步：先看"发起采样的线程"自己的私有窗口，找不到再用 CAS 认领全表任意
// state==1 的槽位（应对"进程里有多份本文件副本"的情况，详见下面第 2 步的注释）。
// 采样线程同一时刻最多只有一个请求在飞（sample_tid_range 严格串行），所以不会认错。
void ps_signal_handler(int, siginfo_t *, void *vctx) {
    __atomic_add_fetch(&g_handler_runs, 1, __ATOMIC_RELAXED);
    ucontext_t *uc = (ucontext_t *)vctx;
    unsigned long ip = PS_UC_IP(uc);

    // 1) 先在"发起采样的那个线程"的私有窗口里找待回填的槽位。
    //    g_slot_window 是 thread_local，而处理器运行在发起采样的线程上，
    //    所以这里读到的正是发起者的窗口（绝大多数情况走这条）。
    int base = g_slot_window < 0 ? 0 : g_slot_window;
    int end = base + kSlotsPerThread;
    if (end > kMaxSlots) end = kMaxSlots;
    for (int i = base; i < end; ++i) {
        if (__atomic_load_n(&g_slots[i].state, __ATOMIC_ACQUIRE) == 1) {
            g_slots[i].ip = ip;
            __atomic_store_n(&g_slots[i].state, 2, __ATOMIC_RELEASE);
            return;
        }
    }

    // 2) 退路：扫全表，用原子操作认领任意 state==1 的槽位。
    //
    // 【为什么必须有这条退路】同一个进程里可能存在【多份本文件的副本】——
    // 例如 perfstat_srv.so 里一份、离线自检程序里一份。信号处理器是进程级的、
    // 后装的会覆盖先装的，而 g_slots / g_slot_window 却是每份副本各自的。
    // 于是会出现"处理器是 A 副本的，但发起采样的是 B 副本"的情况：
    // A 的处理器只看 A 的窗口（里面什么都没有），B 的槽位永远等不到回填，
    // 表现就是每次采样都卡满 20ms 超时、一个样本都拿不到。
    //
    // 用 CAS 认领：只有把 state 从 1 抢到 3 的那个处理器才写 ip，
    // 所以多个处理器同时进来也不会把同一个槽位写两次。
    for (int i = 0; i < kMaxSlots; ++i) {
        int expected = 1;
        if (__atomic_compare_exchange_n(&g_slots[i].state, &expected, 3, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            g_slots[i].ip = ip;
            __atomic_store_n(&g_slots[i].state, 2, __ATOMIC_RELEASE);
            return;
        }
    }

    // 没有任何槽位在等回填（理论上不该发生），丢掉即可
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

    {
        static int dbg_targets = 0;
        if (g_ps_debug && dbg_targets < 6) {
            fprintf(stderr, "[perfstat-hb] targets(i0=%d,n=%d) self=%d ->", i0, n, (int)self);
            for (int k = 0; k < n && k < 8; ++k) {
                fprintf(stderr, " %d", tid_list[i0 + k]);
            }
            fprintf(stderr, "\n");
            dbg_targets++;
        }
    }

    for (int k = 0; k < n && count < max_ips; ++k) {
        if (__atomic_load_n(&g_stop_sampling, __ATOMIC_ACQUIRE)) break;
        const int pos = i0 + k;
        if (pos < 0 || pos >= (int)tid_list.size()) break;
        const int tid = tid_list[pos];
        if (tid == self) continue;

        // 在本线程的私有窗口内轮转（同一时刻只有一格是"已投递"状态）
        const int base = slot_window_base();
        const int idx = base + g_slot_cursor;
        g_slot_cursor = (g_slot_cursor + 1) % kSlotsPerThread;

        __atomic_store_n(&g_slots[idx].ip, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_slots[idx].state, 1, __ATOMIC_RELEASE);

        int tk = (int)syscall(SYS_tgkill, pid, tid, g_sig);
        if (tk != 0 && g_ps_debug) {
            fprintf(stderr, "[perfstat-hb] tgkill(tid=%d) FAILED rc=%d sig=%d\n", tid, tk, g_sig);
        }
        if (tk != 0) {
            __atomic_store_n(&g_slots[idx].state, 0, __ATOMIC_RELEASE);
            continue;
        }

        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (;;) {
            // 2 = 已回填；3 = 别的副本的处理器正在认领它，再等一会儿就到 2 了
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
            if (ms > 20) break;  // 该线程屏蔽了这个信号，放弃

            // 轮询之间小睡一下，避免空转烧 CPU（原来是纯忙等）
            struct timespec tiny;
            tiny.tv_sec = 0;
            tiny.tv_nsec = 200000;  // 0.2ms
            nanosleep(&tiny, 0);
        }
        {
            static int dbg_slots = 0;
            if (g_ps_debug && dbg_slots < 10) {
                int st = (int)__atomic_load_n(&g_slots[idx].state, __ATOMIC_ACQUIRE);
                fprintf(stderr,
                        "[perfstat-hb]   slot state=%d ip=%p tid=%d | handler_runs=%ld miss=%ld\n",
                        st, (void *)__atomic_load_n(&g_slots[idx].ip, __ATOMIC_ACQUIRE), tid,
                        (long)__atomic_load_n(&g_handler_runs, __ATOMIC_RELAXED),
                        (long)__atomic_load_n(&g_handler_miss, __ATOMIC_RELAXED));
                dbg_slots++;
            }
        }
        __atomic_store_n(&g_slots[idx].state, 0, __ATOMIC_RELEASE);
    }
    return count;
}

int ps_sample_threads(uintptr_t *ips, uint32_t *out_tids, int max_ips) {
    if (!g_inited) ps_platform_init();
    if (g_sig < 0 || !ips || max_ips <= 0) return 0;

    static std::vector<int> tid_list;
    tid_list = list_tids();

    int n = (int)tid_list.size();
    if (n > max_ips) n = max_ips;
    return sample_tid_range(tid_list, 0, n, ips, out_tids, max_ips);
}

int ps_sample_threads_window(uintptr_t *ips, uint32_t *out_tids, int max_ips, int *cursor,
                             int window) {
    if (!g_inited) ps_platform_init();
    if (g_sig < 0 || !ips || max_ips <= 0) return 0;

    static std::vector<int> tid_list;
    tid_list = list_tids();
    const int total = (int)tid_list.size();
    {
        static int dbg_calls = 0;
        if (g_ps_debug && (dbg_calls < 3 || dbg_calls % 200 == 0)) {
            fprintf(stderr,
                    "[perfstat-hb] window: tids=%d self=%d sig=%d | handler_runs=%ld miss=%ld\n",
                    total, (int)syscall(SYS_gettid), g_sig,
                    (long)__atomic_load_n(&g_handler_runs, __ATOMIC_RELAXED),
                    (long)__atomic_load_n(&g_handler_miss, __ATOMIC_RELAXED));
        }
        dbg_calls++;
    }
    if (total <= 0) return 0;
    if (window <= 0 || window > max_ips) window = max_ips;

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
