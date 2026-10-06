# perfstat —— 服务器进程内 CPU / 内存分析插件（纯引擎插件）

一个**直接由游戏引擎加载**的服务器插件，用法和 `l4dtoolz` 一样：

```
plugin_load perfstat
```

- **不依赖 metamod**、**不依赖 sourcemod**
- **不需要 hl2sdk 就能编译**（自带一份与官方 SDK 逐字对齐的最小 ABI 头文件）
- 运行期只调用操作系统 API 与 libc，**不链接 tier0 / tier1 / vstdlib**

功能：按**动态链接库（DLL / SO）**统计服务器进程内部的 CPU 占用与内存占用，从大到小排序，
并可以展开看每个模块内部的**热点函数 / 偏移**，以及**线程维度**明细。

---

## 目录

- [1. 它到底能测出什么（先看这段）](#1-它到底能测出什么先看这段)
- [2. 编译](#2-编译)
- [3. 安装与加载](#3-安装与加载)
- [4. 控制台指令](#4-控制台指令)
- [5. 怎么读报告](#5-怎么读报告)
- [6. 配置 perfstat.ini](#6-配置-perfstatini)
- [7. 原理](#7-原理)
- [8. 开销与风险（重要）](#8-开销与风险重要)
- [9. 常见问题](#9-常见问题)
- [10. 自检工具（不用开服就能验证）](#10-自检工具不用开服就能验证)
- [11. GitHub Actions 自动编译](#11-github-actions-自动编译)
- [12. 文件结构](#12-文件结构)
- [13. 可以继续加的功能](#13-可以继续加的功能)

---

## 1. 它到底能测出什么（先看这段）

| 需求 | 能不能做到 | 怎么做到的 |
| --- | --- | --- |
| 每个 DLL 的 CPU 占比 | ✅ 能，但属于**统计抽样** | 定时抓取各线程“此刻正在执行哪条指令”，按地址落在哪个模块归类 |
| 每个 DLL 的内存占用 | ✅ 能，比较准 | Windows 遍历虚拟地址空间 + 查工作集；Linux 解析 `/proc/self/maps` 与 `smaps` |
| 模块内部哪个函数最热 | ✅ 能，但受“有没有符号表”限制 | 热点偏移 → PE 导出表 / ELF 符号表二分查找最近的符号 |
| 哪个线程在烧 CPU | ✅ 能 | 采样时记录线程 ID |
| 函数之间的调用关系（谁调用了谁） | ❌ 做不到 | 需要抓完整调用栈，代价和风险都高一个量级，没有做 |

**两条必须理解的前提：**

1. **"CPU 占比"是抽样比例，不是精确计时。**
   采样看到的是"此刻正在执行谁"。跑 1 分钟以上结论才可靠；跑 5 秒只能看个大概。
2. **统计的是"加载这个插件的那个进程"。**
   一台机器上开多个 srcds 实例时，需要在每个实例里各加载一次。

---

## 2. 编译

### Windows（32 位）

需要 VS2022（含 C++ 生成工具）和 Windows SDK。

```bat
:: 直接双击，或在 cmd 里执行
build_win32.bat
```

产物：`build\perfstat.dll`

脚本会自动找 `vcvars32.bat`（已适配 VS 装在非 C 盘的常见情况）。
如果你的 VS 路径特殊，就自己开一个 **x86 Native Tools Command Prompt**，然后：

```bat
cl /nologo /LD /MT /O2 /Ob2 /Oi /GS- /W3 /EHsc /utf-8 /GR- /DNDEBUG /DWIN32 /D_WINDOWS ^
   /Fo:build\ /Fd:build\perfstat.pdb ^
   src\perfstat.cpp src\core.cpp src\perf_platform_win32.cpp ^
   /link /OUT:build\perfstat.dll /MACHINE:X86 /SUBSYSTEM:WINDOWS ^
   /INCREMENTAL:NO /OPT:REF /OPT:ICF kernel32.lib user32.lib psapi.lib
```

> **必须是 32 位**（`/MACHINE:X86`）。L4D2 的 srcds 是 32 位进程，64 位 DLL 加载不进去。
> 源码里有 `static_assert(sizeof(void*) == 4)` 会在编译期拦住这种错误。

### Linux（32 位）

```bash
# 64 位机器上编 32 位需要 multilib
sudo apt install g++-multilib
make            # 产物：Release/perfstat_srv.so
```

> ℹ️ **Linux 由 GitHub Actions 验证，不是我在本机编的。**
> 交付环境只有 Windows + MSVC，我没有 Linux 工具链，所以 Linux 的编译与自检放在
> [工作流](#11-github-actions-自动编译)里跑：它会 `g++ -m32` 编出 `.so`、用 `file`/`nm`
> 校验产物、再跑一遍 POSIX 加载自检（dlopen + 假 ICvar + 真实采样）。
> 如果 Actions 报错，把失败那一步的日志发我即可。

### 上传到 GitHub 之前要做什么

**直接推整个 `perfstat` 文件夹就行，不需要手动删东西**：`.gitignore` 已经把
`build/`、`Release/`、`dist/`、`*.log`、`*.obj`、`*.dll`、`*.so` 之类全部忽略掉了。

仓库里应该只有这些（20 个文件）：

```
.github/workflows/build.yml      GitHub Actions 工作流
.gitignore
build_win32.bat                  Windows 构建脚本
Makefile                         Linux 构建脚本
perfstat.ini                     配置示例
README.md
src/*.h  src/*.cpp               源码（8 个）+ perfstat.def（说明用，当前不需要）
tools/*.bat  tools/Makefile.linux_tests
tools/perfstat_tests.cpp         算法/ABI 自检
tools/perfstat_loader_test.cpp   Windows 加载自检
tools/perfstat_loader_test_linux.cpp   Linux 加载自检
tools/perfstat_smoke_linux.cpp   Linux 平台层冒烟测试
```

> 如果你已经推过一次、发现仓库里有 `build/` 之类的残留，用这两条清掉：
> ```bash
> git rm -r --cached build Release dist
> git commit -m "stop tracking build output"
> ```

### 想改用真实 hl2sdk 编译？

加 `/DPERFSTAT_USE_SDK`（或 `-DPERFSTAT_USE_SDK`），并把 `hl2sdk-l4d2/public` 等目录加进
include 路径。此时：
- 头文件改用 SDK 的 `iserverplugin.h` / `interface.h` / `convar.h` / `icvar.h`
- `InterfaceReg` / `CreateInterface` 交给 SDK 的 `interface.cpp`（需要链接 `tier1.lib`）

默认（不加这个宏）走自带头文件，完全自包含。

---

## 3. 安装与加载

把编译产物放到服务器的 `left4dead2/addons/` 目录：

```
left4dead2/
└── addons/
    └── perfstat.dll        (Windows)
    └── perfstat_srv.so     (Linux)
```

然后在服务器控制台：

```
plugin_print                :: 顺便确认插件列表里有 perfstat
plugin_load perfstat        :: 也可以写 plugin_load addons/perfstat
perf_help
```

加载成功后控制台会打印一段说明。如果 `plugin_load` 报 `Unable to load plugin`，
先确认位数是 32 位（见上一节）。

---

## 4. 控制台指令

| 指令 | 作用 |
| --- | --- |
| `perf_help` | 显示全部指令与读表说明 |
| `perf_start [间隔ms] [时长秒]` | 开始采样。例：`perf_start 10 60` = 10ms 采一次、跑 60 秒后自动停 |
| `perf_stop` | 停止采样（数据保留，还能继续看报告） |
| `perf_stat [行数]` | 在控制台打印**模块排名表**（CPU + 内存） |
| `perf_top [行数]` | 排名表 + 每个模块内部的**热点函数 / 偏移** |
| `perf_threads` | 报告 + **线程维度**明细（看是哪个线程在烧 CPU） |
| `perf_dump [路径]` | 把完整报告写入 `logs/perfstat-时间戳.log`，并打印路径 |
| `perf_reset` | 清空统计并立刻重新开始采样 |
| `perf_load` | 重新读取 `perfstat.ini` |
| `perf_selftest` | 自检：报告当前用的控制台输出通道、采样状态、模块/线程数、落盘是否可用 |

**建议流程：**

```
perf_start 10 60        :: 让服务器正常跑着（有人、有图），采 60 秒
:: ... 60 秒后自动停止 ...
perf_top 10             :: 先看哪个 DLL 最热、它内部哪个函数最热
perf_dump               :: 存档一份，方便对比
```

> 插件默认**加载后自动开始采样**（`auto_start = 1`），所以你也可以不执行 `perf_start`，
> 过一会儿直接 `perf_stat`。想彻底零开销就把 `auto_start` 改成 0。

---

## 5. 怎么读报告

```
================================================================================
 perfstat  服务器性能分析报告
--------------------------------------------------------------------------------
 时间         : 2026-10-06 20:19:06
 平台         : win32   (采样方式: 逐线程挂起取指令指针)
 采样间隔     : 10 ms     采样次数: 905     采样漏掉/无效: 0
 统计时长     : 3.2 秒   实际采样频率: 285.9 次/秒
 模块数量     : 5        线程数量: 3
--------------------------------------------------------------------------------
 #  CPU%   MODULE                          MEM(MB)  mem排序   内存构成
--------------------------------------------------------------------------------
 1 66.41  ntdll.dll                            1.64  #1    map 1.0 / priv 0.0 / other 0.7
 2 33.48  perfstat_tests.exe                   2.42  #5    map 0.2 / priv 0.0 / other 2.2
 3  0.11  KERNEL32.DLL                         0.60  #4    map 0.2 / priv 0.0 / other 0.3
--------------------------------------------------------------------------------
 内存合计     : 文件映射 1.8 MB + 私有提交 0.0 MB + 其它 5.1 MB = 6.9 MB
```

### CPU% 怎么算

```
CPU% = 该模块命中采样数 / 总采样数
```

一次采样会覆盖进程内的多个线程，所以**总采样数 ≥ 采样次数**。
粗略地说：**某个模块 100% ≈ 它独占了一个 CPU 核心。**

在主循环里跑代码的模块（`engine.dll` / `server.dll`）通常是第一梯队；
如果某个第三方插件的 DLL 排到了前排，那就是它在吃 CPU。

### 三个内存列

| 列 | 含义 |
| --- | --- |
| `map` | 该模块**文件映射**部分驻留在内存里的量（就是 DLL/SO 本体） |
| `priv` | 落在该模块地址区间内的**私有提交**（堆 / 栈 / 运行时分配） |
| `other` | 已提交但当前不在工作集里的部分（Windows 上是 `QueryWorkingSetEx` 判定；Linux 上是 `smaps` 的段大小减 RSS） |

`mem排序` 那一列是按"内存总量"重新排的名次，方便你一眼看出**内存大户是谁**（它和 CPU 排名往往差很多）。

> **关于 `priv` 的重要提醒**：引擎的全局内存分配器（`tier0` 那套）在**主程序**里，
> 它分配出来的内存地址落在主程序的区间内，所以会算到主程序（`srcds.exe` / `srcds_linux`）头上，
> 而不是算给"真正申请内存的那个插件"。这是地址区间归属法的固有限制，不是 bug。
> 想看具体某个插件自己申请了多少内存，得用堆分析（本插件没做）。

### 热点明细（`perf_top`）

```
[G:\...\server.dll]  基址=0x004D0000  大小=2.4 MB  CPU=33.48%  采样=303
     20.79% of module   ServerGameDLL003+0x157D (rva 0x157D)
     14.85% of module   server.dll+0x1581
```

- `xx% of module` 是**占该模块自身**的比例，不是占全进程。
- 有符号就显示 `符号名+偏移`，没有就退化成 `模块名+偏移`。
- 符号来源：Windows 读 PE 导出表，Linux 读 ELF 的 `.symtab` / `.dynsym`。
  **导出表只有导出函数**，所以出现 `server.dll+0x1581` 这种说明命中的是内部函数。
  Windows 上如果想还原成函数名，可以用 `dumpbin /symbols`、IDA 或 PDB；
  有 `.map` 文件的话按 RVA 查也可以。

### 线程明细（`perf_threads`）

```
 #  CPU%   TID      THREADNAME
 1 33.59  3332     Thread-3332
 2 33.48  24976    NetThread
```

线程名来自 `GetThreadDescription`（Windows）/ `/proc/self/task/<tid>/comm`（Linux）。
引擎自己的线程通常有名字（网络线程、物理线程等），能看到"是网络线程在烧"这种结论。

---

## 6. 配置 `perfstat.ini`

把 [`perfstat.ini`](perfstat.ini) 放到下面任一位置即可：

1. 服务器工作目录下的 `perfstat.ini`
2. srcds 可执行文件同目录
3. `<srcds目录>/cfg/perfstat.ini`

改完执行 `perf_load` 立刻生效。所有键都可省略。

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `sample_ms` | 10 | 采样间隔（毫秒），越小越精细、开销越大 |
| `duration_sec` | 0 | 默认采样时长，0 = 不自动停 |
| `console_top` | 25 | `perf_stat` 打印多少行 |
| `hotspot_top` | 8 | 每个模块展开几个热点 |
| `hotspot_min_hits` | 2 | 热点显示门槛（过滤噪声） |
| `show_hotspot` | 0 | `perf_stat` 是否也展开热点 |
| `show_threads` | 0 | 是否附带线程明细 |
| `auto_start` | 1 | 加载后自动开始采样 |
| `auto_dump_sec` | 0 | 每隔多少秒自动落盘一份报告 |
| `auto_dump_path` | 空 | 固定落盘路径（配合 `duration_sec`） |

---

## 7. 原理

### 7.1 CPU：逐线程挂起取指令指针

```
采样线程（插件创建，优先级略低）
   │
   ├─ 每 sample_ms 一轮：
   │    枚举本进程所有线程
   │    for 每个线程（滑动窗口，一轮最多 24 个）:
   │        SuspendThread(t)                  ← 挂起
   │        GetThreadContext(t) → EIP/RIP     ← 取“此刻执行到哪”
   │        ResumeThread(t)                   ← 立刻恢复（必须！）
   │    把每个 IP 归属到模块 → 计数 + 热点直方图
   │
   └─ 睡到下一个周期
```

- **Windows**：`Toolhelp32` 枚举线程 + `OpenThread` + `SuspendThread` + `GetThreadContext`。
  在 WOW64（32 位进程跑在 64 位系统）上用 `Wow64GetThreadContext` 取 32 位的 `Eip`。
  打开线程的权限必须包含 `THREAD_GET_CONTEXT`，少一个就是静默失败（开发时踩过这个坑）。
- **Linux**：读 `/proc/self/task` 拿线程 ID，用 `tgkill` 向每个线程投递一个实时信号，
  信号处理器里从 `ucontext` 取 `EIP`/`RIP` 写进槽位。处理器内不加锁、不分配内存。
  逐线程"投递→等待回填"，避免多线程同时进处理器时串槽位。

> **滑动窗口**：线程很多的服务器（几百个线程）如果一轮全挂起来，单次停顿会变长。
> 所以一轮最多实际挂起 `24` 个线程，其余线程在后续轮次轮转覆盖——统计上仍然是均匀的。

### 7.2 地址归因

按模块基址升序建表，采样到的 IP 用**二分查找**找"最后一个基址 ≤ IP 的模块"，
再检查 `IP < base + size`。落在模块之外的采样（JIT 代码、已卸载模块）不参与百分比。

### 7.3 符号解析

不用 dbghelp、不用 libbfd，直接读文件：

- **PE（Windows）**：`MapViewOfFile` 映射 DLL，走 `IMAGE_DIRECTORY_ENTRY_EXPORT`，
  收集 `AddressOfFunctions` / `AddressOfNames` / `AddressOfNameOrdinals`，
  每个导出函数记下 `RVA + 名字`，按 RVA 排序后二分查最近符号。转发导出跳过。
- **ELF（Linux）**：读整个文件，找 `.symtab` / `.dynsym` 与对应字符串表，
  收 `STT_FUNC` / `STT_OBJECT` 且 `st_shndx != SHN_UNDEF` 的符号，
  `st_value` 减去 ELF 头里 PT_LOAD 的最低 `p_vaddr` 得到模块内偏移。

### 7.4 内存归因

- **Windows**：`VirtualQueryEx` 遍历整个虚拟地址空间 → `GetMappedFileName` 拿到
  该区域映射的是哪个文件（`\Device\HarddiskVolumeX\...` 会用 `QueryDosDevice` 建的前缀表
  转回盘符路径）→ 逐页用 `QueryWorkingSetEx` 判断是否驻留。
- **Linux**：解析 `/proc/self/maps` 得到段的范围、权限、文件路径，
  再从 `/proc/self/smaps` 取每段的 `Rss`。带文件的段算 `map`，匿名段算 `priv`。

> ⚠️ **所有文件 API 都用宽字符版本**。中文路径（例如 `G:\工作区\...`）用
> `CreateFileA` 会直接返回 `ERROR_PATH_NOT_FOUND`。这是开发时真实踩过的坑。

### 7.5 为什么不抓调用栈

抓完整调用栈需要栈展开（unwind），32 位 + 优化过的二进制 + 在别人线程的栈上走，
很容易读坏内存直接崩服。所以这里只取"当前指令指针"这一层：
**"此刻在执行谁"** —— 对"哪个 DLL 更吃 CPU"这个问题已经足够，而且安全得多。

---

## 8. 开销与风险（重要）

### 性能开销

| 项目 | 量级 |
| --- | --- |
| 不采样时 | ≈ 0（采样线程只每 50ms 醒一次判断状态） |
| 采样中（默认 10ms 间隔） | 约 1% ~ 3% CPU（每次挂起 + 取上下文约几十微秒，摊到进程上很小） |
| 生成报告时（内存遍历） | **Windows 上会停顿几百毫秒**（32 位进程大约 300ms ~ 1s） |

> **生成报告会短暂卡服**：因为要逐页查工作集，而且是在持有锁的情况下做的。
> 建议在换图间隙或人少的时候执行 `perf_stat` / `perf_dump`。
> 只想快速看 CPU 排名的话，可以把 `perfstat.ini` 里的内存相关需求交给 `perf_dump`（后台落盘）来做。

### 已知风险

1. **挂起线程**：采样时会把别的线程挂起很短的时间。这是所有进程内采样器的通行做法，
   但客观上存在极小概率的副作用（例如某个线程正好持有 loader lock）。
   如果服务器对稳定性要求极高，建议只在排查问题时开启，长期跑的话把 `sample_ms` 调大（如 50）。
2. **采样线程自身不在统计内**：插件不挂起自己，所以报告里看不到采样线程的开销。
3. **只统计加载之后的行为**：`plugin_load` 之前的模块加载、初始化开销看不到。
4. **模块热更**：采样期间新加载的 DLL 会在下一轮报告里出现；已卸载模块的历史采样会变成"无归属"。

### 卸载（`plugin_unload`）

`Unload()` 会按顺序做三件事：

1. 停掉采样线程并 **`WaitForSingleObject` / `pthread_join` 等它真正退出**
   —— 否则线程会跑在已卸载的代码上，直接崩服；
2. 把所有 `perf_*` 指令从 `ICvar` **反注册**并释放；
3. 关闭符号表、释放缓存。

所以 `plugin_load` / `plugin_unload` 可以反复执行。

---

## 9. 常见问题

**Q：`plugin_load perfstat` 提示 `Unable to load plugin`？**
确认是 32 位 DLL（`dumpbin /headers perfstat.dll` 看 `machine (x86)`），
并且文件确实在 `addons/` 目录下。

**Q：加载成功但 `perf_stat` 报 `Unknown command`？**
说明指令没注册上（拿不到 `VEngineCvar007`）。看加载时的输出有没有
`警告: 拿不到 VEngineCvar007 接口`。这种情况下仍可用 `perfstat.ini` 配置采样，
但需要用别的方式触发报告（例如 `auto_dump_sec` 自动落盘）。

**Q：指令能联想、不报 Unknown command，但控制台什么文字都没有？**
这是 v1.0 的一个已知问题，已经在 v1.0.1 修掉：早期版本用 C 运行时的 `printf` 输出，
而本地 listen server 里进程的 `stdout` 并不接到游戏控制台（只有服务器控制台窗口/日志能看到）。
现在改成按优先级走引擎自己的输出通道：

1. `ICvar::ConsolePrintf`（首选，服务器控制台注册的显示函数会把它带给玩家）
2. `tier0!ConMsg`（次选，走引擎 spew 输出）
3. `fputs(stdout)`（兜底，纯命令行 srcds 或重定向到文件时有用）

执行 `perf_selftest` 会明确告诉你当前用的是哪一条通道。如果那里显示的是
`stdout (兜底)`，说明前两条都不可用，把结果发我。

**Q：Linux 上执行 `perf_*` 会不会卡住？**
v1.0.2 修掉了一个真实的死等 bug。Linux 的采样是"给目标线程发信号，等信号处理器把指令指针
写回槽位"；槽位下标原本放在**全局变量**里，于是当进程里存在**两个采样者**时（插件自己的
采样线程 + 离线自检程序直接调 `ps_sample_threads`，或者将来别的插件也做类似的事），
两边会互相改写这个下标，导致某一边永远等不到回填而死等。
现在槽位改成 `thread_local`，每个采样线程各用一份，互不干扰。Windows 用
`SuspendThread` 挂起线程取上下文，不存在这个问题——这也解释了为什么当初只有 Linux 卡。

**Q：`perf_stat` 输出的 CPU% 全是 0？**
还没采到数据。先 `perf_start 10 60`，或者确认 `perfstat.ini` 里 `auto_start = 1`。
报告末尾也会提示"还没有采到数据，请先执行 perf_start"。

**Q：为什么第一名是 `ntdll.dll` 而不是 `engine.dll`？**
空转/等待的服务器里，线程大多停在系统调用里（`ntdll` 的等待代码），
这是正常的。等服务器真有负载（有人、有图）时再采，才看得出真正的热点。

**Q：各列加起来不到 100%？**
落在任何模块之外的采样（JIT 代码、已卸载模块、无效地址）不参与百分比计算。
正常情况这个缺口很小（1% 以内）。

**Q：内存那一列全是 0？**
v1.0.3 修掉了一个真实的顺序 bug：`report()` 里原本是"**先取快照、再收集内存**"，
导致内存数据永远晚一拍才写进报告，那一列永远是 0。现在改成先收集内存再取快照。
（顺便也修了 `QueryWorkingSetEx` 的动态加载——新 Windows 把它挪到了
`kernel32.dll` 并以 `K32QueryWorkingSetEx` 导出，现在两个位置都试。）

**Q：`priv` 全是 0？**
说明工作集信息拿不到（Windows 的 `QueryWorkingSetEx` 不可用，或 Linux 的
`/proc/self/smaps` 读不到）。这时候 Windows 会退化成"不区分驻留"，
页数按 `map` / `priv` 归类，量级仍然是对的。
另外提醒：`priv` 是**按地址区间归属**的估算，引擎的全局分配器都在主程序名下，
所以小 DLL 的 `priv` 通常就是 0，这是正常的。

**Q：能统计"插件调用引擎"的开销吗？**
不能。采样只回答"此刻在谁那里执行"，不回答"是谁调用的"。需要调用关系就得抓调用栈（见 7.5）。

---

## 10. 自检工具（不用开服就能验证）

`tools/` 下有两个自检程序，用来验证"插件能不能被引擎加载"和"采样归因算得对不对"。
它们**不需要 srcds**，直接在本机跑。

### 10.1 算法自检 `perfstat_tests.exe`

```bat
tools\build_tests.bat
build\perfstat_tests.exe
```

验证内容：

- **ABI 布局**：`CCommand` / `ConCommandBase` / `ConCommand` 的尺寸与字段偏移；
  通过 vtable 逐个真调用 `IServerPluginCallbacks` 的 20 个回调，确认槽位顺序与 SDK 完全一致。
  （x86 的 `__thiscall` 是被调用者清栈，自检里为每个槽位写了参数个数正确的原型。）
- **模块枚举 / PE 导出表 / 地址归因**：用真实的 `kernel32.dll` 做样本，
  确认能把 `CreateFileA` 的绝对地址反解回名字。
- **端到端采样**：真起两个线程（一个空转烧 CPU、一个睡觉），采 3 秒，
  确认样本落在正确的模块和函数上，并打印一份真实报告。

### 10.2 加载自检 `perfstat_loader_test.exe`

```bat
tools\build_loader_test.bat
build\perfstat_loader_test.exe
```

这一步模拟引擎的 `plugin_load` 全流程：

1. `LoadLibrary` + `GetProcAddress("CreateInterface")`
2. 请求 `ISERVERPLUGINCALLBACKS003`
3. 造一个**假的 `ICvar`（VEngineCvar007）**传给 `Load()`，
   确认 9 个 `perf_*` 指令**确实注册上来了**（这条极其关键：
   不注册的话引擎会直接报 `Unknown command`，插件的 `ClientCommand` 永远收不到）
4. 通过假 ICvar 记录的 `ConCommandBase*`，用 vtable 调 `Dispatch()`
   —— 这就是引擎敲命令时走的路径 —— 把 `perf_help` / `perf_start 5 2` /
   `perf_top` / `perf_threads` / `perf_dump` / `perf_stop` 全跑一遍
5. `Unload()` 后确认 9 个指令都被反注册了（不会留下野指针）
6. `FreeLibrary` 卸载成功（说明采样线程收干净了，没有线程跑在已卸载代码上）

当前状态：**36 项 + 16 项全部通过**。

> 自检程序失败时返回退出码 1、成功返回 0，CI 里用**退出码**判断成败。
> 不要用 `findstr` 去匹配日志里的中文（"失败 0 项"）——`findstr` 对 UTF-8 中文的匹配
> 不可靠，测试全过也会被判失败（这个坑踩过一次）。

### 10.3 Linux 平台层冒烟测试 `perfstat_smoke_linux`

```bash
make -f tools/Makefile.linux_tests smoke
./build/perfstat_smoke_linux 60        # 参数是超时秒数
```

这个程序**不加载插件**，只调平台层的三个入口（`ps_platform_init` /
`ps_enum_modules` / `ps_sample_threads`），用来把"信号采样本身有没有问题"和
"插件加载流程有没有问题"分开定位。它带一个 `SIGALRM` 看门狗：超时就把当前卡在哪个阶段
写到 stderr 再退出，不会让 CI 一直挂着。

### 10.4 Linux 自检 `perfstat_loader_test_linux`

```bash
make -f tools/Makefile.linux_tests
./build/perfstat_loader_test_linux Release/perfstat_srv.so
```

因为 Linux 上没有引擎可以托管插件，这个自检用 `dlopen` 直接把 `.so` 当普通共享库加载：

1. `dlsym("perfstat_factory")` / `dlsym("CreateInterface")` —— 取插件入口
2. 请求 `ISERVERPLUGINCALLBACKS003`，校验类型
3. 假 `ICvar` 传给 `Load()`，确认 10 个 `perf_*` 指令注册成功，
   并通过 `ConsolePrintf` 验证输出通道
4. 调 `Dispatch()` 跑几条指令
5. **真起一个忙线程，直接驱动平台采样层**（`ps_sample_threads`）采 2 秒，
   验证信号采样通路和模块归因真的能工作
6. `Unload()` 反注册全部指令，`dlclose()` 成功（说明采样线程收干净了）

### 10.5 还没验证的部分

- **在真实 srcds 里加载**：本机没有 L4D2 服务端，无法实测。
  能离线验证的部分（导出符号、ABI、加载流程、指令注册与派发、采样算法、
  Linux 的编译与运行）都由自检和 CI 覆盖了；真机上剩下的不确定点主要是
  "引擎传进来的 `interfaceFactory` 能否取到 `VEngineCvar007`" 这类加载时机差异。
- **Windows 上 listen server 的控制台表现**：输出通道已经改成 `ICvar::ConsolePrintf`，
  但最终效果要你在真机上确认一次（见下面的 FAQ）。

反馈方式：`plugin_load perfstat` 之后执行一次 `perf_selftest`，
把控制台内容发回来，我按实际情况调。

---

## 11. GitHub Actions 自动编译

仓库里已经带好了工作流：[`.github/workflows/build.yml`](.github/workflows/build.yml)。
把整个 `perfstat` 文件夹推到 GitHub，然后在 **Actions** 页面就能看到：

1. 打开仓库的 **Actions** 标签页
2. 左侧选 **build**
3. 右上角 **Run workflow** → 选分支 → 运行

也可以在推送到 `main` / `master` 或提 PR 时自动触发。

跑完后在对应那次运行的页面底部 **Artifacts** 处下载：

| 产物 | 内容 |
| --- | --- |
| `perfstat-windows.zip` | `addons/perfstat.dll`、`perfstat.ini`、`README.md`、两个自检 exe 与日志 |
| `perfstat-linux.zip` | `addons/perfstat_srv.so`、`perfstat.ini`、`README.md`、Linux 自检程序与日志 |
| `perfstat-all-platforms.zip` | 上面两个合并成一个总包（一次下载搞定） |

工作流做了这些事：

- **Windows 任务**：准备 32 位 MSVC 环境 → `build_win32.bat` → 用 `dumpbin` 校验产物是
  x86 且导出了 `CreateInterface` → 编译并运行两个自检程序 → 检查日志里有"失败 0 项"
  → 打包上传
- **Linux 任务**：安装 `g++-multilib` → `make` → 用 `file` / `nm` 校验产物是 32 位 ELF
  且导出了 `CreateInterface` / `perfstat_factory` → 编译并运行 POSIX 加载自检
  → 检查日志里"失败 0 项" → 打包上传
- **汇总任务**：等两个平台都成功后，把它们合并成一个总包

**任何一步失败整个工作流就会标红**，所以 Actions 变绿其实就等于"两个平台都编出来了、
而且自检全过"。这也是替代我无法在本机验证 Linux 的办法。

> 想改产物文件名/目录结构，直接改 `build.yml` 里对应步骤的 `cp` / `Compress-Archive` 行即可。

---

## 12. 文件结构

```
perfstat/
├── build_win32.bat                  Windows 32 位构建脚本
├── Makefile                         Linux 32 位构建脚本
├── perfstat.ini                     可选配置（放服务器目录）
├── .gitignore
├── .github/workflows/build.yml      GitHub Actions：双平台自动编译 + 自检 + 打包
├── src/
│   ├── plugin_api.h                 自带的引擎 ABI 头文件（逐字对齐官方 SDK）
│   ├── perfstat.h                   插件类声明 + 编译期 ABI 断言
│   ├── perfstat.cpp                 入口：22 个回调、指令注册与派发、日志落盘
│   ├── core.h / core.cpp            与平台无关：模块表、采样归因、热点统计、报告生成
│   ├── platform.h                   平台抽象层接口
│   ├── perf_platform_win32.cpp      Windows：Toolhelp/挂起采样、PE 导出表、VirtualQuery
│   ├── perf_platform_linux.cpp      Linux：信号采样、ELF 符号、/proc/self/maps
│   ├── compat.h                     跨编译器的小工具（原子操作、睡眠）
│   └── perfstat.def                 说明用（当前不需要，见文件内注释）
└── tools/
    ├── perfstat_tests.cpp              算法/ABI 自检（跨平台）
    ├── perfstat_loader_test.cpp        模拟引擎加载的自检（Windows）
    ├── perfstat_loader_test_linux.cpp  模拟引擎加载的自检（Linux / dlopen）
    ├── perfstat_smoke_linux.cpp        Linux 平台层冒烟测试（不加载插件，带看门狗）
    ├── Makefile.linux_tests            Linux 两个自检的构建
    ├── build_tests.bat
    └── build_loader_test.bat
```

数据流：

```
perfstat.cpp (插件入口 / 指令)
      │
      ├── core.cpp (Profiler)  ── 模块表、采样归因、热点、报告
      │        │
      │        └── platform.h ──┬── perf_platform_win32.cpp
      │                         └── perf_platform_linux.cpp
      │
      └── plugin_api.h (ABI) + perfstat.h (对外接口)
```

---

## 13. 可以继续加的功能

按"性价比"排序，想做哪个告诉我：

1. **`perfstat.ini` 之外的真 ConVar**（`perf_sample_ms` 等），可以用 `cvar` 指令直接改
2. **CSV 输出**，方便在 Excel 里画趋势图
3. **定时对比**：把两次采样的差值算出来，专门看"这段时间里谁涨得最快"
4. **调用栈采样**（`-fno-omit-frame-pointer` 的模块上做帧指针回溯），能给出"谁调用了谁"
5. **按函数聚合而不是按偏移**：把同一函数内的多个偏移合并成一行，减少噪声
6. **堆分析**：接引擎的内存分配钩子，才能真正回答"这个插件自己吃了多少内存"
7. **外部监控工具**：脱离插件、从外部看机器上所有 srcds 实例（需要独立进程 + 权限，是另一个方案）

---

## 附：与 l4dtoolz 的关系

本项目的加载方式、平台分层思路参考了 [`l4dtoolz`](../l4dtoolz)（同为 L4D2 引擎插件）。
区别在于：

| | l4dtoolz | perfstat |
| --- | --- | --- |
| 目的 | 改引擎行为（人数、tickrate） | 只读分析，不改任何引擎状态 |
| 依赖 | 需要 hl2sdk 头文件编译 | 自带 ABI 头文件，零外部依赖 |
| 偏移寻址 | 大量硬编码引擎内部偏移（版本敏感） | 不使用任何引擎内部偏移，只用公开接口 |
| 额外功能 | ConVar 注册（依赖 tier1） | 自己实现 ICvar 注册路径，不链接 tier1 |

因此 perfstat 对游戏版本更新**不敏感**（只要 `ISERVERPLUGINCALLBACKS003` 与
`VEngineCvar007` 不变），代价是它拿不到引擎内部数据，只能从外部观察。
