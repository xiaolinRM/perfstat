//========================================================================================
// perfstat - 自带的“最小化”引擎接口定义
//
// 目的：本插件不依赖 metamod / sourcemod，也【不需要 hl2sdk-l4d2 就能编译】。
//       因此这里按 Valve 官方 hl2sdk（l4d2 分支）的定义逐字重写了加载插件与控制台
//       指令注册所必需的那几个结构。
//
// 与官方 SDK 的对应关系（字段顺序 / 虚函数顺序 / 调用约定均逐字对齐）：
//   CreateInterfaceFn / InterfaceReg   ->  public/tier1/interface.h
//   IServerPluginCallbacks             ->  public/engine/iserverplugin.h
//   CCommand                           ->  public/tier1/convar.h
//   ConCommandBase / ConCommand        ->  public/tier1/convar.h
//   ICvar / CVAR_INTERFACE_VERSION     ->  public/icvar.h
//
// !! 重要 !!
// 这几个结构会直接参与“引擎 <-> 插件”的 ABI 交互（引擎会拿着我们给的
// ConCommandBase* 自己按字段偏移读、按 vtable 槽位调），所以：
//   * 成员顺序不能改；
//   * 虚函数顺序不能改；
//   * 不要随意增删字段。
// 如果你有 hl2sdk-l4d2，可以用 tools/abi_check 的思路做一次 diff 校验。
//========================================================================================

#ifndef PERFSTAT_PLUGIN_API_H
#define PERFSTAT_PLUGIN_API_H

#include <stddef.h>

//----------------------------------------------------------------------------------------
// 基础类型 / 接口工厂
//----------------------------------------------------------------------------------------
typedef void *(*CreateInterfaceFn)(const char *pName, int *pReturnCode);
typedef void *(*InstantiateInterfaceFn)();

typedef int CVarDLLIdentifier_t;
typedef int QueryCvarCookie_t;

// 引擎侧的不完整类型：插件只拿指针，不需要真实定义
class edict_t;
class ConCommandBase;
class ConVar;
class Color;
class CCommand;

//----------------------------------------------------------------------------------------
// IServerPluginCallbacks —— 与 L4D2 的 ISERVERPLUGINCALLBACKS003 完全一致的 22 个虚函数
//----------------------------------------------------------------------------------------
typedef enum {
    PLUGIN_CONTINUE = 0,
    PLUGIN_OVERRIDE,
    PLUGIN_STOP,
} PLUGIN_RESULT;

typedef enum {
    eQueryCvarValueStatus_ValueIntact = 0,
    eQueryCvarValueStatus_CvarNotFound = 1,
    eQueryCvarValueStatus_NotACvar = 2,
    eQueryCvarValueStatus_CvarProtected = 3
} EQueryCvarValueStatus;

#define INTERFACEVERSION_ISERVERPLUGINCALLBACKS001 "ISERVERPLUGINCALLBACKS001"
#define INTERFACEVERSION_ISERVERPLUGINCALLBACKS002 "ISERVERPLUGINCALLBACKS002"
#define INTERFACEVERSION_ISERVERPLUGINCALLBACKS003 "ISERVERPLUGINCALLBACKS003"
#define INTERFACEVERSION_ISERVERPLUGINCALLBACKS INTERFACEVERSION_ISERVERPLUGINCALLBACKS003

#define CVAR_INTERFACE_VERSION "VEngineCvar007"

// 备用输出通道：host 进程里本来就加载了 tier0，直接取它的 ConMsg 用
// （ConMsg 会走引擎的 spew 输出系统，也就是服务器控制台）
typedef void (*ConMsgFn)(const char *pFormat, ...);

class IServerPluginCallbacks {
public:
    virtual bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory) = 0;
    virtual void Unload(void) = 0;
    virtual void Pause(void) = 0;
    virtual void UnPause(void) = 0;
    virtual const char *GetPluginDescription(void) = 0;
    virtual void LevelInit(char const *pMapName) = 0;
    virtual void ServerActivate(edict_t *pEdictList, int edictCount, int clientMax) = 0;
    virtual void GameFrame(bool simulating) = 0;
    virtual void LevelShutdown(void) = 0;
    virtual void ClientActive(edict_t *pEntity) = 0;
    virtual void ClientDisconnect(edict_t *pEntity) = 0;
    virtual void ClientPutInServer(edict_t *pEntity, char const *playername) = 0;
    virtual void SetCommandClient(int index) = 0;
    virtual void ClientSettingsChanged(edict_t *pEdict) = 0;
    virtual PLUGIN_RESULT ClientConnect(bool *bAllowConnect, edict_t *pEntity, const char *pszName,
                                        const char *pszAddress, char *reject, int maxrejectlen) = 0;
    virtual PLUGIN_RESULT ClientCommand(edict_t *pEntity, const CCommand &args) = 0;
    virtual PLUGIN_RESULT NetworkIDValidated(const char *pszUserName, const char *pszNetworkID) = 0;
    virtual void OnQueryCvarValueFinished(QueryCvarCookie_t iCookie, edict_t *pPlayerEntity,
                                          EQueryCvarValueStatus eStatus, const char *pCvarName,
                                          const char *pCvarValue) = 0;
    virtual void OnEdictAllocated(edict_t *edict) = 0;
    virtual void OnEdictFreed(const edict_t *edict) = 0;
};

//----------------------------------------------------------------------------------------
// CCommand —— 逐字对齐 l4d2 convar.h
//   注意：这个类在引擎里是【按值传递的栈对象】，插件只能读，不能构造（除了自检工具）。
//----------------------------------------------------------------------------------------
class CCommand {
public:
    CCommand() {}
    CCommand(int nArgC, const char **ppArgV) {
        (void)nArgC;
        (void)ppArgV;
    }

    enum {
        COMMAND_MAX_ARGC = 64,
        COMMAND_MAX_LENGTH = 512,
    };

    int ArgC() const { return m_nArgc; }

    const char **ArgV() const { return m_nArgc ? (const char **)m_ppArgv : 0; }

    const char *ArgS() const { return m_nArgv0Size ? &m_pArgSBuffer[m_nArgv0Size] : ""; }

    const char *GetCommandString() const { return m_nArgc ? m_pArgSBuffer : ""; }

    const char *Arg(int nIndex) const {
        if (nIndex < 0 || nIndex >= m_nArgc) return "";
        return m_ppArgv[nIndex];
    }

    const char *operator[](int nIndex) const { return Arg(nIndex); }

    static int MaxCommandLength() { return COMMAND_MAX_LENGTH - 1; }

private:
    int m_nArgc;
    int m_nArgv0Size;
    char m_pArgSBuffer[COMMAND_MAX_LENGTH];
    char m_pArgvBuffer[COMMAND_MAX_LENGTH];
    const char *m_ppArgv[COMMAND_MAX_ARGC];
};

//----------------------------------------------------------------------------------------
// FCVAR 标记（只列出插件用得到的；数值与 public/tier1/iconvar.h 一致）
//----------------------------------------------------------------------------------------
#define FCVAR_NONE 0
#define FCVAR_UNREGISTERED (1 << 0)
#define FCVAR_DEVELOPMENTONLY (1 << 1)
#define FCVAR_GAMEDLL (1 << 2)
#define FCVAR_CLIENTDLL (1 << 3)
#define FCVAR_HIDDEN (1 << 4)
#define FCVAR_PROTECTED (1 << 5)
#define FCVAR_SPONLY (1 << 6)
#define FCVAR_ARCHIVE (1 << 7)
#define FCVAR_NOTIFY (1 << 8)
#define FCVAR_USERINFO (1 << 9)
#define FCVAR_PRINTABLEONLY (1 << 10)
#define FCVAR_UNLOGGED (1 << 11)
#define FCVAR_NEVER_AS_STRING (1 << 12)
#define FCVAR_REPLICATED (1 << 13)
#define FCVAR_CHEAT (1 << 14)
#define FCVAR_SS (1 << 15)
#define FCVAR_DEMO (1 << 16)
#define FCVAR_DONTRECORD (1 << 17)
#define FCVAR_SS_ADDED (1 << 18)
#define FCVAR_RELEASE (1 << 19)
#define FCVAR_MENUBAR (1 << 20)
#define FCVAR_RELOAD_MATERIALS (1 << 22)
#define FCVAR_NOT_CONNECTED (1 << 26)
#define FCVAR_ARCHIVE_XBOX (1 << 27)
#define FCVAR_SERVER_CAN_EXECUTE (1 << 28)
#define FCVAR_SERVER_CANNOT_QUERY (1 << 29)
#define FCVAR_CLIENTCMD_CAN_EXECUTE (1 << 30)

//----------------------------------------------------------------------------------------
// 一个执行档要能用 ConVar，就要实现这个访问器（对应 convar.h 的 IConCommandBaseAccessor）
//----------------------------------------------------------------------------------------
class IConCommandBaseAccessor {
public:
    virtual bool RegisterConCommandBase(ConCommandBase *pVar) = 0;
};

//----------------------------------------------------------------------------------------
// 控制台指令回调类型（与 convar.h 一致）
//----------------------------------------------------------------------------------------
typedef void (*FnCommandCallbackV1_t)(void);
typedef void (*FnCommandCallback_t)(const CCommand &command);

#if defined(_WIN32)
#define PS_CVAR_INTERFACE __thiscall
#else
#define PS_CVAR_INTERFACE
#endif

//----------------------------------------------------------------------------------------
// ConCommandBase —— 逐字对齐 l4d2 convar.h
//
// 引擎会把这个对象挂进全局命令行链表，并调用它的虚函数取名字/帮助文本/标记。
// 字段偏移必须完全一致（32 位下：vptr0, m_pNext4, m_bRegistered8, m_pszName12,
// m_pszHelpString16, m_nFlags20, s_pConCommandBases24, s_pAccessor28，共 32 字节）。
//----------------------------------------------------------------------------------------
class ConCommandBase {
public:
    ConCommandBase(void)
        : m_pNext(0), m_bRegistered(false), m_pszName(0), m_pszHelpString(0), m_nFlags(0) {}

    ConCommandBase(const char *pName, const char *pHelpString = 0, int flags = 0)
        : m_pNext(0), m_bRegistered(false), m_pszName(pName), m_pszHelpString(pHelpString),
          m_nFlags(flags) {}

    virtual ~ConCommandBase(void) {}

    virtual bool IsCommand(void) const { return false; }

    virtual bool IsFlagSet(int flag) const { return (m_nFlags & flag) != 0; }
    virtual void AddFlags(int flags) { m_nFlags |= flags; }
    virtual void RemoveFlags(int flags) { m_nFlags &= ~flags; }

    virtual int GetFlags() const { return m_nFlags; }
    virtual const char *GetName(void) const { return m_pszName; }
    virtual const char *GetHelpText(void) const { return m_pszHelpString; }

    const ConCommandBase *GetNext(void) const { return m_pNext; }
    ConCommandBase *GetNext(void) { return m_pNext; }
    void SetNext(ConCommandBase *pBase) { m_pNext = pBase; }

    virtual bool IsRegistered(void) const { return m_bRegistered; }
    virtual CVarDLLIdentifier_t GetDLLIdentifier() const { return 0; }

    //----------------------------------------------------------------------------------------
    // 【必须设置】注册成功后要把 m_bRegistered 置 true，反注册前置回 false。
    //
    // 这是 Source 注册流程的契约：IConCommandBaseAccessor::RegisterConCommandBase 在
    // 真正注册成功后负责把它置位（引擎内部就是这么做的）。
    //
    // 漏掉它的后果（实测踩过）：引擎的 UnregisterConCommand 看到 IsRegistered()==false
    // 就认为"这条命令没被注册过"，于是【静默不做任何事】—— 命令永远留在引擎的命令表里：
    //   * 卸载后输入框仍有联想词、help xxx 仍能看到描述、也不报 Unknown command
    //   * 再次 plugin_load 时每条指令都报
    //     "WARNING: unable to link xxx and xxx because one or more is a ConCommand."
    //----------------------------------------------------------------------------------------
    void SetRegistered(bool state) { m_bRegistered = state; }

protected:
    virtual void Create(const char *pName, const char *pHelpString = 0, int flags = 0) {
        m_pszName = pName;
        m_pszHelpString = pHelpString;
        m_nFlags = flags;
    }

    virtual void Init() {}

private:
    // 这段字段布局必须与 SDK 完全一致，不要动
    ConCommandBase *m_pNext;
    bool m_bRegistered;
    const char *m_pszName;
    const char *m_pszHelpString;
    int m_nFlags;

protected:
    static ConCommandBase *s_pConCommandBases;
    static IConCommandBaseAccessor *s_pAccessor;
};

//----------------------------------------------------------------------------------------
// ConCommand —— 逐字对齐 l4d2 convar.h 的虚函数顺序
//----------------------------------------------------------------------------------------
class ConCommand : public ConCommandBase {
public:
    ConCommand(const char *pName, FnCommandCallbackV1_t callback, const char *pHelpString = 0,
               int flags = 0)
        : ConCommandBase(pName, pHelpString, flags), m_bUsingNewCommandCallback(false) {
        m_fnCommandCallbackV1 = callback;
    }

    ConCommand(const char *pName, FnCommandCallback_t callback, const char *pHelpString = 0,
               int flags = 0)
        : ConCommandBase(pName, pHelpString, flags), m_bUsingNewCommandCallback(true) {
        m_fnCommandCallback = callback;
    }

    virtual ~ConCommand(void) {}

    virtual bool IsCommand(void) const { return true; }

    virtual int AutoCompleteSuggest(const char *partial, void *commands) {
        (void)partial;
        (void)commands;
        return 0;
    }

    virtual bool CanAutoComplete(void) { return false; }

    // 引擎在控制台敲命令时走到这里
    virtual void Dispatch(const CCommand &command) {
        if (m_bUsingNewCommandCallback) {
            if (m_fnCommandCallback) m_fnCommandCallback(command);
        } else if (m_fnCommandCallbackV1) {
            m_fnCommandCallbackV1();
        }
    }

private:
    union {
        FnCommandCallbackV1_t m_fnCommandCallbackV1;
        FnCommandCallback_t m_fnCommandCallback;
        void *m_pCommandCallback;
    };
    union {
        void *m_fnCompletionCallback;
        void *m_pCommandCompletionCallback;
    };
    bool m_bHasCompletionCallback : 1;
    bool m_bUsingNewCommandCallback : 1;
    bool m_bUsingCommandCallbackInterface : 1;
};

//----------------------------------------------------------------------------------------
// ICvar —— 只声明我们要用到的槽位。为了偏移绝对正确，前面的槽位必须全部补全。
//   IAppSystem: 0 QueryInterface 1 Connect 2 Disconnect 3 Init 4 Shutdown
//   ICvar:      5 AllocateDLLIdentifier 6 RegisterConCommand 7 UnregisterConCommand
//               8 UnregisterConCommands 9 GetCommandLineValue 10 FindCommandBase(non-const)
//               11 FindCommandBase(const) 12 FindVar 13 FindVar(const)
//               14 FindCommand 15 FindCommand(const)
//               16 InstallGlobalChangeCallback 17 RemoveGlobalChangeCallback
//               18 CallGlobalChangeCallbacks 19 InstallConsoleDisplayFunc
//               20 RemoveConsoleDisplayFunc 21 ConsoleColorPrintf 22 ConsolePrintf
//
// 我们实际用到：
//   * RegisterConCommand / UnregisterConCommand —— 注册 perf_* 指令（必须，否则引擎
//     会直接报 Unknown command，插件的 ClientCommand 永远收不到）
//   * ConsolePrintf —— 往服务器控制台打印。这是本地 listen server 下唯一能保证
//     “玩家在自己控制台看得到”的输出通道（C 运行时的 stdout 在游戏里不通）
//----------------------------------------------------------------------------------------
class ICvar {
public:
    // ---- IAppSystem ----
    virtual void *QueryInterface(const char *pInterfaceName) = 0;
    virtual void *Connect(void *pFactory) = 0;
    virtual void Disconnect(void) = 0;
    virtual void *Init(void) = 0;
    virtual void Shutdown(void) = 0;

    // ---- ICvar ----
    virtual CVarDLLIdentifier_t AllocateDLLIdentifier() = 0;
    virtual void RegisterConCommand(ConCommandBase *pCommandBase) = 0;
    virtual void UnregisterConCommand(ConCommandBase *pCommandBase) = 0;
    virtual void UnregisterConCommands(CVarDLLIdentifier_t id) = 0;
    virtual const char *GetCommandLineValue(const char *pVariableName) = 0;
    virtual ConCommandBase *FindCommandBase(const char *name) = 0;
    virtual const ConCommandBase *FindCommandBase(const char *name) const = 0;
    virtual ConVar *FindVar(const char *var_name) = 0;
    virtual const ConVar *FindVar(const char *var_name) const = 0;
    virtual ConCommandBase *FindCommand(const char *name) = 0;
    virtual const ConCommandBase *FindCommand(const char *name) const = 0;
    virtual void InstallGlobalChangeCallback(void *callback) = 0;
    virtual void RemoveGlobalChangeCallback(void *callback) = 0;
    virtual void CallGlobalChangeCallbacks(ConVar *var, const char *pOldString,
                                           float flOldValue) = 0;
    virtual void InstallConsoleDisplayFunc(void *pDisplayFunc) = 0;
    virtual void RemoveConsoleDisplayFunc(void *pDisplayFunc) = 0;
    virtual void ConsoleColorPrintf(const void *clr, const char *pFormat, ...) = 0;
    virtual void ConsolePrintf(const char *pFormat, ...) = 0;
};

//----------------------------------------------------------------------------------------
// 接口注册 / 导出机制（对应 tier1/interface.h）
//----------------------------------------------------------------------------------------
#if defined(_WIN32)
#define PS_DLL_EXPORT extern "C" __declspec(dllexport)
#else
#define PS_DLL_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define CREATEINTERFACE_PROCNAME "CreateInterface"

class InterfaceReg {
public:
    InterfaceReg(InstantiateInterfaceFn fn, const char *pName);
    InstantiateInterfaceFn m_CreateFn;
    const char *m_pName;
    InterfaceReg *m_pNext;
};

#endif  // PERFSTAT_PLUGIN_API_H
