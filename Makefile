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
LDFLAGS   = -m32 -shared -static-libgcc
LIBS      = -lpthread -ldl -lm

SRCDIR    = src
BIN_DIR   = Release
TARGET    = $(BIN_DIR)/$(PROJECT)_srv.so

# 目标名写成 perfstat_srv.so 只是沿用 Source 服务器的命名习惯，
# plugin_load 时用哪个名字就用哪个文件，改成 perfstat.so 也可以。

all: $(TARGET)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/%.o: $(SRCDIR)/%.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -c $< -o $@

$(TARGET): $(addprefix $(BIN_DIR)/,$(OBJECTS:.cpp=.o))
	$(CXX) $(LDFLAGS) $^ $(LIBS) -s -o $@
	@echo ""
	@echo "构建完成: $(TARGET)"
	@echo "安装: 把该文件放到服务器的 left4dead2/addons/ 目录下，然后执行 plugin_load perfstat"
	@echo ""

# 方便：make so 也能得到 Release/perfstat.so
so: $(TARGET)
	cp $(TARGET) $(BIN_DIR)/$(PROJECT).so

clean:
	rm -rf $(BIN_DIR)

.PHONY: all so clean
