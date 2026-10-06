import io

# ============================================================================
# loader test：结尾打印"可直接贴给我"的短摘要
# ============================================================================
p = r"G:\工作区\004\perfstat\tools\perfstat_loader_test_linux.cpp"
s = io.open(p, encoding="utf-8").read()

old = u"""    fprintf(stderr, "\\n================================================================================\\n");
    fprintf(stderr, "结果: 通过 %d 项, 失败 %d 项\\n", g_pass, g_fail);"""
if old not in s:
    # 找实际的结果打印
    import re
    m = re.search(u"结果: 通过 %d 项, 失败 %d 项", s)
    print("anchor A found:", bool(m))
    raise SystemExit(1)

new = u"""    fprintf(stderr, "\\n================================================================================\\n");
    fprintf(stderr, "结果: 通过 %d 项, 失败 %d 项\\n", g_pass, g_fail);

    //----------------------------------------------------------------------------------------
    // 【给故障排查用】极短摘要 —— 出问题时只需要把这一段贴出来就够了，
    // 不用贴整份日志（几百行）。
    //----------------------------------------------------------------------------------------
    fprintf(stderr, "\\n===== PERFSTAT 诊断摘要（出问题时贴这一段即可）=====\\n");
    fprintf(stderr, "结果: 通过=%d 失败=%d\\n", g_pass, g_fail);
    if (g_first_fail[0]) fprintf(stderr, "首个失败: %s\\n", g_first_fail);
    fprintf(stderr, "平台层来源: 插件 dlsym（单副本）\\n");
    fprintf(stderr, "插件平台层采样: 样本=%d 落在本模块=%d 最慢=%.1fms 慢轮=%d\\n", g_diag_total,
            g_diag_in_self, g_diag_worst_ms, g_diag_slow);
    fprintf(stderr, "插件平台层信号处理器运行次数=%ld\\n", g_diag_handler_runs);
    fprintf(stderr, "插件自身采样次数(观察前/后)=%llu/%llu  running=%d auto_stop=%d\\n",
            g_diag_plugin_before, g_diag_plugin_after, g_diag_plugin_running, g_diag_plugin_autostop);
    fprintf(stderr, "锁持有者='%s'\\n", g_diag_lock_holder);
    fprintf(stderr, "===== 摘要结束 =====\\n");"""
s = s.replace(old, new, 1)

# 加全局诊断变量 + 首个失败记录
old2 = u"""static int g_fail = 0;
static int g_pass = 0;"""
if old2 not in s:
    old2 = u"""static int g_pass = 0;
static int g_fail = 0;"""
new2 = old2 + u"""

// 诊断摘要用的全局量（出问题时只贴摘要，节省来回）
static char g_first_fail[512] = {0};
static int g_diag_total = 0, g_diag_in_self = 0, g_diag_slow = 0;
static double g_diag_worst_ms = 0.0;
static long g_diag_handler_runs = 0;
static unsigned long long g_diag_plugin_before = 0, g_diag_plugin_after = 0;
static int g_diag_plugin_running = -1, g_diag_plugin_autostop = -1;
static char g_diag_lock_holder[128] = {0};"""
assert old2 in s, "pass/fail decl not found"
s = s.replace(old2, new2, 1)

io.open(p, "w", encoding="utf-8", newline="\n").write(s)
print("summary added")
