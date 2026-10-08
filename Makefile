#========================================================================================
# perfstat - Linux 构建（srcds_run / srcds_linux 是 32 位进程）
#
#   本插件不依赖 metamod / sourcemod，也【不需要 hl2sdk】，所以没有任何外部库依赖：
#   它自带最小化的引擎 ABI 头文件，只调用 libc / libpthread。
#
#   用法：
#       make             # 生成 Release/perfstat.so
#       make clean
#
#   交叉编译示例（在 64 位机器上编 32 位 .so）：
#       sudo apt install gcc-multilib
#       make
#
#   注意：perfstat.def 只对 Windows 有意义，这里是 .so，靠 -fvisibility 控制导出。
#========================================================================================

PROJECT   = perfstat
OBJECTS   = perfstat.cpp core.cpp perf_platform_linux.cpp

CXX       = g++
CXXFLAGS  = -m32 -std=c++11 -O2 -fPIC -fno-exceptions -fno-rtti -fno-strict-aliasing \
            -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wextra \
            -Wno-unused-parameter -DNDEBUG -DLINUX -D_stricmp=strcasecmp -D_strnicmp=strncasecmp
# 说明：
#   -static-libgcc -static-libstdc++  【必须】
#     服务端自带的 bin/libstdc++.so.6 往往比编译机上的旧。实测在 Ubuntu 22.04
#     (gcc 11) 上编译出的 .so 拿到 L4D2 服务端会加载失败：
#       failed to dlopen perfstat.so error=bin/libstdc++.so.6:
#         version `GLIBCXX_3.4.29' not found
#     把 libstdc++ / libgcc 静态链进去，插件就不依赖服务端的 C++ 运行库版本。
#
#   ⚠️ 但这【解决不了 glibc】。glibc 基本没法静态链接，而且它是"向前兼容"的：
#      在老系统上编 -> 能在新系统上跑；在新系统上编 -> 老系统跑不了。
#      所以【必须在足够老的 glibc 环境里编译】。CI 用 ubuntu:20.04 容器
#      (glibc 2.31) 就是为此；若直接拿 Ubuntu 24.04 编，产物会要求 GLIBC_2.36，
#      Ubuntu 22.04 (glibc 2.35) 的用户会直接加载失败。
#      自检命令：objdump -T Release/perfstat.so | grep -o 'GLIBC_[0-9.]*' | sort -V -u
LDFLAGS   = -m32 -shared -rdynamic -static-libgcc -static-libstdc++
LIBS      = -lpthread -ldl -lm

SRCDIR    = src
BIN_DIR   = Release
TARGET    = $(BIN_DIR)/$(PROJECT).so

# 【为什么产物必须叫 perfstat.so】
# Source 的自动加载是靠 .vdf 文件，里面 "file" 写的是【不带扩展名的基名】：
#     "Plugin" { "file" "addons/perfstat" }
# 引擎自己按平台拼成 .dll（Windows）/ .so（Linux）。所以 Linux 产物必须正好叫
# perfstat.so，写别的名字（例如服务端自带的 _srv 那种）自动加载会找不到 —— 
# 那个 _srv 命名是引擎自己的内部文件习惯，不是插件约定。

all: $(TARGET)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/%.o: $(SRCDIR)/%.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -c $< -o $@

$(TARGET): $(addprefix $(BIN_DIR)/,$(OBJECTS:.cpp=.o))
	$(CXX) $(LDFLAGS) $^ $(LIBS) -s -o $@
	@echo ""
	@echo "构建完成: $(TARGET)"
	@echo "安装: 把 perfstat.so + perfstat.vdf + perfstat.ini 放进 left4dead2/addons/"
	@echo "      自动加载（下次开服生效）；或手动执行 plugin_load perfstat"
	@echo ""

clean:
	rm -rf $(BIN_DIR)

.PHONY: all clean
