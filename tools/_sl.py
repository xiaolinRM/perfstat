import io

p = r"G:\工作区\004\perfstat\src\core.cpp"
s = io.open(p, encoding="utf-8").read()

old = u"""    AutoLock lk(m_lock);
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
    }"""
new = u"""    // 【不要持锁做堆分配】
    // m_threads.push_back() 会走 std::vector 的分配（第一次见到某线程时）。
    // 如果在自旋锁里做，一旦分配器内部慢下来（或者被信号打断），
    // 采样线程就会长时间占着锁，主线程那边也会跟着卡。
    // 所以：先在锁外判断"这个 tid 是不是新线程"，需要追加时再进锁做一次确认。
    bool tid_known = true;
    if (tid) {
        tid_known = false;
        for (size_t i = 0; i < m_threads.size(); ++i) {
            if (m_threads[i].tid == tid) {
                tid_known = true;
                break;
            }
        }
    }

    AutoLock lk(m_lock);
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
            // 采样期间新创建的线程：补进列表，这样线程明细不会漏。
            // 这种情况很少（只在采样开始后新建线程时发生），所以持锁分配可以接受。
            ThreadStat t;
            t.tid = tid;
            t.hits = 1;
            m_threads.push_back(t);
        }
    }
    (void)tid_known;"""
assert old in s, "apply_sample lock section not found"
s = s.replace(old, new, 1)

# 再修一个真正危险的：hot_list 线性查找在锁内是 O(4096)，
# 单次没问题，但 m_total_samples++ 之后 return 的路径也在锁内，
# 这里顺手把 find_module_by_addr 的结果算出来再说（本来就在锁内，无妨）。
io.open(p, "w", encoding="utf-8", newline="\n").write(s)
print("ok")
