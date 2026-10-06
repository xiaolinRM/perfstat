import io

# ---- platform.h: 加一个记录"锁被卡住"的调试钩子 ----
p = r"G:\工作区\004\perfstat\src\compat.h"
s = io.open(p, encoding="utf-8").read()
if u"ps_dbg_lock_site" not in s:
    old = u"""#endif  // PERFSTAT_COMPAT_H"""
    new = u"""//----------------------------------------------------------------------------------------
// 锁诊断：记录"最后一次成功加锁的位置"和"当前等待加锁的位置"。
// 只在 verbose 模式下使用，用来回答"到底是谁把锁握住了"。
// 定义在 core.cpp，因为自检不编 core.cpp 的话不会有引用。
//----------------------------------------------------------------------------------------
extern "C" void ps_dbg_lock_site(const char *who);
extern "C" void ps_dbg_lock_wait(const char *who);

#endif  // PERFSTAT_COMPAT_H"""
    assert old in s
    s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)
    print("compat.h ok")

# ---- core.cpp: 实现 + 在 SpinLock 前后打点 ----
p = r"G:\工作区\004\perfstat\src\core.cpp"
s = io.open(p, encoding="utf-8").read()
old = u"""// 平台层用 extern "C" int g_perfstat_verbose_flag; 引用它。"""
if old not in s:
    old = u"""namespace {"""
    new = u"""//----------------------------------------------------------------------------------------
// 锁诊断（只在 verbose 下生效）
//----------------------------------------------------------------------------------------
static const char *g_lock_holder = "";
static const char *g_lock_waiter = "";

extern "C" void ps_dbg_lock_site(const char *who) { g_lock_holder = who; }
extern "C" void ps_dbg_lock_wait(const char *who) { g_lock_waiter = who; }

const char *ps_dbg_lock_holder(void) { return g_lock_holder; }
const char *ps_dbg_lock_waiter(void) { return g_lock_waiter; }

namespace {"""
    assert old in s
    s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)
    print("core.cpp helpers ok")
