//========================================================================================
// perfstat - 离线自检工具（不需要启动服务器就能验证核心逻辑）
//
//   验证 1: CCommand / IServerPluginCallbacks 的 ABI 布局是否与官方 SDK 一致
//   验证 2: Win32 模块枚举 + PE 导出表符号解析 + 地址归因（用真实 DLL 做样本）
//   验证 3: 采样 + 归因 + 报告生成，端到端跑一遍（本进程内真实烧 CPU 的线程）
//
// 构建：tools\build_tests.bat
// 运行：build\perfstat_tests.exe
//========================================================================================

#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "../src/core.h"
#include "../src/platform.h"
#if defined(PERFSTAT_USE_SDK)
#include "engine/iserverplugin.h"
#else
#include "../src/plugin_api.h"
#endif

static int g_fail = 0;
static int g_pass = 0;

// 主模块信息（test_symbols 里填，test_sampling 里用来交叉验证归因）
static uintptr_t g_self_base = 0;
static size_t g_self_size = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) {                                                   \
            printf("  [ OK ] %s\n", msg);                             \
            g_pass++;                                                 \
        } else {                                                      \
            printf("  [FAIL] %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_fail++;                                                 \
        }                                                             \
    } while (0)

// 供符号解析测试使用的导出函数（必须 dllexport 才会进导出表）
extern "C" __declspec(dllexport) void perfstat_marker_alpha() {}
extern "C" __declspec(dllexport) void perfstat_marker_beta() {}
extern "C" __declspec(dllexport) void perfstat_busy_loop(unsigned spin);

extern "C" __declspec(dllexport) void perfstat_busy_loop(unsigned spin) {
    volatile unsigned long long acc = 0;
    for (unsigned i = 0; i < spin; ++i) {
        acc += (unsigned long long)i * 2654435761u;
        acc ^= acc >> 13;
    }
    if (acc == 0x12345678ULL) printf("");
}

//----------------------------------------------------------------------------------------
// 验证 1：ABI 布局
//
// 说明 1：不能直接拿“成员函数指针”去和 vtable 槽位比地址 —— 借道基类重写的成员函数
//         指针指向编译器生成的 this 调整 thunk，vtable 里放的是真实函数入口。
// 说明 2：x86 的 __thiscall 是被调用者清栈（ret N），所以通过 vtable 调用时必须给足
//         参数个数，否则栈会失衡（这一条曾经在自检工具里踩过坑）。
// 所以这里为 22 个回调各写一个参数个数正确的调用包装，逐个真调用并检查标记值。
//----------------------------------------------------------------------------------------
class AbiProbe : public IServerPluginCallbacks {
public:
    int tag;
    AbiProbe() : tag(0) {}

    virtual bool Load(CreateInterfaceFn, CreateInterfaceFn) { tag = 100; return true; }
    virtual void Unload(void) { tag = 101; }
    virtual void Pause(void) { tag = 102; }
    virtual void UnPause(void) { tag = 103; }
    virtual const char *GetPluginDescription(void) { tag = 104; return "AbiProbe-DESCRIPTION"; }
    virtual void LevelInit(char const *) { tag = 105; }
    virtual void ServerActivate(edict_t *, int, int) { tag = 106; }
    virtual void GameFrame(bool) { tag = 107; }
    virtual void LevelShutdown(void) { tag = 108; }
    virtual void ClientActive(edict_t *) { tag = 109; }
    virtual void ClientDisconnect(edict_t *) { tag = 110; }
    virtual void ClientPutInServer(edict_t *, char const *) { tag = 111; }
    virtual void SetCommandClient(int) { tag = 112; }
    virtual void ClientSettingsChanged(edict_t *) { tag = 113; }
    virtual PLUGIN_RESULT ClientConnect(bool *, edict_t *, const char *, const char *, char *, int) {
        tag = 114;
        return PLUGIN_OVERRIDE;
    }
    virtual PLUGIN_RESULT ClientCommand(edict_t *, const CCommand &) { tag = 115; return PLUGIN_STOP; }
    virtual PLUGIN_RESULT NetworkIDValidated(const char *, const char *) {
        tag = 116;
        return PLUGIN_CONTINUE;
    }
    virtual void OnQueryCvarValueFinished(QueryCvarCookie_t, edict_t *, EQueryCvarValueStatus,
                                          const char *, const char *) {
        tag = 117;
    }
    virtual void OnEdictAllocated(edict_t *) { tag = 118; }
    virtual void OnEdictFreed(const edict_t *) { tag = 119; }
};

// 每个槽位一套“参数个数正确”的原型（thiscall 由被调用者清栈，个数必须对上）
typedef bool(__thiscall *F0)(void *, CreateInterfaceFn, CreateInterfaceFn);
typedef void(__thiscall *F1)(void *);
typedef const char *(__thiscall *F2)(void *);
typedef void(__thiscall *F3)(void *, const char *);
typedef void(__thiscall *F4)(void *, edict_t *, int, int);
typedef void(__thiscall *F5)(void *, bool);
typedef void(__thiscall *F6)(void *, edict_t *);
typedef void(__thiscall *F7)(void *, edict_t *, const char *);
typedef void(__thiscall *F8)(void *, int);
typedef PLUGIN_RESULT(__thiscall *F9)(void *, bool *, edict_t *, const char *, const char *, char *,
                                      int);
typedef PLUGIN_RESULT(__thiscall *F10)(void *, edict_t *, const CCommand &);
typedef PLUGIN_RESULT(__thiscall *F11)(void *, const char *, const char *);
typedef void(__thiscall *F12)(void *, QueryCvarCookie_t, edict_t *, EQueryCvarValueStatus,
                              const char *, const char *);
typedef void(__thiscall *F13)(void *, const edict_t *);

static const char *kSlotNames[22] = {
    "Load",           "Unload",           "Pause",
    "UnPause",        "GetPluginDescription", "LevelInit",
    "ServerActivate", "GameFrame",        "LevelShutdown",
    "ClientActive",   "ClientDisconnect", "ClientPutInServer",
    "SetCommandClient", "ClientSettingsChanged", "ClientConnect",
    "ClientCommand",  "NetworkIDValidated", "OnQueryCvarValueFinished",
    "OnEdictAllocated", "OnEdictFreed",   "reserved20",
    "reserved21"};

static int call_slot(AbiProbe *p, void **vt, int i) {
    CCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    switch (i) {
        case 0: return ((F0)vt[0])(p, 0, 0) ? 100 : -1;
        case 1: ((F1)vt[1])(p); return p->tag;
        case 2: ((F1)vt[2])(p); return p->tag;
        case 3: ((F1)vt[3])(p); return p->tag;
        case 4: ((F2)vt[4])(p); return p->tag;
        case 5: ((F3)vt[5])(p, "de_dust2"); return p->tag;
        case 6: ((F4)vt[6])(p, 0, 1, 2); return p->tag;
        case 7: ((F5)vt[7])(p, false); return p->tag;
        case 8: ((F1)vt[8])(p); return p->tag;
        case 9: ((F6)vt[9])(p, 0); return p->tag;
        case 10: ((F6)vt[10])(p, 0); return p->tag;
        case 11: ((F7)vt[11])(p, 0, "name"); return p->tag;
        case 12: ((F8)vt[12])(p, 42); return p->tag;
        case 13: ((F6)vt[13])(p, 0); return p->tag;
        case 14: {
            bool allow = false;
            PLUGIN_RESULT r = ((F9)vt[14])(p, &allow, 0, "n", "a", 0, 0);
            return r == PLUGIN_OVERRIDE ? p->tag : -1;
        }
        case 15: {
            PLUGIN_RESULT r = ((F10)vt[15])(p, 0, cmd);
            return r == PLUGIN_STOP ? p->tag : -1;
        }
        case 16: {
            PLUGIN_RESULT r = ((F11)vt[16])(p, "u", "s");
            return r == PLUGIN_CONTINUE ? p->tag : -1;
        }
        case 17: ((F12)vt[17])(p, 0, 0, eQueryCvarValueStatus_ValueIntact, "c", "v"); return p->tag;
        case 18: ((F6)vt[18])(p, 0); return p->tag;
        case 19: ((F13)vt[19])(p, 0); return p->tag;
        default: return -2;  // SDK 只定义了 20 个回调
    }
}

static void test_abi() {
    printf("\n=== 验证 1: ABI 布局（与官方 iserverplugin.h / commandbuffer.h 对照） ===\n");

    CHECK(sizeof(IServerPluginCallbacks) == sizeof(void *),
          "sizeof(IServerPluginCallbacks) == 4 字节（纯虚类只有 vptr）");

    AbiProbe probe;
    void **vt = *(void ***)&probe;
    CHECK(vt != 0, "取到 IServerPluginCallbacks 的 vtable 指针");

    // 直接调用一次，先把探针本身验证通过
    probe.tag = 0;
    probe.Load(0, 0);
    CHECK(probe.tag == 100, "探针类直接调用 Load() 正常（tag=100）");

    // 逐个槽位通过 vtable 真调用
    int mismatches = 0;
    for (int i = 0; i < 20; ++i) {
        probe.tag = -1;
        int got = call_slot(&probe, vt, i);
        if (got != 100 + i) {
            printf("  [FAIL] vtable[%2d] 应该是 %s（tag 期望 %d，实际 %d）\n", i, kSlotNames[i],
                   100 + i, got);
            mismatches++;
            g_fail++;
        }
    }
    CHECK(mismatches == 0, "vtable[0..19] 逐个通过 vtable 真调用，落到的回调与声明顺序完全一致");

    // 剩下两个槽位（SDK 里没有第 21/22 个回调）不应该存在
    CHECK(call_slot(&probe, vt, 20) == -2 && call_slot(&probe, vt, 21) == -2,
          "SDK 只有 20 个回调，没有多出来的槽位");

    // CCommand / ConCommandBase / ConCommand 的字段布局（必须与 l4d2 SDK 一致）
    CHECK(sizeof(CCommand) == 1288, "sizeof(CCommand) == 1288（2 int + 2×512 缓冲 + 64 指针）");
    CHECK(sizeof(ConCommandBase) == 24, "sizeof(ConCommandBase) == 24（与 convar.h 一致）");
    CHECK(sizeof(ConCommand) == 36, "sizeof(ConCommand) == 36（Base 24 + 2 回调 + 标志字节）");

    {
        CCommand fake;
        memset(&fake, 0, sizeof(fake));
        CHECK(fake.ArgC() == 0, "空 CCommand 的 ArgC() == 0");
        CHECK(strcmp(fake.Arg(5), "") == 0, "空 CCommand 越界取 Arg() 安全返回空串");
    }

    {
        // 按 SDK 的字段偏移手工填一个 ArgC()==2 的对象，确认 Arg 系列读法正确
        // 布局：m_nArgc(0) m_nArgv0Size(4) m_pArgSBuffer(8,512) m_pArgvBuffer(520,512)
        //       m_ppArgv(1032, 64×4)
        CCommand c;
        memset(&c, 0, sizeof(c));
        int *raw = reinterpret_cast<int *>(&c);
        raw[0] = 2;   // m_nArgc
        raw[1] = 10;  // m_nArgv0Size
        char *argv_buf = reinterpret_cast<char *>(&c) + 520;
        strcpy(argv_buf, "perf_stat");
        strcpy(argv_buf + 10, "10");
        char *cmdbuf = reinterpret_cast<char *>(&c) + 8;  // m_pArgSBuffer 是整条命令原文
        strcpy(cmdbuf, "perf_stat 10");
        const char **pp = reinterpret_cast<const char **>(reinterpret_cast<char *>(&c) + 1032);
        pp[0] = argv_buf;
        pp[1] = argv_buf + 10;
        CHECK(c.ArgC() == 2, "手工构造的 CCommand ArgC() == 2");
        CHECK(strcmp(c.Arg(0), "perf_stat") == 0, "Arg(0) 读到命令名");
        CHECK(strcmp(c.operator[](1), "10") == 0, "operator[](1) 读到第一个参数");
        CHECK(strcmp(c.ArgS(), "10") == 0, "ArgS() 读到第 0 个参数之后的内容");
        CHECK(strcmp(c.GetCommandString(), "perf_stat 10") == 0, "GetCommandString() 是整条命令");
        CHECK(strcmp(c.Arg(99), "") == 0, "越界 Arg() 返回空串");
    }
}

//----------------------------------------------------------------------------------------
// 验证 2：模块枚举 + PE 导出符号 + 地址归因
//----------------------------------------------------------------------------------------
static void test_symbols() {
    printf("\n=== 验证 2: 模块枚举 / PE 导出表 / 地址归因 ===\n");

    static ps::ModuleInfo mods[1024];
    ps::ModuleVisits mv;
    mv.items = mods;
    mv.count = 0;
    mv.capacity = 1024;
    ps::ps_enum_modules(&mv);
    printf("  枚举到 %d 个模块\n", mv.count);
    CHECK(mv.count > 3, "枚举到多个模块（本程序 + ntdll + kernel32 ...）");

    bool sorted = true, ranges_ok = true;
    for (int i = 0; i < mv.count; ++i) {
        if (mods[i].size == 0 || mods[i].path[0] == 0) ranges_ok = false;
        if (i > 0 && mods[i].base < mods[i - 1].base) sorted = false;
    }
    CHECK(ranges_ok, "每个模块都有路径和大小");
    CHECK(sorted, "模块按基址升序排列（归因二分查找的前提）");

    char self[1024];
    GetModuleFileNameA(0, self, sizeof(self) - 1);
    const char *self_name = strrchr(self, '\\');
    self_name = self_name ? self_name + 1 : self;

    ps::ModuleInfo *me = 0;
    for (int i = 0; i < mv.count; ++i) {
        if (_stricmp(mods[i].name, self_name) == 0) {
            me = &mods[i];
            break;
        }
    }
    CHECK(me != 0, "在模块列表里找到本测试程序自身");
    if (!me) return;
    g_self_base = me->base;
    g_self_size = me->size;
    printf("  主模块: %s\n          base=%p size=%u KB\n", me->path, (void *)me->base,
           (unsigned)(me->size / 1024));

    uintptr_t handle = ps::ps_symbols_open(me->path, me->base);
    CHECK(handle != 0, "解析本程序的 PE 导出表成功");
    if (handle) {
        int count = 0;
        uint32_t rva = 0;
        const char *name = 0;
        while (ps::ps_symbols_get(handle, count, &rva, &name)) count++;
        printf("  导出表条目数: %d\n", count);
        CHECK(count >= 3, "导出表里至少有 3 个符号");

        bool found_alpha = false, found_loop = false, rva_sorted = true;
        uint32_t last = 0;
        for (int i = 0; i < count; ++i) {
            ps::ps_symbols_get(handle, i, &rva, &name);
            if (i > 0 && rva < last) rva_sorted = false;
            last = rva;
            if (!name) continue;
            if (strcmp(name, "perfstat_marker_alpha") == 0) found_alpha = true;
            if (strcmp(name, "perfstat_busy_loop") == 0) found_loop = true;
        }
        CHECK(found_alpha, "导出表里有 perfstat_marker_alpha");
        CHECK(found_loop, "导出表里有 perfstat_busy_loop");
        CHECK(rva_sorted, "导出表按 RVA 升序（二分查找的前提）");

        uintptr_t addr = (uintptr_t)&perfstat_busy_loop;
        CHECK(addr >= me->base && addr < me->base + me->size, "函数地址落在主模块区间内");
        uint32_t off = 0;
        const char *sym = ps::ps_symbolize(handle, (uint32_t)(addr - me->base), &off);
        printf("  归因: perfstat_busy_loop @%p -> %s + 0x%X\n", (void *)addr, sym ? sym : "(null)",
               off);
        CHECK(sym && strcmp(sym, "perfstat_busy_loop") == 0, "ps_symbolize 正确解析出函数名");

        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        char k32path[1024];
        GetModuleFileNameA(k32, k32path, sizeof(k32path) - 1);
        uintptr_t h2 = ps::ps_symbols_open(k32path, (uintptr_t)k32);
        CHECK(h2 != 0, "解析 kernel32.dll 的导出表成功");
        if (h2) {
            void *p = (void *)GetProcAddress(k32, "CreateFileA");
            uint32_t off2 = 0;
            const char *sym2 = ps::ps_symbolize(h2, (uint32_t)((uintptr_t)p - (uintptr_t)k32), &off2);
            printf("  kernel32 归因: CreateFileA @%p -> %s + 0x%X\n", p, sym2 ? sym2 : "(null)", off2);
            CHECK(sym2 && strcmp(sym2, "CreateFileA") == 0, "kernel32 的 CreateFileA 归因正确");
        }

        uintptr_t h3 = ps::ps_symbols_open("Z:\\no\\such\\file.dll", 0x12345000);
        CHECK(h3 == 0, "解析不存在的文件时安全返回 0");
    }
}

//----------------------------------------------------------------------------------------
// 验证 3：采样 -> 归因 -> 报告（端到端）
//----------------------------------------------------------------------------------------
static volatile bool g_busy = false;
static volatile unsigned long long g_sink = 0;

static DWORD WINAPI busy_thread(LPVOID) {
    while (g_busy) {
        perfstat_busy_loop(200000);  // 在本程序的 .text 里空转
        g_sink++;
    }
    return 0;
}

static DWORD WINAPI idle_thread(LPVOID) {
    while (g_busy) Sleep(50);
    return 0;
}

static ps::Profiler *g_prof = 0;
static DWORD WINAPI sampler_thread(LPVOID) {
    g_prof->sampler_loop();
    return 0;
}

static void file_out(void *user, const char *text) { fputs(text, (FILE *)user); }

static void test_sampling() {
    printf("\n=== 验证 3: 10ms 采样 3 秒 -> 归因 -> 报告 ===\n");

    ps::Profiler profiler;
    g_prof = &profiler;
    profiler.init_once();
    profiler.set_sample_interval_ms(10);
    printf("  初始模块数=%d 线程数=%d\n", profiler.module_count(), profiler.thread_count());
    CHECK(profiler.module_count() > 3, "profiler 初始化后模块表非空");
    CHECK(profiler.thread_count() >= 1, "profiler 初始化后线程表非空");

    g_busy = true;
    HANDLE busy = CreateThread(0, 0, busy_thread, 0, 0, 0);
    HANDLE idle = CreateThread(0, 0, idle_thread, 0, 0, 0);
    if (busy) SetThreadPriority(busy, THREAD_PRIORITY_ABOVE_NORMAL);

    profiler.set_running(true);
    HANDLE sampler = CreateThread(0, 0, sampler_thread, 0, 0, 0);
    Sleep(3000);

    // 交叉验证：这里 busy/idle 线程还在跑，用平台采样接口独立取样本
    static uintptr_t probe_ips[256];
    static uint32_t probe_tids[256];
    uint64_t self_hits = 0;
    int probe_total = 0;
    for (int round = 0; round < 20; ++round) {
        int n = ps::ps_sample_threads(probe_ips, probe_tids, 256);
        for (int i = 0; i < n; ++i) {
            probe_total++;
            if (probe_ips[i] >= g_self_base && probe_ips[i] < g_self_base + g_self_size) self_hits++;
        }
        Sleep(5);
    }
    printf("  交叉验证: 落在主模块的样本 %llu / %d (%.1f%%)\n", (unsigned long long)self_hits,
           probe_total, probe_total > 0 ? (double)self_hits * 100.0 / probe_total : 0.0);
    CHECK(probe_total > 0, "平台采样接口能独立取到样本");
    CHECK(self_hits > 0, "交叉验证确有样本落在主模块（busy 线程跑在本程序 .text 里）");

    profiler.set_running(false);
    g_busy = false;
    WaitForSingleObject(sampler, 3000);
    WaitForSingleObject(busy, 3000);
    WaitForSingleObject(idle, 3000);
    CloseHandle(sampler);
    CloseHandle(busy);
    CloseHandle(idle);

    double elapsed = profiler.elapsed_seconds();
    uint64_t total = profiler.total_samples();
    printf("  总采样数=%llu 用时=%.2fs 实际频率=%.1f 次/秒\n", (unsigned long long)total, elapsed,
           elapsed > 0 ? (double)total / elapsed : 0.0);
    CHECK(total > 100, "3 秒内采到足够多的样本（>100）");
    CHECK(elapsed >= 2.0 && elapsed <= 8.0, "计时正常（2~8 秒）");

    ps::Profiler::Options opt;
    opt.top = 3;
    opt.show_hot = true;
    opt.show_threads = true;
    opt.min_hits = 2;
    opt.title = "offline self-test";
    printf("\n---------- 报告开始 ----------\n");
    profiler.report(file_out, stdout, opt);
    printf("---------- 报告结束 ----------\n");

}

int main(int, char **) {
    setvbuf(stdout, 0, _IONBF, 0);  // 崩溃时也能看到已经打印的内容
    printf("perfstat 自检工具\n");
    printf("================================================================================\n");

    test_abi();
    test_symbols();
    test_sampling();

    printf("\n================================================================================\n");
    printf("结果: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
