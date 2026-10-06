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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include "../src/platform.h"

static volatile const char *g_phase = "start";
static int g_timeout_sec = 60;

static const char kTimeoutPrefix[] = "\n[smoke] 超时！卡在阶段: ";

static void on_alarm(int) {
    // 只用异步信号安全的东西（write 是异步信号安全的，printf 不是）
    const char *p = g_phase;
    (void)!write(2, kTimeoutPrefix, sizeof(kTimeoutPrefix) - 1);
    (void)!write(2, p, strlen(p));
    (void)!write(2, "\n", 1);
    _exit(3);
}

static void phase(const char *p) {
    g_phase = p;
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

    // ---- 3. 线程枚举 ----
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
        fflush(stderr);
    }

    // ---- 4. 采样：单次 ----
    phase("ps_sample_threads 单次");
    {
        uintptr_t ips[64];
        uint32_t tids[64];
        int n = ps::ps_sample_threads(ips, tids, 64);
        fprintf(stderr, "[smoke] OK: 单次采样返回 %d 个样本\n", n);
        for (int i = 0; i < n && i < 5; ++i) {
            fprintf(stderr, "         ip=%p tid=%u\n", (void *)ips[i], tids[i]);
        }
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
        fprintf(stderr, "[smoke] OK: 100 轮共 %d 个样本（非空 %d）\n", total, nonnull);
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

    alarm(0);
    fprintf(stderr, "[smoke] 全部阶段完成，失败 %d 项\n", fails);
    fflush(stderr);
    return fails == 0 ? 0 : 1;
}
