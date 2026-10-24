/*
 * dinput.c - DirectInput for king.exe: an IDirectInput7A with no devices.
 *
 * LongRoad2's traces of this game (docs/api-surface/dinput.md there): it
 * creates DirectInput, enumerates, and releases it -- never a device acquired
 * or polled. Keyboard and mouse reach it as window messages (gfx_host.c), so
 * an enumeration that finds nothing is the whole job. A gamepad, later, would
 * be a device this enumerates, fed from SDL.
 */
#include <stdio.h>
#include <stdlib.h>
#include "com_bridge.h"
#include "../gfx/gfx_mem.h"

#define DI_OK               0u
#define DIERR_DEVICENOTREG  0x80040154u

typedef struct { uint32_t refcount; } dinput_t;

static const uint8_t IID_IUnknown_[16] = { 0,0,0,0, 0,0, 0,0, 0xC0,0,0,0,0,0,0,0x46 };
static const uint8_t IID_IDirectInputA_[16] =    /* {89521360-AA8A-11CF-BFC7-444553540000} */
    { 0x60,0x13,0x52,0x89, 0x8A,0xAA, 0xCF,0x11, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };
static const uint8_t IID_IDirectInput2A_[16] =   /* {5944E662-AA8A-11CF-BFC7-444553540000} */
    { 0x62,0xE6,0x44,0x59, 0x8A,0xAA, 0xCF,0x11, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };
static const uint8_t IID_IDirectInput7A_[16] =   /* {9A4CB684-236D-11D3-8E9D-00C04F6844AE} */
    { 0x84,0xB6,0x4C,0x9A, 0x6D,0x23, 0xD3,0x11, 0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE };

static void x_DirectInputCreateA(void) {         /* (HINSTANCE, DWORD version, LPDIRECTINPUTA*, outer) */
    dinput_t *di = calloc(1, sizeof *di);
    di->refcount = 1;
    fprintf(stderr, "[dinput] DirectInputCreateA (version 0x%04X): no devices\n", A32(1));
    com_out(A32(2), di, CK_DINPUT);
    RET(DI_OK, 4);
}

static void i_QueryInterface(void) {
    COM_SELF(dinput_t, CK_DINPUT, 2);
    if (com_guid_is(A32(1), IID_IUnknown_) || com_guid_is(A32(1), IID_IDirectInputA_) ||
        com_guid_is(A32(1), IID_IDirectInput2A_) || com_guid_is(A32(1), IID_IDirectInput7A_)) {
        self->refcount++;
        com_out(A32(2), self, CK_DINPUT);
        RET(DI_OK, 3);
    }
    com_out(A32(2), NULL, CK_NONE);
    RET(COM_E_NOINTERFACE, 3);
}
static void i_AddRef(void) { COM_SELF(dinput_t, CK_DINPUT, 0); RET(++self->refcount, 1); }
static void i_Release(void) {
    COM_SELF(dinput_t, CK_DINPUT, 0);
    uint32_t rc = --self->refcount;
    if (rc == 0) { gfx_native_freed(self); free(self); }
    RET(rc, 1);
}
static void i_CreateDevice(void) {               /* (REFGUID, LPDIRECTINPUTDEVICEA*, outer): there are none */
    COM_SELF(dinput_t, CK_DINPUT, 3);
    com_out(A32(2), NULL, CK_NONE);
    RET(DIERR_DEVICENOTREG, 4);
}
static void i_EnumDevices(void) { COM_SELF(dinput_t, CK_DINPUT, 4); RET(DI_OK, 5); }   /* (type, cb, ref, flags) */
static void i_GetDeviceStatus(void) { COM_SELF(dinput_t, CK_DINPUT, 1); RET(DIERR_DEVICENOTREG, 2); }
static void i_RunControlPanel(void) { COM_SELF(dinput_t, CK_DINPUT, 2); RET(DI_OK, 3); }
static void i_Initialize(void) { COM_SELF(dinput_t, CK_DINPUT, 2); RET(DI_OK, 3); }
static void i_FindDevice(void) { COM_SELF(dinput_t, CK_DINPUT, 3); RET(DIERR_DEVICENOTREG, 4); }        /* 2A */
static void i_CreateDeviceEx(void) {             /* 7A: (REFGUID, REFIID, LPVOID*, outer) */
    COM_SELF(dinput_t, CK_DINPUT, 4);
    com_out(A32(3), NULL, CK_NONE);
    RET(DIERR_DEVICENOTREG, 5);
}

static const win32hle_shim k_dinput[] = {
    { "QueryInterface", i_QueryInterface }, { "AddRef", i_AddRef }, { "Release", i_Release },
    { "CreateDevice", i_CreateDevice }, { "EnumDevices", i_EnumDevices },
    { "GetDeviceStatus", i_GetDeviceStatus }, { "RunControlPanel", i_RunControlPanel },
    { "Initialize", i_Initialize }, { "FindDevice", i_FindDevice }, { "CreateDeviceEx", i_CreateDeviceEx },
};
static const win32hle_shim k_imports[] = { { "DirectInputCreateA", x_DirectInputCreateA }, { 0, 0 } };

void ht2_dinput_register(void) {
    win32hle_register(k_imports);
    com_define(CK_DINPUT, "IDirectInput7A", k_dinput, (int)(sizeof k_dinput / sizeof k_dinput[0]));
}
