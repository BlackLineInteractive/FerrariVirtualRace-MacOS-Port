// DirectDraw: only used by the game to query video memory and set a cooperative level.
#include "rt.h"
#include <cstring>

static constexpr uint32_t VIDMEM = 0x40000000;   // report 1 GiB (fits the 32-bit DWORD)

static void dd_QueryInterface(CPU *c) { W32(ARG(2), ARG(0)); RETN(3, 0); }
static void dd_AddRef(CPU *c) { uint32_t o = ARG(0); W32(o + 4, R32(o + 4) + 1); RETN(1, R32(o + 4)); }
static void dd_Release(CPU *c) { uint32_t o = ARG(0); uint32_t r = R32(o + 4) - 1; W32(o + 4, r); RETN(1, r); }
static void dd_SetCooperativeLevel(CPU *c) { RETN(3, 0); }
static void dd_GetAvailableVidMem(CPU *c) {
    if (ARG(2)) W32(ARG(2), VIDMEM);
    if (ARG(3)) W32(ARG(3), VIDMEM - 0x1000000);
    RETN(4, 0);
}
static void dd_GetCaps(CPU *c) {
    // DDCAPS: dwSize at 0, dwVidMemTotal at 0x3c, dwVidMemFree at 0x40
    for (int i = 1; i <= 2; i++) if (uint32_t p = ARG(i)) {
        uint32_t sz = R32(p);
        memset(H(p + 4), 0, sz > 4 ? sz - 4 : 0);
        if (sz >= 0x44) { W32(p + 0x3c, VIDMEM); W32(p + 0x40, VIDMEM - 0x1000000); }
    }
    RETN(3, 0);
}
static void dd_RestoreDisplayMode(CPU *c) { RETN(1, 0); }
static void dd_GetDeviceIdentifier(CPU *c) {
    uint32_t p = ARG(1);
    memset(H(p), 0, 0x428 + 16 + 4 * 4 + 16 + 4);
    gstrcpy(p, "macgl", 512);
    gstrcpy(p + 512, "AMD Radeon (OpenGL on macOS)", 512);
    W32(p + 0x410, 0x1002);   // dwVendorId (after LARGE_INTEGER driver version at 0x400)
    RETN(3, 0);
}

static uint32_t make_dd() {
    static uint32_t vt;
    if (!vt) {
        HostFn m[30] = {};
        m[0] = dd_QueryInterface; m[1] = dd_AddRef; m[2] = dd_Release;
        m[11] = dd_GetCaps; m[19] = dd_RestoreDisplayMode; m[20] = dd_SetCooperativeLevel;
        m[23] = dd_GetAvailableVidMem; m[27] = dd_GetDeviceIdentifier;
        vt = com_make_vtable("IDirectDraw7", m, 30);
    }
    uint32_t o = com_new_object(vt, 4);
    W32(o + 4, 1);
    return o;
}

API(ddraw, DirectDrawCreateEx, 4) { W32(ARG(1), make_dd()); RETN(4, 0); }
API(ddraw, DirectDrawCreate, 3) { W32(ARG(1), make_dd()); RETN(3, 0); }
