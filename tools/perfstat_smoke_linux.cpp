//========================================================================================
// perfstat - 平台层最小冒烟测试（POSIX）
//
// 这个程序【不加载插件】，只调用平台层的三个入口：
//     ps_platform_init / ps_enum_modules / ps_sample_threads
// 用来把"信号采样本身有没有问题"和"插件加载流程有没有问题"分开定位。
//
// 每一步都 fprintf(stderr) + fflush，这样即使后面卡住或崩了，日志里也能看到走到哪一步。
// 另外带一个闹钟（SIGALRM）：超过 N 秒没跑完就打印当前阶段并直接退出，避免 CI 挂到超时。
//
// 构建：make -f tools/Makefile.linux_tests smoke
// 运行：./build/perfstat_smoke_linux [超时秒数]
//========================================================================================

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vector>

#include "../src/platform.h"

static int g_timeout_sec = 60;
static int g_fails = 0;

#define CHECK_TRUE(cond, msg)                                    \
    do {                                                         \
        if (cond) {                                              \
            fprintf(stderr, "[smoke] OK: %s\n", msg);           \
        } else {                                                 \
            fprintf(stderr, "[smoke] FAIL: %s\n", msg);         \
            g_fails++;                                           \
        }                                                        \
    } while (0)

// 忙线程：代码显式导出 + noinline，保证就在本程序自己的 .text 里，
// 这样"样本归属到本模块"这个断言才有意义。
static volatile int g_busy = 0;
static volatile unsigned long long g_sink = 0;

extern "C" __attribute__((noinline, used, visibility("default"))) void perfstat_smoke_busy(
    unsigned spin) {
    unsigned long long acc = 0;
    for (unsigned i = 0; i < spin; ++i) {
        acc += (unsigned long long)i * 2654435761u;
        acc ^= acc >> 13;
    }
    g_sink += acc;
}

static void *busy_thread_fn(void *) {
    while (g_busy) perfstat_smoke_busy(200000);
    return 0;
}

static void start_busy_thread(pthread_t *t) {
    g_busy = 1;
    pthread_create(t, 0, busy_thread_fn, 0);
}

static void stop_busy_thread(pthread_t t) {
    g_busy = 0;
    pthread_join(t, 0);
}

// 看门狗用的阶段表。
//
// 踩过的坑：这里一开始写的是 `static volatile const char *g_phase`，想靠 volatile 让
// 信号处理器读到最新值。实际上 `const char * volatile` 和 `volatile const char *` 是两回事，
// 后者给"指向的内容"加了 volatile，于是 `const char *p = g_phase;` 会因为
// "const volatile char* → const char*" 编译不过（gcc 报 -fpermissive）。
// 正确做法：阶段名放静态字符串数组（静态存储期，永远不会悬空），
// 只用一个 volatile sig_atomic_t 下标来同步，这才是信号处理器里该用的类型。
static const char *g_phases[] = {
    "start",
    "安装闹钟",
    "ps_platform_init",
    "ps_enum_modules",
    "启动忙线程",
    "ps_enum_threads",
    "ps_sample_threads 单次",
    "连续采样 100 轮",
    "ps_symbols_open / ps_symbolize",
    "延迟与归因验证",
    "完成",
};
static volatile sig_atomic_t g_phase_idx = 0;

static const char kTimeoutPrefix[] = "\n[smoke] 超时！卡在阶段: ";

static void on_alarm(int) {
    // 只用异步信号安全的东西（write 是异步信号安全的，printf 不是）
    int i = (int)g_phase_idx;
    if (i < 0 || i >= (int)(sizeof(g_phases) / sizeof(g_phases[0]))) i = 0;
    const char *p = g_phases[i];
    (void)!write(2, kTimeoutPrefix, sizeof(kTimeoutPrefix) - 1);
    (void)!write(2, p, strlen(p));
    (void)!write(2, "\n", 1);
    _exit(3);
}

static void phase(const char *p) {
    for (size_t i = 0; i < sizeof(g_phases) / sizeof(g_phases[0]); ++i) {
        if (strcmp(g_phases[i], p) == 0) {
            g_phase_idx = (sig_atomic_t)i;
            break;
        }
    }
    fprintf(stderr, "[smoke] 阶段: %s\n", p);
    fflush(stderr);
}

int main(int argc, char **argv) {
    if (argc > 1) g_timeout_sec = atoi(argv[1]);
    if (g_timeout_sec <= 0) g_timeout_sec = 60;

    fprintf(stderr, "perfstat 平台层冒烟测试 (超时 %d 秒)\n", g_timeout_sec);
    fflush(stderr);
    phase("安装闹钟");
    signal(SIGALRM, on_alarm);
    alarm((unsigned)g_timeout_sec);

    int fails = 0;

    // ---- 1. 平台初始化（装信号处理器 + 屏蔽当前线程的采样信号）----
    phase("ps_platform_init");
    if (!ps::ps_platform_init()) {
        fprintf(stderr, "[smoke] FAIL: ps_platform_init 返回 false\n");
        fails++;
    } else {
        fprintf(stderr, "[smoke] OK: ps_platform_init\n");
    }
    fflush(stderr);

    // ---- 2. 模块枚举 ----
    phase("ps_enum_modules");
    {
        static ps::ModuleInfo mods[512];
        ps::ModuleVisits mv;
        mv.items = mods;
        mv.count = 0;
        mv.capacity = 512;
        ps::ps_enum_modules(&mv);
        fprintf(stderr, "[smoke] OK: 枚举到 %d 个模块\n", mv.count);
        if (mv.count <= 3) {
            fprintf(stderr, "[smoke] FAIL: 模块数太少\n");
            fails++;
        }
        for (int i = 0; i < mv.count && i < 5; ++i) {
            fprintf(stderr, "         [%d] %p %-28s %s\n", i, (void *)mods[i].base, mods[i].name,
                    mods[i].path);
        }
        fflush(stderr);
    }

    // ---- 3. 起一个忙线程 ----
    //
    // 【为什么必须在这里起】采样是靠"给【别的】线程发信号"取指令指针的，
    // 采样时总是跳过自己（sample_tid_range 里 `if (tid == self) continue;`）。
    // 所以如果进程里只有主线程一个线程，采样必然一个目标都没有、永远返回 0。
    // 这个坑踩过：忙线程原来加在后面的"归因验证"阶段，
    // 结果前面几个采样阶段全是 0 样本 + 20ms 超时，看起来像信号通路坏了。
    phase("启动忙线程");
    pthread_t g_busy_thread;
    start_busy_thread(&g_busy_thread);
    fprintf(stderr, "[smoke] OK: 忙线程已启动（采样需要有别的线程可采）\n");
    fflush(stderr);

    // ---- 3b. 线程枚举（现在应该能看到至少 2 个线程）----
    phase("ps_enum_threads");
    {
        static ps::ThreadInfo ths[256];
        ps::ThreadVisits tv;
        tv.items = ths;
        tv.count = 0;
        tv.capacity = 256;
        ps::ps_enum_threads(&tv);
        fprintf(stderr, "[smoke] OK: 枚举到 %d 个线程\n", tv.count);
        for (int i = 0; i < tv.count; ++i) {
            fprintf(stderr, "         tid=%u name=%s\n", ths[i].tid, ths[i].name);
        }
        CHECK_TRUE(tv.count >= 2, "枚举到不止 1 个线程（采样才有目标）");
        fflush(stderr);
    }

    // ---- 4. 采样：单次 ----
    phase("ps_sample_threads 单次");
    {
        uintptr_t ips[64];
        uint32_t tids[64];
        int n = ps::ps_sample_threads(ips, tids, 64);
        fprintf(stderr, "[smoke] 单次采样返回 %d 个样本\n", n);
        for (int i = 0; i < n && i < 5; ++i) {
            fprintf(stderr, "         ip=%p tid=%u\n", (void *)ips[i], tids[i]);
        }
        CHECK_TRUE(n > 0, "单次采样能取到样本（说明信号投递+回填是通的）");
        fflush(stderr);
    }

    // ---- 5. 采样：连续（模拟插件里的采样线程）----
    phase("连续采样 100 轮");
    {
        uintptr_t ips[64];
        uint32_t tids[64];
        int total = 0, nonnull = 0;
        for (int round = 0; round < 100; ++round) {
            int n = ps::ps_sample_threads(ips, tids, 64);
            total += n;
            for (int i = 0; i < n; ++i) {
                if (ips[i]) nonnull++;
            }
            if (round % 25 == 0) {
                fprintf(stderr, "[smoke]   轮 %d -> 累计样本 %d\n", round, total);
                fflush(stderr);
            }
        }
        fprintf(stderr, "[smoke] 100 轮共 %d 个样本（非空 %d）\n", total, nonnull);
        CHECK_TRUE(total > 0, "连续采样能采到样本");
        fflush(stderr);
    }

    // ---- 6. 符号解析：拿自己的主模块试一下 ----
    phase("ps_symbols_open / ps_symbolize");
    {
        static ps::ModuleInfo mods[512];
        ps::ModuleVisits mv;
        mv.items = mods;
        mv.count = 0;
        mv.capacity = 512;
        ps::ps_enum_modules(&mv);

        char self[1024];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = 0;
            const char *bn = strrchr(self, '/');
            bn = bn ? bn + 1 : self;
            for (int i = 0; i < mv.count; ++i) {
                if (strcmp(mods[i].name, bn) == 0) {
                    uintptr_t h = ps::ps_symbols_open(mods[i].path, mods[i].base);
                    fprintf(stderr, "[smoke] 符号表句柄 = %p\n", (void *)h);
                    int cnt = 0;
                    uint32_t rva = 0;
                    const char *nm = 0;
                    while (ps::ps_symbols_get(h, cnt, &rva, &nm)) cnt++;
                    fprintf(stderr, "[smoke] 解析到 %d 个符号\n", cnt);
                    fflush(stderr);
                    break;
                }
            }
        }
    }

    // ---- 7. 单次延迟 + 归因（这一段是平台层的确定性验证）----
    phase("延迟与归因验证");
    {
        // 忙线程已经在上面起好了（它的代码在【本程序自己的 .text】里，显式导出 + noinline），
        // 所以采到的样本必须能归属回本程序这个模块。
        // 本程序的模块范围
        static ps::ModuleInfo mods[512];
        ps::ModuleVisits mv;
        mv.items = mods;
        mv.count = 0;
        mv.capacity = 512;
        ps::ps_enum_modules(&mv);
        uintptr_t self_base = 0;
        size_t self_size = 0;
        for (int i = 0; i < mv.count; ++i) {
            if (strstr(mods[i].name, "perfstat_smoke") != NULL) {
                self_base = mods[i].base;
                self_size = mods[i].size;
                break;
            }
        }
        CHECK_TRUE(self_base != 0, "定位到本程序的模块范围");

        uintptr_t ips[64];
        uint32_t tids[64];
        int total = 0, in_self = 0, slow = 0;
        double worst = 0.0;
        for (int round = 0; round < 100; ++round) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int n = ps::ps_sample_threads(ips, tids, 64);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
            if (ms > worst) worst = ms;
            if (ms > 15.0) slow++;
            for (int i = 0; i < n; ++i) {
                total++;
                if (self_base && ips[i] >= self_base && ips[i] < self_base + self_size) in_self++;
            }
            usleep(5000);
        }
        fprintf(stderr,
                "[smoke] 采样 100 轮：样本 %d，落在本模块 %d；单次最慢 %.2f ms，>15ms 的 %d 轮\n",
                total, in_self, worst, slow);
        CHECK_TRUE(total > 50, "连续采样能采到足够样本");
        CHECK_TRUE(in_self > 0, "样本能正确归属到本程序模块（信号采样+归因都正常）");
        CHECK_TRUE(slow == 0, "没有一轮采样卡在 15ms 以上（信号处理器正常回填槽位）");
    }

    stop_busy_thread(g_busy_thread);

    alarm(0);
    fprintf(stderr, "[smoke] 全部阶段完成，本地断言失败 %d 项，CHECK_TRUE 失败 %d 项\n", fails,
            g_fails);
    fflush(stderr);
    return (fails == 0 && g_fails == 0) ? 0 : 1;
}
