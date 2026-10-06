//========================================================================================
// perfstat - 跨编译器的小工具（MSVC / gcc 都能用）
//   这些宏不能放进 platform.h，因为 platform.h 被纯 C 风格的平台实现包含。
//========================================================================================

#ifndef PERFSTAT_COMPAT_H
#define PERFSTAT_COMPAT_H

#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>
#include <string.h>
#include <windows.h>

// MSVC 没有 strcasecmp / strncasecmp（只有 _stricmp / _strnicmp）
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

#define PS_ATOMIC_INC(p) _InterlockedIncrement(reinterpret_cast<volatile long *>(p))
#define PS_ATOMIC_DEC(p) _InterlockedDecrement(reinterpret_cast<volatile long *>(p))
#define PS_ATOMIC_SET(p, v) _InterlockedExchange(reinterpret_cast<volatile long *>(p), (long)(v))
#define PS_ATOMIC_GET(p) (long)_InterlockedCompareExchange(reinterpret_cast<volatile long *>(p), 0, 0)
static inline void ps_sleep_ms(int ms) {
    if (ms < 0) ms = 0;
    Sleep((DWORD)ms);
}
#else
#include <time.h>
#define PS_ATOMIC_INC(p) __sync_add_and_fetch((p), 1)
#define PS_ATOMIC_DEC(p) __sync_sub_and_fetch((p), 1)
#define PS_ATOMIC_SET(p, v) __sync_lock_test_and_set((p), (v))
#define PS_ATOMIC_GET(p) __sync_add_and_fetch((p), 0)
static inline void ps_sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}
#endif

#endif  // PERFSTAT_COMPAT_H
