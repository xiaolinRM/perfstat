//========================================================================================
// perfstat - 插件类声明
//========================================================================================

#ifndef PERFSTAT_PLUGIN_H
#define PERFSTAT_PLUGIN_H

#include <string>

#include "core.h"

// 如果你以后想用 hl2sdk-l4d2 自带的头文件编译，可以在编译命令里加
//   /DPERFSTAT_USE_SDK
// 并把 hl2sdk-l4d2/public 下的相关目录加入 include 路径即可（见 README）。
#if defined(PERFSTAT_USE_SDK)
#include "eiface.h"
#include "engine/iserverplugin.h"
#include "tier1/interface.h"
#include "tier1/tier1.h"
#else
#include "plugin_api.h"
#endif

//----------------------------------------------------------------------------------------
// 接口暴露宏
//   * 默认（自带头文件）：InterfaceReg / CreateInterface 在 perfstat.cpp 里自己实现
//   * /DPERFSTAT_USE_SDK：直接用 SDK 的 EXPOSE_SINGLE_INTERFACE_GLOBALVAR
//----------------------------------------------------------------------------------------
#ifndef PERFSTAT_EXPOSE_PLUGIN
#if defined(PERFSTAT_USE_SDK)
#define PERFSTAT_EXPOSE_PLUGIN(className, globalVarName)                                         \
    EXPOSE_SINGLE_INTERFACE_GLOBALVAR(className, IServerPluginCallbacks,                          \
                                      INTERFACEVERSION_ISERVERPLUGINCALLBACKS, globalVarName)
#else
#define PERFSTAT_EXPOSE_PLUGIN(className, globalVarName)                                         \
    namespace ps {                                                                               \
    static void *__CreatePerfStatPlugin_##className() {                                          \
        return static_cast<IServerPluginCallbacks *>(&globalVarName);                             \
    }                                                                                            \
    }                                                                                            \
    static InterfaceReg __g_CreatePerfStatPlugin_##className##_reg(                               \
        ps::__CreatePerfStatPlugin_##className, INTERFACEVERSION_ISERVERPLUGINCALLBACKS);
#endif
#endif

//----------------------------------------------------------------------------------------
// ABI 自检：这些必须在编译期就对上，否则说明头文件与真实 SDK 不一致
//----------------------------------------------------------------------------------------
// CCommand 在 32 位下是：2 个 int + 2×512 字符缓冲 + 64 个指针 = 4+4+512+512+256 = 1288
static_assert(sizeof(CCommand) == 1288,
              "CCommand 大小与 l4d2 SDK 不一致：引擎传进来的参数对象会被读错位");
static_assert(sizeof(void *) == 4,
              "必须编译成 32 位（/MACHINE:X86 或 -m32），L4D2 服务器是 32 位进程");

#define PERFSTAT_VERSION "1.2.1"

namespace ps {

extern Profiler *g_profiler;

class PerfStatPlugin : public IServerPluginCallbacks {
public:
    PerfStatPlugin();

    // ---- IServerPluginCallbacks ----
    virtual bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory);
    virtual void Unload(void);
    virtual void Pause(void);
    virtual void UnPause(void);
    virtual const char *GetPluginDescription(void);
    virtual void LevelInit(char const *pMapName) { (void)pMapName; }
    virtual void ServerActivate(edict_t *pEdictList, int edictCount, int clientMax) {
        (void)pEdictList;
        (void)edictCount;
        (void)clientMax;
    }
    virtual void GameFrame(bool simulating);
    virtual void LevelShutdown(void) {}
    virtual void ClientActive(edict_t *pEntity) { (void)pEntity; }
    virtual void ClientDisconnect(edict_t *pEntity) { (void)pEntity; }
    virtual void ClientPutInServer(edict_t *pEntity, char const *playername) {
        (void)pEntity;
        (void)playername;
    }
    virtual void SetCommandClient(int index) { (void)index; }
    virtual void ClientSettingsChanged(edict_t *pEdict) { (void)pEdict; }
    virtual PLUGIN_RESULT ClientConnect(bool *bAllowConnect, edict_t *pEntity, const char *pszName,
                                        const char *pszAddress, char *reject, int maxrejectlen) {
        (void)bAllowConnect;
        (void)pEntity;
        (void)pszName;
        (void)pszAddress;
        (void)reject;
        (void)maxrejectlen;
        return PLUGIN_CONTINUE;
    }
    virtual PLUGIN_RESULT ClientCommand(edict_t *pEntity, const CCommand &args);
    virtual PLUGIN_RESULT NetworkIDValidated(const char *pszUserName, const char *pszNetworkID) {
        (void)pszUserName;
        (void)pszNetworkID;
        return PLUGIN_CONTINUE;
    }
    virtual void OnQueryCvarValueFinished(QueryCvarCookie_t iCookie, edict_t *pPlayerEntity,
                                          EQueryCvarValueStatus eStatus, const char *pCvarName,
                                          const char *pCvarValue) {
        (void)iCookie;
        (void)pPlayerEntity;
        (void)eStatus;
        (void)pCvarName;
        (void)pCvarValue;
    }
    virtual void OnEdictAllocated(edict_t *edict) { (void)edict; }
    virtual void OnEdictFreed(const edict_t *edict) { (void)edict; }

private:
    void cmd_help();
    void cmd_selftest();  // 诊断：确认控制台输出通道 / 采样 / 落盘是否正常
    void cmd_start(int argc, const char **argv);
    void cmd_stop();
    void cmd_stat(int top, bool hot, bool threads, bool no_memory = false);
    void cmd_dump(int argc, const char **argv);
    void cmd_reset();
    void cmd_load();
    bool write_report_file(const std::string &explicit_path, std::string &out_path);

    double m_last_auto_dump;
    bool m_dumped_once;
};

// 全局唯一实例（perfstat.cpp 里定义，接口暴露宏与指令回调都用它）
extern PerfStatPlugin g_perfstat;

}  // namespace ps

#endif  // PERFSTAT_PLUGIN_H
