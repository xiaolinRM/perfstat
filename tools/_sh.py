import io

p = r"G:\工作区\004\perfstat\src\core.cpp"
s = io.open(p, encoding="utf-8").read()

old = u"""void Profiler::collect_memory_locked() {
    static ModuleInfo buf[1024];
    if (m_modules.empty()) return;
    if ((int)m_modules.size() > 1024) return;

    // 先清空旧的统计
    for (size_t i = 0; i < m_modules.size(); ++i) {
        buf[i] = ModuleInfo();
        buf[i].base = m_modules[i].base;
        buf[i].size = m_modules[i].size;
        strncpy(buf[i].path, m_modules[i].path.c_str(), sizeof(buf[i].path) - 1);
        strncpy(buf[i].name, m_modules[i].name.c_str(), sizeof(buf[i].name) - 1);
        buf[i].is_main = m_modules[i].is_main;
    }

    // 注意：lookup 回调里不能再次加锁（本函数已经持锁），所以直接内联一个二分查找
    struct Local {
        static int lookup(uintptr_t addr, void *user) {
            Profiler *p = (Profiler *)user;
            // 这里不能调用 find_module_by_addr 的加锁版本，直接内联二分
            int lo = 0, hi = (int)p->m_modules.size() - 1, best = -1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (p->m_modules[mid].base <= addr) {
                    best = mid;
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
            if (best < 0) return -1;
            if (addr < p->m_modules[best].base + p->m_modules[best].size) return best;
            return -1;
        }
    };

    ps_walk_memory(&Local::lookup, this, buf, (int)m_modules.size());

    for (size_t i = 0; i < m_modules.size(); ++i) {
        m_modules[i].mem_mapped = buf[i].mapped_bytes;
        m_modules[i].mem_private = buf[i].private_bytes;
        m_modules[i].mem_other = buf[i].other_bytes;
    }
}"""
new = u"""// 内存归因
//
// 【关键：必须在不持锁的情况下做】ps_walk_memory 在 Linux 上要读 /proc/self/smaps
// 并遍历全部映射段，大进程上可能耗时数百毫秒到数秒。如果持着 m_lock 做这件事，
// 采样线程就会在 apply_sample() 里一直等锁 —— 表现是"采样线程在跑、但样本数不涨"，
// 而且不报任何错（踩过：自检里 total_samples 一直是 0，日志显示 apply_sample 从未被调用）。
//
// 所以流程是：持锁取一份模块表快照 -> 放锁 -> 遍历内存 -> 再持锁合并结果。
void Profiler::collect_memory_locked() {
    // 注意：调用本函数时【已经持有】m_lock（名字里的 locked 就是这个意思）。
    // 这里先取快照，然后立刻放锁再做慢操作。
    static ModuleInfo buf[1024];
    static ModuleInfo snapshot[1024];
    int count = 0;

    {
        if (m_modules.empty()) return;
        if ((int)m_modules.size() > 1024) return;
        count = (int)m_modules.size();
        for (int i = 0; i < count; ++i) {
            buf[i] = ModuleInfo();
            buf[i].base = m_modules[i].base;
            buf[i].size = m_modules[i].size;
            buf[i].is_main = m_modules[i].is_main;
            // 同步一份不含 std::string 的快照给遍历回调用（回调里不能碰锁）
            snapshot[i] = buf[i];
        }
    }

    // ---- 慢操作：不持锁 ----
    struct Local {
        static int lookup(uintptr_t addr, void *user) {
            ModuleInfo *mods = (ModuleInfo *)user;
            // 按基址升序（模块表就是这么维护的），直接线性找最后一个 base <= addr 的
            // 模块数量是几十~几百，线性扫比二分更适合这里（不用传 count，也不怕写法出错）
            int best = -1;
            for (int i = 0; i < 1024; ++i) {
                if (mods[i].size == 0 && mods[i].base == 0) break;
                if (mods[i].base <= addr) {
                    best = i;
                } else {
                    break;
                }
            }
            if (best < 0) return -1;
            if (addr < mods[best].base + mods[best].size) return best;
            return -1;
        }
    };

    // 上一次的结果要清掉，否则会累加
    for (int i = 0; i < count; ++i) {
        buf[i].mapped_bytes = 0;
        buf[i].private_bytes = 0;
        buf[i].other_bytes = 0;
    }

    // 释放锁（调用者持着，所以我们这里手动解开再重新加上）
    m_lock.unlock();
    ps_walk_memory(&Local::lookup, snapshot, buf, count);
    m_lock.lock();

    // ---- 合并结果 ----
    for (size_t i = 0; i < m_modules.size() && (int)i < count; ++i) {
        m_modules[i].mem_mapped = buf[i].mapped_bytes;
        m_modules[i].mem_private = buf[i].private_bytes;
        m_modules[i].mem_other = buf[i].other_bytes;
    }
}"""
assert old in s, "collect_memory_locked not found"
s = s.replace(old, new, 1)

io.open(p, "w", encoding="utf-8", newline="\n").write(s)
print("ok")
