//========================================================================================
// perfstat - Windows 平台实现
//
//   * 模块枚举      : Toolhelp32 (Module32First/Next)
//   * 线程采样      : Toolhelp32 枚举本进程线程 +
//                     SuspendThread + GetThreadContext 读取 EIP，取完立刻 ResumeThread
//   * 内存归因      : VirtualQueryEx 遍历地址空间 + GetMappedFileName 归属到具体 DLL
//                     （GetMappedFileName 只能拿到 \Device\HarddiskVolumeX\... 形式，
//                       这里用 QueryDosDevice 建前缀表转回盘符路径）
//                     驻留判定用 QueryWorkingSetEx（psapi.dll，动态加载）
//   * 符号解析      : 直接解析 PE 导出表（.edata），不依赖 dbghelp / PDB
//========================================================================================

#if !defined(_WIN32)
#error "perf_platform_win32.cpp should only be compiled on Windows"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "platform.h"

namespace ps {

namespace {

// 打开线程时的权限：
//   THREAD_QUERY_INFORMATION  -> GetThreadId
//   THREAD_SUSPEND_RESUME     -> SuspendThread / ResumeThread
//   THREAD_GET_CONTEXT        -> GetThreadContext / Wow64GetThreadContext
//   少了 THREAD_GET_CONTEXT 会直接失败并返回 ERROR_ACCESS_DENIED(5)，
//   结果就是“一个样本都取不到”但也不报错。
#define PERFSTAT_THREAD_ACCESS \
    (THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT)

//----------------------------------------------------------------------------------------
// 本进程句柄 / 环境
//----------------------------------------------------------------------------------------
HANDLE g_process = 0;
bool g_wow64 = false;
bool g_inited = false;

// 平台层自己的诊断开关（与 Linux 侧接口一致）
static volatile int g_ps_debug = 0;

// "尽快停止采样"标志（与 Linux 侧语义一致）
// 这个文件不包含 compat.h（它是纯 C 风格的平台实现），所以直接用 MSVC 内部函数。
static volatile long g_stop_sampling = 0;
static inline long stop_flag_get(void) {
    return _InterlockedCompareExchange(const_cast<volatile long *>(&g_stop_sampling), 0, 0);
}
static inline void stop_flag_set(long v) {
    _InterlockedExchange(const_cast<volatile long *>(&g_stop_sampling), v);
}

typedef BOOL(WINAPI *QueryWorkingSetExFn)(HANDLE, PVOID, DWORD);

QueryWorkingSetExFn g_QueryWorkingSetEx = 0;

//----------------------------------------------------------------------------------------
// 导出表符号
//----------------------------------------------------------------------------------------
struct SymEntry {
    uint32_t rva;
    uint32_t name_index;
};

struct SymModule {
    uintptr_t base;
    std::string path;
    std::vector<SymEntry> entries;  // 按 rva 升序
    std::vector<std::string> names;
};

std::vector<SymModule *> g_sym_modules;

//----------------------------------------------------------------------------------------
// 内存遍历缓存
//----------------------------------------------------------------------------------------
const int kChunkPages = 8192;  // 一次查 8192 页 = 32MB
PSAPI_WORKING_SET_EX_INFORMATION *g_ws_info = 0;

//----------------------------------------------------------------------------------------
// 小工具
//----------------------------------------------------------------------------------------
const char *ps_basename(const char *p) {
    const char *s1 = strrchr(p, '\\');
    const char *s2 = strrchr(p, '/');
    const char *s = (s1 && (!s2 || s1 > s2)) ? s1 : s2;
    return s ? s + 1 : p;
}

bool ps_wide_to_utf8(const wchar_t *w, char *out, size_t out_size) {
    if (!w || !out || out_size == 0) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)out_size, 0, 0);
    if (n <= 0) {
        out[0] = 0;
        return false;
    }
    out[out_size - 1] = 0;
    return true;
}

// UTF-8 -> UTF-16。注意：不能在 {Get,Create,Load}FileA 里传中文路径（会变成
// ERROR_PATH_NOT_FOUND），所以所有文件操作都要用宽字符版本。
bool ps_utf8_to_wide(const char *s, wchar_t *out, size_t out_chars) {
    if (!s || !out || out_chars == 0) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)out_chars);
    if (n <= 0) {
        out[0] = 0;
        return false;
    }
    out[out_chars - 1] = 0;
    return true;
}

// 把 \Device\HarddiskVolumeX\... 转成 C:\...
std::vector<std::pair<std::string, std::string> > g_device_map;

void build_device_map() {
    if (!g_device_map.empty()) return;
    char drives[512];
    DWORD n = GetLogicalDriveStringsA(sizeof(drives) - 1, drives);
    if (!n) return;
    for (char *p = drives; *p; p += strlen(p) + 1) {
        char letter[4] = {p[0], ':', 0, 0};
        char target[1024];
        if (QueryDosDeviceA(letter, target, sizeof(target))) {
            std::string dev(target);
            std::string dos(letter);
            g_device_map.push_back(std::make_pair(dev, dos));
        }
    }
}

void normalize_device_path(const char *in, char *out, size_t out_size) {
    if (!in || !in[0]) {
        out[0] = 0;
        return;
    }
    if (in[0] != '\\') {
        strncpy(out, in, out_size - 1);
        out[out_size - 1] = 0;
        return;
    }
    build_device_map();
    for (size_t i = 0; i < g_device_map.size(); ++i) {
        const std::string &dev = g_device_map[i].first;
        if (dev.size() && _strnicmp(in, dev.c_str(), dev.size()) == 0) {
            snprintf(out, out_size, "%s%s", g_device_map[i].second.c_str(), in + dev.size());
            return;
        }
    }
    strncpy(out, in, out_size - 1);
    out[out_size - 1] = 0;
}

//----------------------------------------------------------------------------------------
// PE 导出表解析
//----------------------------------------------------------------------------------------
bool rva_to_offset(const IMAGE_NT_HEADERS32 *nt, uint32_t rva, DWORD file_size, DWORD *out) {
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(const_cast<IMAGE_NT_HEADERS32 *>(nt));
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        uint32_t va = sec[i].VirtualAddress;
        uint32_t vsize = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        if (rva >= va && rva < va + vsize) {
            DWORD off = sec[i].PointerToRawData + (rva - va);
            if (off >= file_size) return false;
            *out = off;
            return true;
        }
    }
    return false;
}

const char *pe_find_export_name_by_rva(const IMAGE_NT_HEADERS32 *nt, const unsigned char *data,
                                       DWORD file_size, const IMAGE_EXPORT_DIRECTORY *exp,
                                       DWORD func_rva) {
    // 仅用于把转发导出还原成字符串名（一般极少），这里简单线性查找
    DWORD funcs_off = 0, names_off = 0, ords_off = 0;
    if (!rva_to_offset(nt, exp->AddressOfFunctions, file_size, &funcs_off)) return 0;
    if (!rva_to_offset(nt, exp->AddressOfNames, file_size, &names_off)) return 0;
    if (!rva_to_offset(nt, exp->AddressOfNameOrdinals, file_size, &ords_off)) return 0;
    const DWORD *funcs = (const DWORD *)(data + funcs_off);
    const DWORD *names = (const DWORD *)(data + names_off);
    const WORD *ords = (const WORD *)(data + ords_off);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        if (ords[i] >= exp->NumberOfFunctions) continue;
        if (funcs[ords[i]] != func_rva) continue;
        DWORD name_off = 0;
        if (!rva_to_offset(nt, names[i], file_size, &name_off)) continue;
        if (name_off >= file_size) continue;
        return (const char *)(data + name_off);
    }
    return 0;
}

SymModule *load_pe_symbols(const char *path, uintptr_t base) {
    wchar_t wpath[1024];
    if (!ps_utf8_to_wide(path, wpath, 1024)) {
        return 0;
    }
    HANDLE f = CreateFileW(wpath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD file_size = GetFileSize(f, 0);
    if (file_size == INVALID_FILE_SIZE || file_size < 0x100) {
        CloseHandle(f);
        return 0;
    }
    HANDLE map = CreateFileMappingW(f, 0, PAGE_READONLY, 0, 0, 0);
    if (!map) {
        CloseHandle(f);
        return 0;
    }
    const unsigned char *data = (const unsigned char *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    if (!data) {
        CloseHandle(map);
        CloseHandle(f);
        return 0;
    }

    SymModule *mod = new SymModule();
    mod->base = base;
    mod->path = path;

    do {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)data;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            break;
        }
        if ((DWORD)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > file_size) {
            break;
        }
        const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(data + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            break;
        }
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            break;
        }

        DWORD exp_rva =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        DWORD exp_size = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        if (!exp_rva || !exp_size) break;

        DWORD exp_off = 0;
        if (!rva_to_offset(nt, exp_rva, file_size, &exp_off)) {
            break;
        }
        if (exp_off + sizeof(IMAGE_EXPORT_DIRECTORY) > file_size) {
            break;
        }

        const IMAGE_EXPORT_DIRECTORY *exp = (const IMAGE_EXPORT_DIRECTORY *)(data + exp_off);
        DWORD funcs_off = 0, names_off = 0, ords_off = 0;
        if (!rva_to_offset(nt, exp->AddressOfFunctions, file_size, &funcs_off)) break;
        if (!rva_to_offset(nt, exp->AddressOfNames, file_size, &names_off)) break;
        if (!rva_to_offset(nt, exp->AddressOfNameOrdinals, file_size, &ords_off)) break;

        const DWORD *funcs = (const DWORD *)(data + funcs_off);
        const DWORD *names = (const DWORD *)(data + names_off);
        const WORD *ords = (const WORD *)(data + ords_off);

        for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
            if (names_off + (i + 1) * 4 > file_size) break;
            if (ords_off + (i + 1) * 2 > file_size) break;
            DWORD name_rva = names[i];
            WORD ord = ords[i];
            if ((DWORD)ord >= exp->NumberOfFunctions) continue;
            if (funcs_off + ((DWORD)ord + 1) * 4 > file_size) continue;
            DWORD func_rva = funcs[ord];
            if (!func_rva) continue;

            DWORD name_off = 0;
            if (!rva_to_offset(nt, name_rva, file_size, &name_off)) continue;
            if (name_off >= file_size) continue;

            if (func_rva >= exp_rva && func_rva < exp_rva + exp_size) {
                // 转发导出：函数体在别的 DLL 里，用 "名字 -> 目标" 记录，避免误判热点
                const char *target = pe_find_export_name_by_rva(nt, data, file_size, exp, func_rva);
                const char *nm = (const char *)(data + name_off);
                size_t max_len = file_size - name_off;
                size_t len = strnlen(nm, max_len);
                if (len == 0 || len == max_len) continue;
                (void)target;
                // 直接跳过转发项（热点统计不需要）
                continue;
            }

            const char *nm = (const char *)(data + name_off);
            size_t max_len = file_size - name_off;
            size_t len = strnlen(nm, max_len);
            if (len == 0 || len == max_len) continue;

            SymEntry e;
            e.rva = func_rva;
            e.name_index = (uint32_t)mod->names.size();
            mod->names.push_back(std::string(nm, len));
            mod->entries.push_back(e);
        }
    } while (0);

    UnmapViewOfFile(data);
    CloseHandle(map);
    CloseHandle(f);

    if (mod->entries.empty()) {
        delete mod;
        return 0;
    }
    std::stable_sort(mod->entries.begin(), mod->entries.end(),
                     [](const SymEntry &a, const SymEntry &b) { return a.rva < b.rva; });
    return mod;
}

}  // namespace

//----------------------------------------------------------------------------------------
// 生命周期
//----------------------------------------------------------------------------------------
bool ps_platform_init() {
    if (g_inited) return true;
    g_inited = true;

    g_process = GetCurrentProcess();
    {
        BOOL wow = FALSE;
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        typedef BOOL(WINAPI * IsWow64ProcessFn)(HANDLE, PBOOL);
        IsWow64ProcessFn fn = k32 ? (IsWow64ProcessFn)GetProcAddress(k32, "IsWow64Process") : 0;
        if (fn && fn(g_process, &wow) && wow) g_wow64 = true;
    }

    // QueryWorkingSetEx 的位置随 Windows 版本变过：
    //   老版本在 psapi.dll；新版本内核导出的名字叫 K32QueryWorkingSetEx（在 kernel32.dll 里）。
    // 两个都试一遍，全都拿不到也没关系 —— ps_walk_memory 会退化成"不区分驻留"，
    // 至少内存量级是对的（之前这里失败会导致内存列全是 0）。
    {
        HMODULE psapi = GetModuleHandleA("psapi.dll");
        if (!psapi) psapi = LoadLibraryA("psapi.dll");
        if (psapi) {
            g_QueryWorkingSetEx = (QueryWorkingSetExFn)GetProcAddress(psapi, "QueryWorkingSetEx");
            if (!g_QueryWorkingSetEx) {
                g_QueryWorkingSetEx = (QueryWorkingSetExFn)GetProcAddress(psapi, "K32QueryWorkingSetEx");
            }
        }
        if (!g_QueryWorkingSetEx) {
            HMODULE k32 = GetModuleHandleA("kernel32.dll");
            if (k32) {
                g_QueryWorkingSetEx = (QueryWorkingSetExFn)GetProcAddress(k32, "K32QueryWorkingSetEx");
                if (!g_QueryWorkingSetEx) {
                    g_QueryWorkingSetEx =
                        (QueryWorkingSetExFn)GetProcAddress(k32, "QueryWorkingSetEx");
                }
            }
        }
    }

    g_ws_info = (PSAPI_WORKING_SET_EX_INFORMATION *)malloc(sizeof(PSAPI_WORKING_SET_EX_INFORMATION) *
                                                           kChunkPages);
    if (!g_ws_info) return false;
    return true;
}

void ps_platform_shutdown() {
    for (size_t i = 0; i < g_sym_modules.size(); ++i) delete g_sym_modules[i];
    g_sym_modules.clear();
    if (g_ws_info) {
        free(g_ws_info);
        g_ws_info = 0;
    }
}

//----------------------------------------------------------------------------------------
// 模块枚举
//----------------------------------------------------------------------------------------
void ps_enum_modules(ModuleVisits *out) {
    if (!g_inited) ps_platform_init();
    if (!out || !out->items) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me;
    memset(&me, 0, sizeof(me));
    me.dwSize = sizeof(me);
    bool first = true;
    if (Module32FirstW(snap, &me)) {
        do {
            if (out->count >= out->capacity) break;
            ModuleInfo &mi = out->items[out->count];
            memset(&mi, 0, sizeof(mi));
            mi.base = (uintptr_t)me.modBaseAddr;
            mi.size = (size_t)me.modBaseSize;
            char path[520];
            if (!ps_wide_to_utf8(me.szExePath, path, sizeof(path))) {
                ps_wide_to_utf8(me.szModule, path, sizeof(path));
            }
            strncpy(mi.path, path, sizeof(mi.path) - 1);
            strncpy(mi.name, ps_basename(mi.path), sizeof(mi.name) - 1);
            mi.is_main = first;
            first = false;
            out->count++;
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);

    // Toolhelp 是按加载顺序给模块的，这里按基址排序，方便上层做二分查找归因
    std::stable_sort(out->items, out->items + out->count,
                     [](const ModuleInfo &a, const ModuleInfo &b) { return a.base < b.base; });
}

//----------------------------------------------------------------------------------------
// 线程枚举
//----------------------------------------------------------------------------------------
void ps_enum_threads(ThreadVisits *out) {
    if (!g_inited) ps_platform_init();
    if (!out || !out->items) return;
    typedef HRESULT(WINAPI * GetThreadDescriptionFn)(HANDLE, PWSTR *);
    static GetThreadDescriptionFn get_desc = 0;
    static bool tried_desc = false;
    if (!tried_desc) {
        tried_desc = true;
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (k32) get_desc = (GetThreadDescriptionFn)GetProcAddress(k32, "GetThreadDescription");
    }

    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    memset(&te, 0, sizeof(te));
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(PERFSTAT_THREAD_ACCESS, FALSE,
                                  te.th32ThreadID);
            if (!h) continue;
            ThreadInfo ti;
            memset(&ti, 0, sizeof(ti));
            ti.tid = te.th32ThreadID;
            ti.priority = GetThreadPriority(h);
            if (get_desc) {
                PWSTR desc = 0;
                if (SUCCEEDED(get_desc(h, &desc)) && desc) {
                    ps_wide_to_utf8(desc, ti.name, sizeof(ti.name));
                    LocalFree(desc);
                }
            }
            if (!ti.name[0]) snprintf(ti.name, sizeof(ti.name), "Thread-%u", (unsigned)te.th32ThreadID);
            if (out->count < out->capacity) out->items[out->count++] = ti;
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

//----------------------------------------------------------------------------------------
// 采样：逐线程 Suspend -> GetThreadContext(EIP) -> Resume
//----------------------------------------------------------------------------------------
// 抓一次“本进程所有线程 ID”
static int collect_process_tids(DWORD *tids, int capacity) {
    if (!tids || capacity <= 0) return 0;
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    memset(&te, 0, sizeof(te));
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (count >= capacity) break;
            tids[count++] = te.th32ThreadID;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return count;
}

// 挂起一个线程并取出它当前正在执行的指令地址（EIP）。
// 无论成败都保证恢复到原来的挂起计数，否则服务器会卡死。
static uintptr_t peek_thread_ip(DWORD tid) {
    uintptr_t ip = 0;
    HANDLE h = OpenThread(PERFSTAT_THREAD_ACCESS, FALSE, tid);
    if (!h) return 0;

    DWORD prev = SuspendThread(h);
    if (prev == (DWORD)-1) {
        CloseHandle(h);
        return 0;
    }

    if (g_wow64) {
        WOW64_CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = WOW64_CONTEXT_CONTROL;
        typedef BOOL(WINAPI * Wow64GetThreadContextFn)(HANDLE, PWOW64_CONTEXT);
        static Wow64GetThreadContextFn fn = 0;
        static bool tried = false;
        if (!tried) {
            tried = true;
            HMODULE k32 = GetModuleHandleA("kernel32.dll");
            if (k32) fn = (Wow64GetThreadContextFn)GetProcAddress(k32, "Wow64GetThreadContext");
        }
        if (fn && fn(h, &ctx)) {
            ip = (uintptr_t)ctx.Eip;
        } else {
            // 极端情况下 Wow64 接口不可用，退回普通 CONTEXT
            CONTEXT c2;
            memset(&c2, 0, sizeof(c2));
            c2.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &c2)) ip = (uintptr_t)c2.Eip;
        }
    } else {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(h, &ctx)) ip = (uintptr_t)ctx.Eip;
    }

    ResumeThread(h);
    CloseHandle(h);
    return ip;
}

static int sample_tids(const DWORD *tids, int count, uintptr_t *ips, uint32_t *out_tids, int max_ips) {
    const DWORD self = GetCurrentThreadId();
    int n = 0;
    for (int i = 0; i < count && n < max_ips; ++i) {
        if (stop_flag_get()) break;  // 有人请求停止就尽快收手
        if (tids[i] == self || tids[i] == 0) continue;  // 不挂起采样线程自己
        uintptr_t ip = peek_thread_ip(tids[i]);
        if (!ip) continue;
        ips[n] = ip;
        if (out_tids) out_tids[n] = (uint32_t)tids[i];
        n++;
    }
    return n;
}

int ps_sample_threads(uintptr_t *ips, uint32_t *tids, int max_ips) {
    if (!g_inited) ps_platform_init();  // 兜底：确保进程句柄等已就绪
    if (!ips || max_ips <= 0) return 0;

    static DWORD tid_buf[4096];
    int count = collect_process_tids(tid_buf, 4096);
    if (count > max_ips) count = max_ips;
    return sample_tids(tid_buf, count, ips, tids, max_ips);
}

int ps_sample_threads_window(uintptr_t *ips, uint32_t *tids, int max_ips, int *cursor, int window) {
    if (!g_inited) ps_platform_init();
    if (!ips || max_ips <= 0) return 0;

    static DWORD tid_buf[4096];
    int count = collect_process_tids(tid_buf, 4096);
    if (count <= 0) return 0;
    if (window <= 0 || window > max_ips) window = max_ips;

    // 线程不多时直接全抓（等价于 ps_sample_threads，但省一次枚举）
    if (count <= window) {
        if (cursor) *cursor = 0;
        return sample_tids(tid_buf, count, ips, tids, max_ips);
    }

    // 滑动窗口：每轮只挂起 window 个线程，轮转覆盖，整体统计依然是均匀的
    int start = cursor ? (*cursor % count) : 0;
    int n = 0;
    for (int k = 0; k < window && n < max_ips; ++k) {
        int idx = (start + k) % count;
        const DWORD self = GetCurrentThreadId();
        if (tid_buf[idx] == self || tid_buf[idx] == 0) continue;
        uintptr_t ip = peek_thread_ip(tid_buf[idx]);
        if (!ip) continue;
        ips[n] = ip;
        if (tids) tids[n] = (uint32_t)tid_buf[idx];
        n++;
    }
    if (cursor) *cursor = (start + window) % count;
    return n;
}

//----------------------------------------------------------------------------------------
// 内存归因
//----------------------------------------------------------------------------------------
void ps_walk_memory(ModuleLookupFn lookup, void *user, ModuleInfo *mods, int mod_count) {
    if (!g_inited) ps_platform_init();
    if (!g_process || !mods || mod_count <= 0) return;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t page_size = si.dwPageSize ? si.dwPageSize : 4096;

    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
    const uintptr_t max_addr = (uintptr_t)si.lpMaximumApplicationAddress;

    while (addr < max_addr) {
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T got = VirtualQueryEx(g_process, (LPCVOID)addr, &mbi, sizeof(mbi));
        if (got != sizeof(mbi)) break;

        const uintptr_t region_base = (uintptr_t)mbi.BaseAddress;
        const size_t region_size = (size_t)mbi.RegionSize;
        if (region_size == 0) break;

        if (mbi.State == MEM_COMMIT) {
            const bool is_file = (mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED);

            char mapped_path[600];
            mapped_path[0] = 0;
            if (is_file) {
                // 用宽字符版本 + 手动转 UTF-8，中文路径也能正确处理
                wchar_t wbuf[600];
                DWORD n = GetMappedFileNameW(g_process, (LPVOID)region_base, wbuf, 599);
                if (n) {
                    wbuf[n] = 0;
                    char ansi_path[900];
                    if (ps_wide_to_utf8(wbuf, ansi_path, sizeof(ansi_path))) {
                        normalize_device_path(ansi_path, mapped_path, sizeof(mapped_path));
                    }
                }
            }

            const int region_idx = lookup ? lookup(region_base, user) : -1;

            uintptr_t p = region_base;
            const uintptr_t region_end = region_base + region_size;
            while (p < region_end) {
                size_t pages = (region_end - p) / page_size;
                if (pages == 0) pages = 1;
                if (pages > (size_t)kChunkPages) pages = (size_t)kChunkPages;

                bool have_ws = false;
                if (g_QueryWorkingSetEx) {
                    for (size_t k = 0; k < pages; ++k) {
                        g_ws_info[k].VirtualAddress = (PVOID)(p + k * page_size);
                    }
                    have_ws = g_QueryWorkingSetEx(
                                  g_process, g_ws_info,
                                  (DWORD)(pages * sizeof(PSAPI_WORKING_SET_EX_INFORMATION))) != 0;
                }

                for (size_t k = 0; k < pages; ++k) {
                    int idx = lookup ? lookup(p + k * page_size, user) : -1;
                    if (idx < 0) idx = region_idx;
                    if (idx < 0 || idx >= mod_count) continue;
                    ModuleInfo &mi = mods[idx];

                    // 拿不到工作集信息时（QueryWorkingSetEx 不可用）不能把整页算成
                    // "非驻留"，否则内存列会全是 0。这种情况按"已提交"归类到 mapped/
                    // private，至少量级是对的。
                    bool resident = true;
                    if (have_ws) resident = (g_ws_info[k].VirtualAttributes.Valid != 0);
                    if (!resident) {
                        mi.other_bytes += page_size;
                    } else if (is_file && mapped_path[0]) {
                        mi.mapped_bytes += page_size;
                    } else {
                        mi.private_bytes += page_size;
                    }
                }
                p += pages * page_size;
            }
        }

        const uintptr_t next = region_base + region_size;
        if (next <= addr) break;
        addr = next;
    }
}

//----------------------------------------------------------------------------------------
// 符号解析
//----------------------------------------------------------------------------------------
uintptr_t ps_symbols_open(const char *path, uintptr_t base) {
    if (!g_inited) ps_platform_init();
    if (!path || !path[0]) return 0;
    for (size_t i = 0; i < g_sym_modules.size(); ++i) {
        if (g_sym_modules[i]->base == base) return (uintptr_t)g_sym_modules[i];
    }
    SymModule *m = load_pe_symbols(path, base);
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
    if (rva) *rva = m->entries[index].rva;
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
        if (m->entries[mid].rva <= rva) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0) return 0;
    if (offset) *offset = rva - m->entries[best].rva;
    return m->names[m->entries[best].name_index].c_str();
}

//----------------------------------------------------------------------------------------
// 杂项
//----------------------------------------------------------------------------------------
void ps_set_debug(int on) { InterlockedExchange((volatile LONG *)&g_ps_debug, on ? 1 : 0); }

// 见 platform.h 的说明：给本模块 +1 引用计数，让引擎后续的 FreeLibrary 扣不掉我们，
// 从而避免"卸载后引擎残留指针被访问 -> 崩溃"。
// 用 GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 从"本函数地址"反查模块句柄，
// 这样不需要知道 DLL 路径，也不会因为路径写法不同而失败。
bool ps_keep_module_mapped() {
    HMODULE self = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)(const void *)&ps_keep_module_mapped, &self)) {
        return self != 0;  // 引用计数已经 +1，且我们不释放
    }
    return false;
}

// Windows 不用信号采样，恒为 0（保持接口一致）
long ps_handler_run_count(void) { return 0; }

// Windows 用 SuspendThread + GetThreadContext 取指令指针，不依赖信号，
// 所以不需要"解开信号屏蔽"这一步。
void ps_prepare_sampling_thread(void) {}

void ps_request_stop_sampling(void) { stop_flag_set(1); }

bool ps_stop_requested(void) { return stop_flag_get() != 0; }

double ps_now_seconds() {
    static LARGE_INTEGER freq;
    static bool init = false;
    if (!init) {
        init = true;
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (freq.QuadPart == 0) return (double)GetTickCount64() / 1000.0;
    return (double)now.QuadPart / (double)freq.QuadPart;
}

void ps_get_local_time(int *y, int *mo, int *d, int *h, int *mi, int *s) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (y) *y = st.wYear;
    if (mo) *mo = st.wMonth;
    if (d) *d = st.wDay;
    if (h) *h = st.wHour;
    if (mi) *mi = st.wMinute;
    if (s) *s = st.wSecond;
}

const char *ps_platform_name() { return "win32"; }

}  // namespace ps
