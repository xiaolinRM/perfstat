# perfstat —— 服务器进程内 CPU / 内存分析插件（纯引擎插件）

一个**直接由游戏引擎加载**的服务器插件，和 `l4dtoolz` 一样靠 `.vdf` 自动加载：

```
left4dead2/addons/
├── perfstat.vdf      # 自动加载配置
├── perfstat.ini      # 可选配置
└── perfstat.dll      # Windows 产物（Linux 则是 perfstat.so）
```

把这几个文件放进 `addons/`，**下次开服就会自动加载**，不需要输入任何指令。
详见 [3. 安装与加载](#3-安装与加载)。

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
make            # 产物：Release/perfstat.so
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

### 3.1 推荐：用 `.vdf` 自动加载

把这三个文件放进服务器的 `left4dead2/addons/` 目录：

```
left4dead2/
└── addons/
    ├── perfstat.vdf          # 自动加载配置（下面有内容说明）
    ├── perfstat.ini          # 可选配置
    ├── perfstat.dll          # Windows 产物
    └── perfstat.so           # Linux 产物（两个平台只需要放对应的那一个）
```

`perfstat.vdf` 的内容（和 l4dtoolz 一样的写法）：

```
"Plugin"
{
	"file"	"addons/perfstat"
}
```

注意 `"file"` 里写的是**不带扩展名的基名** —— 引擎自己按平台拼成 `.dll` / `.so`。
所以 **Linux 产物必须正好叫 `perfstat.so`**，写成别的名字（例如服务端自带的 `_srv` 那种）
自动加载会找不到。

> **`.vdf` 必须放在 `addons/` 里面**，不能放到 `addons/` 的上一级（即 `left4dead2/` 下）。
> 引擎启动时只扫 `addons/*.vdf`，位置不对就不会被加载。
> 但 `"file"` 里的路径**要带 `addons/` 前缀**（`addons/perfstat`）——
> 这个路径是相对 `left4dead2/` 的。两者一个是"文件放哪"、一个是"路径怎么写"，别搞混。

放好之后**下次开服就会自动加载**，不需要手动输入任何指令。
控制台可以用 `plugin_print` 确认列表里有 perfstat。

> 分隔符必须是**制表符**（tab），键和值都要加引号，`addons/` 用正斜杠。

### 3.2 手动加载（可选；日常不需要）

`.vdf` 已经能自动加载，所以下面这条**只在"不想重启服务器、想立刻加载"时**才用。
注意路径要写**相对 `left4dead2/` 的完整路径**（也就是和 `.vdf` 里 `"file"` 那一行完全一致）：

```
plugin_load addons/perfstat      :: 对；和 vdf 里的 "file" 写法一样
plugin_load perfstat             :: 错；这样会报 Unable to load plugin
perf_print                       :: 确认插件列表里有 perfstat（老版本引擎是 plugin_print）
perf_help
```

**.vdf 和指令的路径为什么是同一个**：`.vdf` 里写 `"file" "addons/perfstat"`，
而引擎解析这条指令时用的也是同一个基名规则 —— 所以插件放在 `addons/` 下时，
两者都写 `addons/perfstat`（**不带扩展名**，引擎自己按平台拼 `.dll` / `.so`）。

加载成功后控制台会打印一段说明。如果报 `Unable to load plugin`，
先确认位数是 32 位（见上一节），再确认路径里带了 `addons/`。

> **卸载注意**：`plugin_unload` 之后插件会**刻意保留控制台命令对象的内存**（不 free）。
> 这是有意为之，不是泄漏 bug —— 因为引擎侧的 ConCommandBase 链表可能仍持有这些对象，
> 一旦 free 掉，下次在输入框里**打任意一个字**（触发补全）就会崩溃。详见第 9 节 FAQ。

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

**Q：卸载后 `perf_*` 指令还在（有联想词、`help perf_stat` 有描述、不报 Unknown command），但输入没反应？**

这是 v1.7.0 修掉的真 bug，根因是**注册时漏了置 `m_bRegistered` 标志**：

Source 的注册流程里，`IConCommandBaseAccessor::RegisterConCommandBase` 在注册成功后
**必须把 `m_bRegistered` 置为 true**。我们原来只调了 `ICvar::RegisterConCommand`，
没置这个标志，于是 `IsRegistered()` 永远返回 false —— 而引擎的 `UnregisterConCommand`
看到"没注册过"就**静默跳过、什么都不做**。结果：

- 命令永远留在引擎命令表里 → 卸载后仍有联想词、`help xxx` 能看到描述、不报 Unknown command
- 但命令对象的代码/字符串在已卸载的模块里 → 输入后没有反应（现在有 `keep_mapped` 所以不崩）
- 再次 `plugin_load` 时每条指令都报
  `WARNING: unable to link xxx and xxx because one or more is a ConCommand.`

**修法**：注册成功后 `pVar->SetRegistered(true)`；反注册时**先**调用
`UnregisterConCommand`（此时标志必须仍为 true，引擎才会真的处理），**再** `SetRegistered(false)`。

自检里加了 3 条断言把回归挡住：假 ICvar 现在会模仿真实引擎"`IsRegistered()==false` 就跳过"
的行为，并统计被跳过次数（必须为 0）。

**Q：CPU 占比几乎全落在 `ntdll.dll` / `libc.so.6` 上（90%+），看不出别的模块谁高谁低？**

这是 v1.7.0 修的另一个真 bug，而且**是统计口径的问题，不是显示精度的问题**。

采样是"给线程投信号、取它当时的指令指针"。但**阻塞在 `futex` / `epoll` / `nanosleep`
里的空闲线程**，指令指针恰好停在 libc/ntdll 的系统调用包装里 —— 被采样到就把票算给了
系统库。服务器上大量工作线程平时都在阻塞等待，于是它们贡献了绝大多数样本。
实测（VM 上 1 个忙线程 + 6 个空闲线程）：

```
A) 采样【全部】线程:      2100 样本   ← 空闲线程贡献了大量无意义样本
                           而这些线程的真实 CPU 时间是 0 ms
B) 只采样【R 状态】线程:  300 样本    ← 每轮正好 1 个（真在跑的那个）
```

**修法**：默认只采样"正在占用 CPU"的线程（`perfstat.ini` 的 `cpu_state_filter = 1`）：

- Linux：`/proc/self/task/<tid>/stat` 第 3 字段 == `R`
- Windows：`NtQueryInformationThread` 的 `WaitReason == 0`（不在等待）

实测把归因率从 50% 提升到 98%（采到的样本几乎全是真在跑的线程）。

> 注意：这么做之后，**如果服务器大部分时间都在空转，样本数会明显变少** ——
> 这是对的，因为那段时间确实没有 CPU 占用可归因。想抓占用就去跑一段有负载的场景。

**Q：`plugin_unload` 之后不崩，但在输入框里打任意一个字（不用回车）就崩溃？**

这是 v1.4.2 修掉的真 bug，崩溃转储的特征很明确：

```
exception code : 0xC0000005 (ACCESS_VIOLATION)
AV type        : read at 0x00000000     ← 读的是空地址，不是某个已释放的堆地址
EIP            : ntdll.dll+0x739BC
```

**根因**：引擎的 `ConCommandBase` 是靠对象自带的 `s_pNext` 串成链表的，而客户端输入框
**每次按键**都会遍历这条链做补全/高亮。我们原来在 `UnregisterConCommand` 之后

```cpp
delete p;      // ← 问题在这里
```

把命令对象还给了堆。引擎下次按键时按旧指针去访问，对象里的 **vtable 已经被清零**，
于是"调用虚函数"变成"读地址 0 处的函数指针" → `read at 0x00000000` → 崩溃。

这解释了为什么是"打字才崩"：不按键就不会去遍历补全。

**修法（分两层，都要做）**：

**第一层（v1.5.0）：不释放命令对象。** 卸载时只反注册、不 `delete`，
名字字符串也保留。原来的代码还有第二个隐患：边遍历边 `delete`，而
`next = p->GetNext()` 是从**已经/即将释放**的对象里读出来的 —— 整段删掉一并消除。

**第二层（v1.6.0，根治）：让模块卸载后仍留在内存里。**
第一层做完后用户实测**仍然崩溃**，于是用崩溃转储重新定位，拿到了更精确的数据：

```
ThreadId         = 19316
ExceptionCode    = 0xC0000005  (ACCESS_VIOLATION)
ExceptionAddress = 0x513D9D03  ->  gameui.dll+0xE9D03
NumberParameters = 0
```

两次崩溃的 `ExceptionAddress` **完全相同**，且落在 `gameui.dll`（游戏 UI，输入框所在）。
这说明问题不在"我们释放了内存"，而在 **`plugin_unload` 把我们的 DLL 从进程里
unmapped 掉了**：此后引擎侧任何指向我们内存的指针（命令对象、名字字符串、
命令链表节点）一被访问就是"访问未映射内存"。

**引擎的插件接口没有"卸载时通知引擎清理引用"的机制**，所以插件侧唯一可靠的自保
办法就是**让模块别真的被卸掉**：

- Windows：`GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, 本函数地址, &self)`
  给本模块 +1 引用计数，引擎随后调用的 `FreeLibrary` 扣不掉我们。
- Linux：`dlopen(自己, RTLD_NOW|RTLD_NOLOAD)` 同样 +1 引用计数。

开关是 `perfstat.ini` 里的 `keep_mapped`（**默认 1**）。代价是卸载后代码/数据仍占
几百 KB 常驻内存；插件卸载后进程通常马上要结束（换图/关服），这点代价换来的是不再崩溃。
想恢复"真正卸载"就设 0（会有上述崩溃风险）。

> 自检里有一条断言专门验证这件事：卸载后 `dlopen(RTLD_NOLOAD)` 还能找到这个 SO，
> 并且从里面解析出导出函数并成功调用 —— 代码确实没被 unmapped。

**Q：`plugin_load perfstat` 提示 `Unable to load plugin`？**
最常见的原因是**路径没带 `addons/` 前缀**：插件在 `addons/` 下时必须写
`plugin_load addons/perfstat`（和 `.vdf` 里 `"file"` 的写法一致，不带扩展名）。

其次确认：

- 是 32 位产物（`dumpbin /headers perfstat.dll` 看 `machine (x86)`）
- Linux 上产物名必须是 **`perfstat.so`**（`.vdf` 写的是基名 `addons/perfstat`，
  引擎自己拼 `.so`；写成 `perfstat_srv.so` 之类会找不到）
- 日常其实**不需要**这条指令 —— 放好 `.vdf` 后开服会自动加载，用 `perf_print` 确认即可

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

**Q：Linux 上采不到样本 / 执行 `perf_*` 卡住 / 卸载时段错误？**
这几个问题都真实出现过（v1.0.2 ~ v1.0.6 陆续修掉），根因都在 Linux 的信号采样上：

1. **槽位下标是全局变量（v1.0.2 修）**
   采样是"给目标线程发信号，等信号处理器把指令指针写回槽位"。下标放全局变量时，
   进程里只要有**两个采样者**（插件自己的采样线程 + 离线自检直接调 `ps_sample_threads`），
   两边就会互相改写这个下标，某一边永远等不到回填而死等。
   现在改成 `thread_local`，每个采样线程各用一份。
   （Windows 用 `SuspendThread` 取上下文，没有这个共享状态，所以只有 Linux 会卡。）

2. **`ps_platform_init()` 把采样信号屏蔽了（v1.0.5 修）**
   本来是想"让采样线程自己不被这个信号打断"，但 POSIX 规定
   **新线程会继承创建者的信号掩码** —— 于是插件初始化之后创建的线程
   （引擎的网络线程、物理线程……）全都屏蔽掉了这个信号，
   `tgkill` 投递的信号永远进不了它们的处理器，**一个样本都采不到，而且不报错**。
   现在完全不屏蔽了：安全性靠"采样时跳过自己"+"处理器本身是异步信号安全的"来保证。
   如果你的宿主进程自己屏蔽了实时信号，那些线程就采不到（这是无法绕过的限制），
   采样时会 20ms 超时后跳过，不影响其它线程。

3. **采样线程自己继承了"已屏蔽"的信号掩码（v1.0.6 修）**
   创建采样线程的是主线程，而主线程在 `ps_platform_init()` 之后是屏蔽着采样信号的
   （测试程序就是这么做的），于是采样线程**自己也收不到采样信号**，
   它那一轮永远采不到东西 —— 表现就是插件报告里"采样次数: 0"，但没有任何报错。
   现在采样线程启动第一件事就是调用 `ps_prepare_sampling_thread()` 解开自己的屏蔽。

4. **卸载时把采样线程 detach 掉（v1.0.6 修）**
   上一版为了防止 `plugin_unload` 无限等，超时后直接 `pthread_detach` 走人。
   结果那个游离线程继续跑，**进程退出时它还在已卸载的代码里 → Segmentation fault**。
   现在改成"先请求停止（`ps_request_stop_sampling()`，让正在进行的采样在毫秒级放弃本轮），
   再耐心等待"，并且**绝不 detach**：宁可在日志里告警，也不制造野线程。

5. **采样槽位用了 `thread_local`（v1.0.8 修）**
   采样是"采样线程写 `state=1` → 信号处理器在**目标线程**上运行并回填 IP"，
   所以槽位是**跨线程**的通信单元。为了修坑 1（两个采样者互相踩），我把槽位表改成了
   `thread_local` —— 结果彻底坏了：处理器看的是目标线程那份（全空），
   采样线程轮询的是自己那份（永远等不到），**每一次采样都必然 20ms 超时、一个样本都拿不到**。
   现在改成：槽位表是**全进程共享的普通全局数组**，但按 tid 给每个采样线程分配一段
   **私有区间**（`kSlotsPerThread = 16`），既跨线程可见、又不会互相踩。

   > 这个 bug 有个很明显的特征：**单次采样耗时恒为 20ms**（超时时间）。
   > 所以自检里加了一条断言 —— "100 轮里没有任何一轮超过 15ms"，
   > 以后这类问题在本地就能被抓到，不用等 CI。

6. **进程里存在多份"平台层副本"，信号处理器被覆盖（v1.0.9 / v1.1.0 修）**
   这个坑最隐蔽。离线自检的 Makefile **只编平台层、不编 `core.cpp`**，所以
   `perfstat_loader_test_linux` 和 `perfstat.so` 里**各有一份** `perf_platform_linux.cpp`。
   两份各有自己的 `g_sig` / `g_slots` / **信号处理器**，而：
   - 信号处理器是**进程级**的，后装的会覆盖先装的；
   - `g_slots` 却是每份副本各自的。

   于是流程变成：测试程序先装自己的处理器 → 插件 `Load()` 里再装它自己的（覆盖）
   → 此后测试程序发信号，跑的是**插件的**处理器，填的是**插件的**槽位表，
   而测试程序轮询的是**自己的**表 → 永远等不到 → 每次采样卡满 20ms 超时、零样本。

   修法分两版（v1.0.9 的思路不够彻底，v1.1.0 才是正解）：

   - **v1.0.9**：处理器先找自己的窗口，找不到就用 CAS 认领全表任意 `state==1` 的槽位。
     这条退路**只能救"发起者与处理器内存可见"的情况**——如果发起采样的是 A 副本、
     处理器是 B 副本，B 的处理器**根本看不到 A 的槽位表**（那是另一块内存），
     所以 A 发起的采样依然全部失败。实测：自检程序能采到样本了，插件自己仍然 0。

   - **v1.1.0（正解）**：让"发起请求的线程"把**待回填槽位的地址**写进一个
     `thread_local` 指针（`g_pending_slot`），处理器直接照着这个地址写。
     处理器运行在**发起采样的那个线程**上，所以读到的就是这个线程写下的地址 ——
     **跨副本也一定正确**，因为传的是地址而不是"表里的某个下标"。

   另外顺手把设计简化了：既然每个采样线程同一时刻只有一个请求在飞，
   就不需要"窗口 + 游标"，直接按 tid 给每个采样线程**独占一格**即可
   （槽位表 512 格，够 512 个并发采样线程）。

7. **平台层引用了插件入口的全局变量（v1.0.7 修）**
   为了让采样线程打心跳日志，平台层引用了 `g_perfstat_verbose_flag`（定义在 `core.cpp`）。
   但 Linux 自检的 Makefile **只编平台层、不编 `core.cpp`**，于是直接
   `undefined reference to 'g_perfstat_verbose_flag'` —— 编译都过不去。
   现在平台层自持一个开关（`ps_set_debug()`），不再依赖 `core.cpp` 或插件入口的任何符号。

   > 这条约束已经写进 `tools/Makefile.linux_tests` 的注释里：
   > **`perf_platform_*.cpp` 只能依赖 `platform.h`**，改了平台层本地跑一下那个 Makefile
   > 就能提前发现这类耦合问题。

8. **把"跨线程通信单元"写成了 `thread_local`（踩了两次：v1.0.8、v1.1.3）**
   这个错误**犯了两次**，而且第二次是修第一个问题的时候顺手又犯的，值得单列：

   Linux 的采样是"采样线程写请求 → **信号处理器在目标线程上运行**并回填结果"。
   这个通信单元**必须是两个线程都能看到的内存**。写成 `thread_local` 之后：

   - 第一次（v1.0.8）：把**槽位表**写成 `thread_local` →
     处理器看目标线程那份（全空）、采样线程等自己那份（永远等不到）。
   - 第二次（v1.1.3）：修"多副本"问题时，把**待回填槽位的指针**写成 `thread_local` →
     采样线程写自己那份、处理器读目标线程那份（恒为 0）→ 什么都没回填。

   两次的表现完全一样：**每次采样卡满 20ms 超时、零样本、而且不报错**。

   > 判断标准很简单：**这个变量是否被"另一个线程"读？**
   > 是 → 绝对不能用 `thread_local`。
   > 在本文件里，只有 `g_slot_index`（"本采样线程独占哪一格"）该用 `thread_local`，
   > 它只被采样线程自己读写。

9. **持着锁做慢操作，把采样线程饿死了（v1.1.7 修）**
   `collect_memory_locked()` 原本是**持有 `m_lock`** 去调 `ps_walk_memory()` 的。
   而后者在 Linux 上要读 `/proc/self/smaps` 并遍历全部映射段，大进程上要几百毫秒到几秒。
   这段时间里采样线程卡在 `apply_sample()` 等锁 —— 表现是：

   - 采样线程**在跑**（心跳日志正常、`running=1`）
   - 但 `total_samples()` **一直是 0**（`apply_sample` 一次都没走完）
   - 而且**不报任何错**

   更麻烦的是它很难发现：报告里"采样次数"正常、内存列也正常，
   只有"采样是否真的在累积"这一个指标会暴露它。

   **修法**：`collect_memory_locked()` 改成"持锁取模块表快照 → **放锁** → 遍历内存 →
   再持锁合并结果"。慢操作期间锁是放开的，采样不受影响。

   > 通用教训：**不要在锁里做 I/O 或遍历整个地址空间这类慢操作**。
   > 只要有一个后台线程要靠这把锁推进，它就会被饿死，而且往往不报错。

10. **自检里两条断言本身写错了（v1.0.7 修）**
   - "样本应落在本程序模块内"：忙循环原来内联在测试的线程函数里，被编译器优化后
     可能落到别的模块（实测跑到了 `perfstat.so`），断言自然失败。
     现在把忙循环做成显式导出的函数，确保那段代码一定在测试程序自己的 `.text` 里。
   - 用 `ps::g_profiler` 检查"插件自己采到多少样本"：在测试程序里那会解析成
     **另一个同名变量**（初值 null），等于没测。现在插件明确导出
     `perfstat_get_profiler()`，测试从动态符号表里取。

顺带加了个**心跳日志**方便以后定位这类问题：离线自检会调用
`perfstat_set_verbose(1)`，之后 `Load()` / `Unload()` 每一步都往 stderr 打一行带毫秒
时间戳的记录：

```
[perfstat-hb      0.0 ms] Load: begin
[perfstat-hb      0.2 ms] Load: platform init done
[perfstat-hb      3.5 ms] Load: sampler thread started
[perfstat-hb   2191.2 ms] Unload: begin
[perfstat-hb   2216.3 ms] Unload: profiler deleted
```

一眼就能看出哪一步慢、卡在哪一步。采样线程自己也有低频心跳（每 100 轮一行）：

```
[perfstat-hb] sampler_loop enter: running=1 cap=64 threads=3 modules=9
[perfstat-hb] sampler round 0: n=3 total=0
[perfstat-hb] sampler round 100: n=3 total=300
```

这能把"采样线程根本没进循环"和"进了循环但一个样本都拿不到"直接区分开。

另外自检里加了一条**关键断言**：插件自己的采样线程必须真的采到样本
（通过导出的 `perfstat_get_profiler()` 读 `total_samples()`）。之前只断言"报告有输出"，
所以插件采样次数真的是 0 也没被发现 —— 这个坑不会再踩第二次。

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
./build/perfstat_loader_test_linux Release/perfstat.so
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

### 10.5 两个自检的分工（重要）

这两个程序验证的东西**刻意分开**，因为它们的"进程里平台层副本数量"不同：

| | `perfstat_smoke_linux` | `perfstat_loader_test_linux` |
| --- | --- | --- |
| 是否 dlopen 插件 | 否 | 是 |
| 进程里平台层副本数 | **1 份** | **2 份**（自检 + 插件） |
| 验证什么 | 信号采样通路 / 归因 / 延迟 / 符号解析 | 插件加载、指令注册与派发、输出通道、**插件自身采样**、卸载 |

为什么要这样分：自检和插件各编译了一份 `perf_platform_linux.cpp`，于是进程里有
两份 `g_slots` / 两份信号处理器；而**信号处理器是进程级的，后装覆盖先装**。
如果自检自己发起采样，实际跑的是插件副本的处理器、填的是插件副本的槽位表，
自检却在自己的表里等 —— 必然超时。

所以：**平台层的确定性验证放 smoke test（单副本）；loader test 只观察插件自己的
profiler 样本数有没有增长**。真实服务器里插件只有一份副本，不存在这个冲突。

### 10.6 自检自身的两个结构坑（都踩过）

写自检时踩的，记录一下免得重复：

1. **进程里只有主线程 → 采样永远 0 个样本**
   采样是靠"给**【别的】**线程发信号"取指令指针的，采样时总是跳过自己
   （`sample_tid_range` 里 `if (tid == self) continue;`）。
   所以如果进程里只有主线程一个线程，**采样必然一个目标都没有**，
   每次都返回 0、并卡满 20ms 超时 —— 看起来特别像"信号通路坏了"。
   修法：**在任何采样之前先起一个忙线程**，并且断言"枚举到的线程数 ≥ 2"。

2. **插件可能已经"自动停止"了，别以为它一直在采**
   `perfstat.ini` 里 `duration_sec` 有默认值（示例里是 2 秒），插件 `Load()` 时会按它
   启动"到点自动停"。而 CI 里从"加载插件"到"去观察采样"之间往往已经超过这个时长 ——
   插件早就停了，于是测试 `sleep(2)` 期间样本数**一个都不涨**，断言失败（踩过）。
   修法：观察之前先显式跑一次 `perf_start 10 0`（间隔 10ms、不自动停），
   并把 `running / auto_stop / elapsed` 一起打印出来，一眼就能看出是"停了"还是"采不到"。

3. **忙线程的代码要落在自己的模块里**
   忙循环如果内联在别处，编译器可能把那段代码放到别的模块，
   于是"样本应落在本程序模块内"的断言会失败（实测跑到了 `perfstat.so`）。
   修法：忙循环做成**显式导出 + noinline** 的函数。

这两条加上前面那条"loader test 不要自己采样"，本质是同一件事：
**先把测试环境搭对，再去测被测对象。**

### 10.7 还没验证的部分

- **在真实 srcds 里加载**：本机没有 L4D2 服务端，无法实测。
  能离线验证的部分（导出符号、ABI、加载流程、指令注册与派发、采样算法、
  Linux 的编译与运行）都由自检和 CI 覆盖了；真机上剩下的不确定点主要是
  "引擎传进来的 `interfaceFactory` 能否取到 `VEngineCvar007`" 这类加载时机差异。
- **Windows 上 listen server 的控制台表现**：输出通道已经改成 `ICvar::ConsolePrintf`，
  但最终效果要你在真机上确认一次（见下面的 FAQ）。

反馈方式：加载插件后执行一次 `perf_selftest`，
把控制台内容发回来，我按实际情况调。

---

### 10.8 本地 Linux 验证环境（重要：不要只靠 CI 猜）

**教训**：修 Linux 采样问题时，曾经连续多轮"改代码 → 推 CI → 等人把日志转过来"，
效率极低，而且很容易误判 —— 日志里 stdout 是块缓冲（进程退出才刷）、stderr 无缓冲，
两者交错后**行号完全不代表时间顺序**，就因为这个读错了好几次、白改好几轮。

**正确做法**：本地准备一个能跑 32 位 Linux 的环境，改完先自己跑通再推。
（本次就是靠这个：VMware + Ubuntu 22.04，SSH 过去编译运行，20 轮的盲改僵局一轮就破了。）
下面这套（VMware + Ubuntu 22.04）已验证可用：

```bash
# 1. 装 32 位编译支持（srcds 是 32 位的，必须 -m32）
sudo apt-get install -y g++ g++-multilib make

# 2. 编译
make                                  # 插件 -> Release/perfstat.so
make -f tools/Makefile.linux_tests    # 两个自检 -> build/

# 3. 跑（两个都必须 exit 0）
./build/perfstat_smoke_linux 60
./build/perfstat_loader_test_linux Release/perfstat.so 120
```

验收标准（自检结尾会打印一段"诊断摘要"，看那段就够，不用翻整份日志）：

```
结果: 通过=36 失败=0
插件平台层采样: 样本=200 落在本模块=100
插件自身采样: 372 -> 374  running=1 auto_stop=0
```

- `落在本模块` 必须 > 0 —— 说明采样能正确归因
- `插件自身采样` 后面的数必须**比前面大** —— 说明插件自己的采样线程真在采到东西
- 退出码必须是 0

> 忙线程会占满一个核，所以"单次采样最慢"几十毫秒是**正常的调度延迟**，不是缺陷。
> 自检里的断言已按这个现实调整（只要求有上界 + 每轮都采到样本）。

**稳定性验证**：两个自检都要**各跑 8 遍以上**确认没有偶发失败。
本项目在这上面栽过：单次通过不等于稳定 —— 曾经出现"连跑 4 次成功 1 次失败"的情况。

> 验证时的两个坑：
> 1. **不要在同一个 shell 里紧接着连跑两个自检**（loader 刚产生过重负载就起 smoke），
>    2 核机器上会因调度偶发失败。CI 里两者是独立 step，验证时也应在各自独立进程里跑。
> 2. `grep -c FAIL` 会把诊断摘要里的 `失败=0` 数进去，别用它判成败 —— 看退出码。

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
### 11.2 下载到的产物是什么结构（重要，别被"套了两层"绕晕）

GitHub Actions 的 **artifact 本身就是一个 zip**（这是它的机制，插件作者无法改变），
所以你会看到两层。这是正常的，照下面认就行：

```
下载的 artifact 包（Actions 生成的 zip）
└── perfstat-windows.zip            ← 解一次得到这个
    └── perfstat-windows/           ← 再解一次得到这个目录
        ├── addons/
        │   ├── perfstat.dll
        │   ├── perfstat.vdf
        │   └── perfstat.ini
        ├── tests/                  （自检程序与日志）
        └── README.md
```

一句话：**artifact 解开 = 一个平台 zip；那个 zip 解开 = 一层 `perfstat-<平台>/` 目录**。

`perfstat-all-platforms.zip` 同理，解开后是 `perfstat-windows/` 和 `perfstat-linux/` 两个目录。

> **两个平台 zip 的内部层级是刻意保持一致的**（都是"一层与 zip 同名的目录"），
> 这样"汇总打包"才能统一解包。以前 Windows 压成了没有顶层目录的结构
> （`addons/...` 直接在根），导致汇总步骤报"缺 windows 插件"（踩过）。
>
> 另外，如果你看到 **zip 里面又是同名 zip**（不是目录），那说明拿到的是旧版本 ——
> 旧版本的 artifact 路径写的是 `dist/` 目录，upload-artifact 会把 `dist/` 这一层也打进去。

| `perfstat-windows.zip` | `addons/perfstat.dll`、`addons/perfstat.vdf`、`addons/perfstat.ini`、`README.md`、两个自检 exe 与日志 |
| `perfstat-linux.zip` | `addons/perfstat.so`、`addons/perfstat.vdf`、`addons/perfstat.ini`、`README.md`、Linux 自检程序与日志 |
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
