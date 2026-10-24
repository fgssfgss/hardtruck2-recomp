/*
 * dplay.c - DirectPlay for king.exe: IDirectPlay4A and IDirectPlayLobby3A
 * that find nothing to connect to (LongRoad2's src/shim/misc/dplay.c, on the
 * COM bridge).
 *
 * Not optional, even single-player: when CoCreateInstance(CLSID_DirectPlay)
 * fails the game logs "Can not create DirectPlay4 object" through its warning
 * routine, and that routine ends in FUN_005dcb90, which parks any thread it
 * does not recognise -- including the main thread during InitInstance, before
 * the WM_TIMER that would have registered it ever arrives. A spin, forever.
 *
 * Real answers only for what startup asks: Initialize succeeds, the
 * enumerations find nothing, GetCaps is zeroed; every other method fails.
 */
#include <stdlib.h>
#include <string.h>
#include "com_bridge.h"
#include "../gfx/gfx_mem.h"

#define DP_OK 0u

typedef struct { uint32_t refcount; } dp_obj_t;

static const uint8_t CLSID_DirectPlay_[16] =      /* {D1EB6D20-8923-11D0-9D97-00A0C90A43CB} */
    { 0x20,0x6D,0xEB,0xD1, 0x23,0x89, 0xD0,0x11, 0x9D,0x97,0x00,0xA0,0xC9,0x0A,0x43,0xCB };
static const uint8_t CLSID_DirectPlayLobby_[16] = /* {2FE8F810-B2A5-11D0-A787-0000F803ABFC} */
    { 0x10,0xF8,0xE8,0x2F, 0xA5,0xB2, 0xD0,0x11, 0xA7,0x87,0x00,0x00,0xF8,0x03,0xAB,0xFC };

/* QueryInterface is "yes, this object" for anything, as LongRoad2's was: the
 * game only asks these for their own interface versions. */
#define DP_COMMON(kind) \
    static void kind##_QueryInterface(void) { \
        COM_SELF(dp_obj_t, kind, 2); self->refcount++; com_out(A32(2), self, kind); RET(DP_OK, 3); } \
    static void kind##_AddRef(void) { COM_SELF(dp_obj_t, kind, 0); RET(++self->refcount, 1); } \
    static void kind##_Release(void) { \
        COM_SELF(dp_obj_t, kind, 0); uint32_t rc = --self->refcount; \
        if (rc == 0) { gfx_native_freed(self); free(self); } \
        RET(rc, 1); }
DP_COMMON(CK_DPLAY4)
DP_COMMON(CK_DPLOBBY3)

/* ---- IDirectPlayLobby3A ---- */
static void l_EnumLocalApplications(void) { COM_SELF(dp_obj_t, CK_DPLOBBY3, 3); RET(DP_OK, 4); }
static const win32hle_shim k_lobby[] = {
    { "QueryInterface", CK_DPLOBBY3_QueryInterface }, { "AddRef", CK_DPLOBBY3_AddRef },
    { "Release", CK_DPLOBBY3_Release },
    { "Connect", com_fail3 }, { "CreateAddress", com_fail6 }, { "EnumAddress", com_fail4 },
    { "EnumAddressTypes", com_fail4 }, { "EnumLocalApplications", l_EnumLocalApplications },
    { "GetConnectionSettings", com_fail3 }, { "ReceiveLobbyMessage", com_fail5 },
    { "RunApplication", com_fail4 }, { "SendLobbyMessage", com_fail4 },
    { "SetConnectionSettings", com_fail3 }, { "SetLobbyMessageEvent", com_fail3 },
    { "CreateCompoundAddress", com_fail4 }, { "ConnectEx", com_fail4 },
    { "RegisterApplication", com_fail2 }, { "UnregisterApplication", com_fail2 },
    { "WaitForConnectionSettings", com_fail1 },
};

/* ---- IDirectPlay4A ---- */
static void p_EnumSessions(void)    { COM_SELF(dp_obj_t, CK_DPLAY4, 5); RET(DP_OK, 6); }   /* found none */
static void p_EnumConnections(void) { COM_SELF(dp_obj_t, CK_DPLAY4, 4); RET(DP_OK, 5); }
static void p_Initialize(void)      { COM_SELF(dp_obj_t, CK_DPLAY4, 1); RET(DP_OK, 2); }
static void p_GetCaps(void) {                    /* (DPCAPS*, flags): dwSize honoured, the rest zero */
    COM_SELF(dp_obj_t, CK_DPLAY4, 2);
    uint32_t *caps = COM_P(1);
    if (!caps) RET(0x88780001u, 3);
    if (caps[0] > 4) memset(caps + 1, 0, caps[0] - 4);
    RET(DP_OK, 3);
}
/* com_fail9 does not exist: SendEx is the one 9-argument method. */
static void p_SendEx(void) { COM_SELF(dp_obj_t, CK_DPLAY4, 9); RET(0x88780001u, 10); }
static const win32hle_shim k_dplay4[] = {
    { "QueryInterface", CK_DPLAY4_QueryInterface }, { "AddRef", CK_DPLAY4_AddRef },
    { "Release", CK_DPLAY4_Release },
    { "AddPlayerToGroup", com_fail2 }, { "Close", com_fail0 }, { "CreateGroup", com_fail5 },
    { "CreatePlayer", com_fail6 }, { "DeletePlayerFromGroup", com_fail2 }, { "DestroyGroup", com_fail1 },
    { "DestroyPlayer", com_fail1 }, { "EnumGroupPlayers", com_fail5 }, { "EnumGroups", com_fail4 },
    { "EnumPlayers", com_fail4 }, { "EnumSessions", p_EnumSessions }, { "GetCaps", p_GetCaps },
    { "GetGroupData", com_fail4 }, { "GetGroupName", com_fail3 }, { "GetMessageCount", com_fail2 },
    { "GetPlayerAddress", com_fail3 }, { "GetPlayerCaps", com_fail3 }, { "GetPlayerData", com_fail4 },
    { "GetPlayerName", com_fail3 }, { "GetSessionDesc", com_fail2 }, { "Initialize", p_Initialize },
    { "Open", com_fail2 }, { "Receive", com_fail5 }, { "Send", com_fail5 }, { "SetGroupData", com_fail4 },
    { "SetGroupName", com_fail3 }, { "SetPlayerData", com_fail4 }, { "SetPlayerName", com_fail3 },
    { "SetSessionDesc", com_fail2 },
    { "AddGroupToGroup", com_fail2 }, { "CreateGroupInGroup", com_fail6 }, { "DeleteGroupFromGroup", com_fail2 },
    { "EnumConnections", p_EnumConnections }, { "EnumGroupsInGroup", com_fail5 },
    { "GetGroupConnectionSettings", com_fail4 }, { "InitializeConnection", com_fail2 },
    { "SecureOpen", com_fail4 }, { "SendChatMessage", com_fail4 }, { "SetGroupConnectionSettings", com_fail3 },
    { "StartSession", com_fail2 }, { "GetGroupFlags", com_fail2 }, { "GetGroupParent", com_fail2 },
    { "GetPlayerAccount", com_fail4 }, { "GetPlayerFlags", com_fail2 },
    { "GetGroupOwner", com_fail2 }, { "SetGroupOwner", com_fail2 }, { "SendEx", p_SendEx },
    { "GetMessageQueue", com_fail5 }, { "CancelMessage", com_fail2 }, { "CancelPriority", com_fail3 },
};

static uint32_t create(com_kind_t kind, uint32_t *out) {
    dp_obj_t *o = calloc(1, sizeof *o);
    o->refcount = 1;
    *out = com_wrap(o, kind);
    return DP_OK;
}
static uint32_t create_dplay(const uint8_t *iid, uint32_t *out) { (void)iid; return create(CK_DPLAY4, out); }
static uint32_t create_lobby(const uint8_t *iid, uint32_t *out) { (void)iid; return create(CK_DPLOBBY3, out); }

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
void ht2_dplay_register(void) {
    com_define(CK_DPLAY4,   "IDirectPlay4A",      k_dplay4, N(k_dplay4));
    com_define(CK_DPLOBBY3, "IDirectPlayLobby3A", k_lobby,  N(k_lobby));
    hle_com_register_class(CLSID_DirectPlay_, create_dplay);
    hle_com_register_class(CLSID_DirectPlayLobby_, create_lobby);
}
_Static_assert(N(k_dplay4) == 3 + 29 + 15 + 6, "IDirectPlay4A: IUnknown + IDirectPlay2 + 3 + 4 slots");
_Static_assert(N(k_lobby)  == 3 + 11 + 1 + 4,  "IDirectPlayLobby3A: IUnknown + Lobby + 2 + 3 slots");
