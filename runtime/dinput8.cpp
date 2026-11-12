// DirectInput 8 replacement on SDL3: system keyboard, system mouse and joysticks / wheels / gamepads.
// Device state is produced in the layout the game selects with SetDataFormat (c_dfDIKeyboard,
// c_dfDIMouse2, c_dfDIJoystick2 or a custom format), and buffered data is derived by diffing states.
#include "rt.h"
#include "win32ui.h"
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <cstring>
#include <algorithm>
#include <array>

enum : uint32_t { DI_OK = 0, DI_BUFFEROVERFLOW = 1, DIERR_INVALIDPARAM = 0x80070057, DIERR_NOTFOUND = 0x80070002,
                  DIERR_UNSUPPORTED = 0x80004001, DIERR_NOINTERFACE = 0x80004002, DIERR_DEVICENOTREG = 0x80040154 };
enum : uint32_t { DIDFT_RELAXIS = 1, DIDFT_ABSAXIS = 2, DIDFT_AXIS = 3, DIDFT_PSHBUTTON = 4, DIDFT_TGLBUTTON = 8,
                  DIDFT_BUTTON = 0xC, DIDFT_POV = 0x10, DIDFT_ANYINSTANCE = 0x00FFFF00, DIDFT_OPTIONAL = 0x80000000 };

struct GUIDv { uint32_t a; uint16_t b, c; uint8_t d[8]; };
static bool guid_eq(const GUIDv &x, const GUIDv &y) { return memcmp(&x, &y, 16) == 0; }
static GUIDv rd_guid(uint32_t p) { GUIDv g; memcpy(&g, H(p), 16); return g; }
static const uint8_t BFC7[8] = {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static GUIDv mk(uint32_t a, uint16_t b, uint16_t c) { GUIDv g{a, b, c, {}}; memcpy(g.d, BFC7, 8); return g; }
static const GUIDv GUID_XAxis = mk(0xA36D02E0, 0xC9F3, 0x11CF), GUID_YAxis = mk(0xA36D02E1, 0xC9F3, 0x11CF),
                   GUID_ZAxis = mk(0xA36D02E2, 0xC9F3, 0x11CF), GUID_RxAxis = mk(0xA36D02F4, 0xC9F3, 0x11CF),
                   GUID_RyAxis = mk(0xA36D02F5, 0xC9F3, 0x11CF), GUID_RzAxis = mk(0xA36D02E3, 0xC9F3, 0x11CF),
                   GUID_Slider = mk(0xA36D02E4, 0xC9F3, 0x11CF), GUID_Button = mk(0xA36D02F0, 0xC9F3, 0x11CF),
                   GUID_Key = mk(0x55728220, 0xD33C, 0x11CF), GUID_POV = mk(0xA36D02F2, 0xC9F3, 0x11CF),
                   GUID_SysMouse = mk(0x6F1D2B60, 0xD5A0, 0x11CF), GUID_SysKeyboard = mk(0x6F1D2B61, 0xD5A0, 0x11CF);

enum DevKind { DK_KEYBOARD, DK_MOUSE, DK_JOY };

struct DevObj {            // an object (axis/button/pov/key) of a physical device
    GUIDv guid;
    uint32_t type;         // DIDFT_* | instance << 8
    int index;             // SDL axis / button / hat index, DIK code, mouse axis
    char name[32];
};
struct Mapping { int obj; uint32_t ofs; };

struct Dev {
    uint32_t guest = 0;
    int refs = 1;
    DevKind kind;
    SDL_JoystickID jid = 0;
    SDL_Joystick *joy = nullptr;
    std::vector<DevObj> objs;
    std::vector<Mapping> map;
    uint32_t data_size = 0;
    std::vector<int32_t> range_min, range_max, deadzone;   // per object
    std::vector<uint8_t> last;       // last produced state (for buffered diffs)
    std::deque<std::array<uint32_t, 5>> events;
    uint32_t buffer_size = 0, seq = 0;
    bool acquired = false, overflow = false;
    std::string name;
};

static std::mutex g_di;
static std::map<uint32_t, Dev *> g_devs;
static uint32_t g_vt_di, g_vt_dev, g_di_obj;
static float g_wheel = 0;

static void di_event_hook(const SDL_Event &e) {
    if (e.type == SDL_EVENT_MOUSE_WHEEL) { std::lock_guard<std::mutex> l(g_di); g_wheel += e.wheel.y * 120.0f; }
}

// ---------------------------------------------------------------- device objects
static void add_obj(Dev *d, GUIDv g, uint32_t type, int idx, const char *nm) {
    DevObj o{g, type, idx, {}};
    snprintf(o.name, sizeof o.name, "%s", nm);
    d->objs.push_back(o);
    d->range_min.push_back(0); d->range_max.push_back(65535); d->deadzone.push_back(0);
}
static void build_objects(Dev *d) {
    char nm[32];
    if (d->kind == DK_KEYBOARD) {
        for (int k = 0; k < 256; k++) { snprintf(nm, sizeof nm, "Key %d", k); add_obj(d, GUID_Key, DIDFT_PSHBUTTON | ((uint32_t)k << 8), k, nm); }
    } else if (d->kind == DK_MOUSE) {
        add_obj(d, GUID_XAxis, DIDFT_RELAXIS | (0 << 8), 0, "X-axis");
        add_obj(d, GUID_YAxis, DIDFT_RELAXIS | (1 << 8), 1, "Y-axis");
        add_obj(d, GUID_ZAxis, DIDFT_RELAXIS | (2 << 8), 2, "Wheel");
        for (int b = 0; b < 8; b++) { snprintf(nm, sizeof nm, "Button %d", b); add_obj(d, GUID_Button, DIDFT_PSHBUTTON | ((uint32_t)(3 + b) << 8), b, nm); }
    } else {
        int na = SDL_GetNumJoystickAxes(d->joy), nb = SDL_GetNumJoystickButtons(d->joy), nh = SDL_GetNumJoystickHats(d->joy);
        const GUIDv *ag[6] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis};
        static const char *an[6] = {"X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation"};
        for (int i = 0; i < na; i++) add_obj(d, i < 6 ? *ag[i] : GUID_Slider, DIDFT_ABSAXIS | ((uint32_t)i << 8), i, i < 6 ? an[i] : "Slider");
        for (int i = 0; i < nh; i++) add_obj(d, GUID_POV, DIDFT_POV | ((uint32_t)i << 8), i, "POV");
        for (int i = 0; i < nb; i++) { snprintf(nm, sizeof nm, "Button %d", i); add_obj(d, GUID_Button, DIDFT_PSHBUTTON | ((uint32_t)i << 8), i, nm); }
    }
}
static uint32_t obj_size(const DevObj &o) { return (o.type & DIDFT_BUTTON) ? 1 : 4; }

// ---------------------------------------------------------------- state
static int32_t axis_value(Dev *d, int oi, int32_t raw) {   // raw -32768..32767
    int32_t mn = d->range_min[oi], mx = d->range_max[oi];
    int32_t dz = d->deadzone[oi];
    if (dz > 0) {
        int32_t lim = (int32_t)(32767LL * dz / 10000);
        if (raw > -lim && raw < lim) raw = 0;
        else raw = raw > 0 ? (int32_t)((int64_t)(raw - lim) * 32767 / std::max(1, 32767 - lim))
                           : (int32_t)((int64_t)(raw + lim) * 32768 / std::max(1, 32768 - lim));
    }
    return mn + (int32_t)(((int64_t)raw + 32768) * ((int64_t)mx - mn) / 65535);
}

static void produce_state(Dev *d, std::vector<uint8_t> &st) {
    st.assign(d->data_size, 0);
    if (d->kind == DK_KEYBOARD) {
        int n = 0;
        const bool *ks = SDL_GetKeyboardState(&n);
        uint8_t keys[256] = {};
        for (int s = 0; s < n; s++) if ((ks[s] && !ui_scripted()) || ui_key_injected((SDL_Scancode)s)) { uint32_t k = ui_dik_of((SDL_Scancode)s); if (k && k < 256) keys[k] = 0x80; }
        for (auto &m : d->map) if (m.ofs < st.size()) st[m.ofs] = keys[d->objs[m.obj].index];
        return;
    }
    if (d->kind == DK_MOUSE) {
        float dx = 0, dy = 0;
        SDL_MouseButtonFlags b = SDL_GetRelativeMouseState(&dx, &dy);
        if (ui_scripted()) { dx = dy = 0; b = 0; }
        int32_t wz = (int32_t)g_wheel; g_wheel -= (float)wz;
        for (auto &m : d->map) {
            const DevObj &o = d->objs[m.obj];
            if (m.ofs + obj_size(o) > st.size()) continue;
            if (o.type & DIDFT_AXIS) {
                int32_t v = o.index == 0 ? (int32_t)dx : o.index == 1 ? (int32_t)dy : wz;
                memcpy(&st[m.ofs], &v, 4);
            } else {
                int sdlb = o.index == 0 ? 1 : o.index == 1 ? 3 : o.index == 2 ? 2 : o.index + 1;
                st[m.ofs] = (b & SDL_BUTTON_MASK(sdlb)) ? 0x80 : 0;
            }
        }
        return;
    }
    if (!d->joy) return;
    for (auto &m : d->map) {
        const DevObj &o = d->objs[m.obj];
        if (m.ofs + obj_size(o) > st.size()) continue;
        if (o.type & DIDFT_AXIS) {
            int32_t v = axis_value(d, m.obj, SDL_GetJoystickAxis(d->joy, o.index));
            memcpy(&st[m.ofs], &v, 4);
        } else if (o.type & DIDFT_POV) {
            uint8_t h = SDL_GetJoystickHat(d->joy, o.index);
            static const int32_t ang[16] = {-1, 0, 9000, 4500, 18000, -1, 13500, -1, 27000, 31500, -1, -1, 22500, -1, -1, -1};
            int32_t v = ang[h & 15];
            memcpy(&st[m.ofs], &v, 4);
        } else st[m.ofs] = SDL_GetJoystickButton(d->joy, o.index) ? 0x80 : 0;
    }
    // unmapped POV slots in the format must read as centered
}

static void poll_dev(Dev *d, std::vector<uint8_t> &st) {
    produce_state(d, st);
    if (!d->buffer_size) { d->last = st; return; }
    if (d->last.size() != st.size()) d->last.assign(st.size(), 0);
    uint32_t now = (uint32_t)SDL_GetTicks();
    for (auto &m : d->map) {
        const DevObj &o = d->objs[m.obj];
        uint32_t sz = obj_size(o);
        if (m.ofs + sz > st.size()) continue;
        uint32_t nv = 0, ov = 0;
        memcpy(&nv, &st[m.ofs], sz); memcpy(&ov, &d->last[m.ofs], sz);
        bool rel = d->kind == DK_MOUSE && (o.type & DIDFT_AXIS);
        if (rel ? nv != 0 : nv != ov) {
            if (d->events.size() >= d->buffer_size) { d->events.pop_front(); d->overflow = true; }
            d->events.push_back({m.ofs, nv, now, d->seq++, 0});
        }
    }
    d->last = st;
}

// ---------------------------------------------------------------- data format mapping
static void set_format(Dev *d, uint32_t fmt) {
    uint32_t objsize = R32(fmt + 4), datasize = R32(fmt + 12), nobj = R32(fmt + 16), arr = R32(fmt + 20);
    d->data_size = datasize;
    d->map.clear();
    std::vector<bool> used(d->objs.size(), false);
    for (uint32_t i = 0; i < nobj; i++) {
        uint32_t p = arr + i * objsize;
        uint32_t pg = R32(p), ofs = R32(p + 4), type = R32(p + 8);
        GUIDv g{};
        if (pg) g = rd_guid(pg);
        uint32_t inst = (type >> 8) & 0xFFFF;
        bool any = inst == 0xFFFF;
        for (size_t k = 0; k < d->objs.size(); k++) {
            if (used[k]) continue;
            const DevObj &o = d->objs[k];
            if (pg && !guid_eq(g, o.guid)) continue;
            if ((type & DIDFT_AXIS) && !(o.type & DIDFT_AXIS)) continue;
            if ((type & DIDFT_BUTTON) && !(o.type & DIDFT_BUTTON)) continue;
            if ((type & DIDFT_POV) && !(o.type & DIDFT_POV)) continue;
            if (!any && ((o.type >> 8) & 0xFFFF) != inst) continue;
            used[k] = true;
            d->map.push_back({(int)k, ofs});
            break;
        }
    }
    d->last.assign(datasize, 0);
}

// ---------------------------------------------------------------- joystick enumeration
static GUIDv joy_instance_guid(SDL_JoystickID id) { GUIDv g{0x4A0E0000u + (uint32_t)id, 0x4A0E, 0x11D2, {0x80, 0, 0x53, 0x44, 0x4C, 0x33, 0, 0}}; return g; }
static GUIDv joy_product_guid(SDL_JoystickID id) {
    uint16_t vid = SDL_GetJoystickVendorForID(id), pid = SDL_GetJoystickProductForID(id);
    GUIDv g{((uint32_t)pid << 16) | vid, 0, 0, {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'}};
    return g;
}
static uint32_t joy_devtype(SDL_JoystickID id) {
    SDL_JoystickType t = SDL_GetJoystickTypeForID(id);
    if (t == SDL_JOYSTICK_TYPE_WHEEL) return 0x16 | (2 << 8) | 0x10000;
    if (t == SDL_JOYSTICK_TYPE_GAMEPAD || SDL_IsGamepad(id)) return 0x15 | (1 << 8) | 0x10000;
    return 0x14 | (1 << 8) | 0x10000;
}
static void write_instance(uint32_t p, GUIDv inst, GUIDv prod, uint32_t devtype, const char *name) {
    memset(H(p + 4), 0, 576);
    W32(p, 580);
    memcpy(H(p + 4), &inst, 16); memcpy(H(p + 20), &prod, 16);
    W32(p + 36, devtype);
    gstrcpy(p + 40, name, 260); gstrcpy(p + 300, name, 260);
    W16(p + 576, 1); W16(p + 578, (devtype & 0xFF) == 0x16 ? 4 : (devtype & 0xFF) == 0x15 ? 5 : 4);
}

#define M(name) static void name(CPU *c)
#define LOCK std::lock_guard<std::mutex> lk_(g_di)
static Dev *dev_of(uint32_t g) { auto it = g_devs.find(g); return it == g_devs.end() ? nullptr : it->second; }

// ---------------------------------------------------------------- IDirectInputDevice8A
M(d_QueryInterface) { LOCK; Dev *d = dev_of(ARG(0)); if (d) d->refs++; W32(ARG(2), ARG(0)); RETN(3, DI_OK); }
M(d_AddRef) { LOCK; Dev *d = dev_of(ARG(0)); RETN(1, d ? ++d->refs : 0); }
M(d_Release) {
    LOCK; Dev *d = dev_of(ARG(0));
    if (!d) RETN(1, 0);
    if (--d->refs > 0) RETN(1, d->refs);
    if (d->joy) SDL_CloseJoystick(d->joy);
    g_devs.erase(d->guest); gfree(d->guest); delete d;
    RETN(1, 0);
}
M(d_GetCapabilities) {
    LOCK; Dev *d = dev_of(ARG(0)); uint32_t p = ARG(1);
    int axes = 0, buttons = 0, povs = 0;
    for (auto &o : d->objs) { if (o.type & DIDFT_AXIS) axes++; else if (o.type & DIDFT_POV) povs++; else buttons++; }
    uint32_t type = d->kind == DK_KEYBOARD ? 0x13 | (4 << 8) : d->kind == DK_MOUSE ? 0x12 | (2 << 8) : joy_devtype(d->jid);
    W32(p + 4, 1); W32(p + 8, type); W32(p + 12, axes); W32(p + 16, buttons); W32(p + 20, povs);
    for (int o = 24; o < 44; o += 4) W32(p + o, 0);
    RETN(2, DI_OK);
}
M(d_EnumObjects) {
    uint32_t cb = ARG(1), ref = ARG(2), flags = ARG(3);
    Dev *d;
    { LOCK; d = dev_of(ARG(0)); }
    uint32_t buf = gmalloc(316, true);
    for (size_t k = 0; k < d->objs.size(); k++) {
        const DevObj &o = d->objs[k];
        uint32_t f = flags & 0xFF;
        if (f && !(((f & DIDFT_AXIS) && (o.type & DIDFT_AXIS)) || ((f & DIDFT_BUTTON) && (o.type & DIDFT_BUTTON)) || ((f & DIDFT_POV) && (o.type & DIDFT_POV)))) continue;
        memset(H(buf), 0, 316);
        W32(buf, 316);
        memcpy(H(buf + 4), &o.guid, 16);
        uint32_t ofs = 0;
        for (auto &m : d->map) if (m.obj == (int)k) ofs = m.ofs;
        W32(buf + 20, ofs); W32(buf + 24, o.type); W32(buf + 28, (o.type & DIDFT_AXIS) ? 0x100 : 0);
        gstrcpy(buf + 32, o.name, 260);
        uint32_t args[2] = {buf, ref};
        if (guest_call(cur_cpu(), cb, 2, args) == 0) break;
    }
    gfree(buf);
    RETN(4, DI_OK);
}
static int prop_target(Dev *d, uint32_t hdr, std::vector<int> &out) {
    uint32_t obj = R32(hdr + 8), how = R32(hdr + 12);
    out.clear();
    for (size_t k = 0; k < d->objs.size(); k++) {
        if (how == 0) { if (d->objs[k].type & DIDFT_AXIS) out.push_back((int)k); }
        else if (how == 1) { for (auto &m : d->map) if (m.obj == (int)k && m.ofs == obj) out.push_back((int)k); }
        else if (how == 2) {
            uint32_t ot = d->objs[k].type;
            if (((ot >> 8) & 0xFFFF) == ((obj >> 8) & 0xFFFF) && (ot & obj & 0x1F)) out.push_back((int)k);
        }
    }
    return (int)out.size();
}
M(d_GetProperty) {
    LOCK; Dev *d = dev_of(ARG(0));
    uint32_t prop = ARG(1), hdr = ARG(2);
    std::vector<int> t;
    switch (prop) {
    case 1: W32(hdr + 16, d->buffer_size); RETN(3, DI_OK);
    case 2: W32(hdr + 16, d->kind == DK_MOUSE ? 1 : 0); RETN(3, DI_OK);       // AXISMODE
    case 3: W32(hdr + 16, 1); RETN(3, DI_OK);                                   // GRANULARITY
    case 4: prop_target(d, hdr, t); if (t.empty()) RETN(3, DIERR_NOTFOUND); W32(hdr + 16, d->range_min[t[0]]); W32(hdr + 20, d->range_max[t[0]]); RETN(3, DI_OK);
    case 5: prop_target(d, hdr, t); W32(hdr + 16, t.empty() ? 0 : d->deadzone[t[0]]); RETN(3, DI_OK);
    case 6: W32(hdr + 16, 10000); RETN(3, DI_OK);
    case 7: W32(hdr + 16, 10000); RETN(3, DI_OK);
    case 9: W32(hdr + 16, 0); RETN(3, DI_OK);
    case 26: W32(hdr + 16, d->joy ? ((uint32_t)SDL_GetJoystickProductForID(d->jid) << 16) | SDL_GetJoystickVendorForID(d->jid) : 0); RETN(3, DI_OK);  // VIDPID
    case 18: case 19: {   // INSTANCENAME / PRODUCTNAME (WCHAR[260])
        const std::string &n = d->name;
        for (size_t i = 0; i < 259; i++) W16(hdr + 16 + 2 * i, i < n.size() ? (uint8_t)n[i] : 0);
        W16(hdr + 16 + 2 * 259, 0);
        RETN(3, DI_OK);
    }
    default: RETN(3, DIERR_UNSUPPORTED);
    }
}
M(d_SetProperty) {
    LOCK; Dev *d = dev_of(ARG(0));
    uint32_t prop = ARG(1), hdr = ARG(2);
    std::vector<int> t;
    switch (prop) {
    case 1: d->buffer_size = std::min<uint32_t>(R32(hdr + 16), 4096); d->events.clear(); RETN(3, DI_OK);
    case 4: prop_target(d, hdr, t); for (int k : t) { d->range_min[k] = (int32_t)R32(hdr + 16); d->range_max[k] = (int32_t)R32(hdr + 20); } RETN(3, DI_OK);
    case 5: prop_target(d, hdr, t); for (int k : t) d->deadzone[k] = (int32_t)R32(hdr + 16); RETN(3, DI_OK);
    default: RETN(3, DI_OK);
    }
}
M(d_Acquire) { LOCK; Dev *d = dev_of(ARG(0)); d->acquired = true; RETN(1, DI_OK); }
M(d_Unacquire) { LOCK; Dev *d = dev_of(ARG(0)); d->acquired = false; RETN(1, DI_OK); }
M(d_GetDeviceState) {
    LOCK; Dev *d = dev_of(ARG(0));
    std::vector<uint8_t> st;
    poll_dev(d, st);
    uint32_t n = std::min<uint32_t>(ARG(1), (uint32_t)st.size());
    memset(H(ARG(2)), 0, ARG(1));
    memcpy(H(ARG(2)), st.data(), n);
    // centered POVs that are in the format but have no device object
    RETN(3, DI_OK);
}
M(d_GetDeviceData) {
    LOCK; Dev *d = dev_of(ARG(0));
    uint32_t osz = ARG(1), out = ARG(2), inout = ARG(3), flags = ARG(4);
    std::vector<uint8_t> st;
    poll_dev(d, st);
    uint32_t want = R32(inout), n = 0;
    bool peek = flags & 1;
    size_t i = 0;
    while (n < want && i < d->events.size()) {
        if (out) {
            uint32_t p = out + n * osz;
            auto &e = d->events[i];
            memset(H(p), 0, osz);
            for (int k = 0; k < 5 && 4 * k < (int)osz; k++) W32(p + 4 * k, e[k]);
        }
        n++; i++;
    }
    if (!peek) d->events.erase(d->events.begin(), d->events.begin() + i);
    W32(inout, n);
    uint32_t r = d->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    d->overflow = false;
    RETN(5, r);
}
M(d_SetDataFormat) { LOCK; Dev *d = dev_of(ARG(0)); set_format(d, ARG(1)); RETN(2, DI_OK); }
M(d_SetEventNotification) { RETN(2, DI_OK); }
M(d_SetCooperativeLevel) { RETN(3, DI_OK); }
M(d_GetObjectInfo) { RETN(4, DIERR_NOTFOUND); }
M(d_GetDeviceInfo) {
    LOCK; Dev *d = dev_of(ARG(0));
    if (d->kind == DK_JOY) write_instance(ARG(1), joy_instance_guid(d->jid), joy_product_guid(d->jid), joy_devtype(d->jid), d->name.c_str());
    else if (d->kind == DK_MOUSE) write_instance(ARG(1), GUID_SysMouse, GUID_SysMouse, 0x12 | (2 << 8), "Mouse");
    else write_instance(ARG(1), GUID_SysKeyboard, GUID_SysKeyboard, 0x13 | (4 << 8), "Keyboard");
    RETN(2, DI_OK);
}
M(d_RunControlPanel) { RETN(3, DI_OK); }
M(d_Initialize) { RETN(4, DI_OK); }
M(d_CreateEffect) { if (ARG(3)) W32(ARG(3), 0); RETN(5, DIERR_UNSUPPORTED); }
M(d_EnumEffects) { RETN(4, DI_OK); }
M(d_GetEffectInfo) { RETN(3, DIERR_UNSUPPORTED); }
M(d_GetForceFeedbackState) { RETN(2, DIERR_UNSUPPORTED); }
M(d_SendForceFeedbackCommand) { RETN(2, DIERR_UNSUPPORTED); }
M(d_EnumCreatedEffectObjects) { RETN(4, DI_OK); }
M(d_Escape) { RETN(2, DIERR_UNSUPPORTED); }
M(d_Poll) { RETN(1, DI_OK); }
M(d_SendDeviceData) { RETN(5, DIERR_UNSUPPORTED); }
M(d_EnumEffectsInFile) { RETN(5, DI_OK); }
M(d_WriteEffectToFile) { RETN(5, DIERR_UNSUPPORTED); }
M(d_BuildActionMap) { RETN(4, DIERR_UNSUPPORTED); }
M(d_SetActionMap) { RETN(4, DIERR_UNSUPPORTED); }
M(d_GetImageInfo) { RETN(2, DIERR_UNSUPPORTED); }

// ---------------------------------------------------------------- IDirectInput8A
M(di_QueryInterface) { W32(ARG(2), ARG(0)); RETN(3, DI_OK); }
M(di_AddRef) { RETN(1, 1); }
M(di_Release) { RETN(1, 0); }
M(di_CreateDevice) {
    GUIDv g = rd_guid(ARG(1));
    Dev *d = new Dev();
    if (guid_eq(g, GUID_SysKeyboard)) { d->kind = DK_KEYBOARD; d->name = "Keyboard"; }
    else if (guid_eq(g, GUID_SysMouse)) { d->kind = DK_MOUSE; d->name = "Mouse"; }
    else {
        d->kind = DK_JOY;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetJoysticks(&n);
        for (int i = 0; i < n; i++) if (guid_eq(g, joy_instance_guid(ids[i]))) d->jid = ids[i];
        SDL_free(ids);
        if (d->jid) d->joy = SDL_OpenJoystick(d->jid);
        if (!d->joy) { delete d; W32(ARG(2), 0); RETN(4, DIERR_DEVICENOTREG); }
        const char *nm = SDL_GetJoystickName(d->joy);
        d->name = nm ? nm : "Joystick";
        rtlog("dinput: opened joystick '%s' (%d axes, %d buttons, %d hats)", d->name.c_str(),
              SDL_GetNumJoystickAxes(d->joy), SDL_GetNumJoystickButtons(d->joy), SDL_GetNumJoystickHats(d->joy));
    }
    build_objects(d);
    LOCK;
    d->guest = com_new_object(g_vt_dev, 4);
    g_devs[d->guest] = d;
    W32(ARG(2), d->guest);
    RETN(4, DI_OK);
}
M(di_EnumDevices) {
    uint32_t type = ARG(1), cb = ARG(2), ref = ARG(3);
    uint32_t buf = gmalloc(580, true);
    auto call = [&](GUIDv inst, GUIDv prod, uint32_t dt, const char *name) {
        write_instance(buf, inst, prod, dt, name);
        uint32_t args[2] = {buf, ref};
        return guest_call(cur_cpu(), cb, 2, args) != 0;
    };
    uint32_t cls = type & 0xFF;
    bool go = true;
    if (go && (cls == 0 || cls == 1 || cls == 3 || cls == 0x13)) go = call(GUID_SysKeyboard, GUID_SysKeyboard, 0x13 | (4 << 8), "Keyboard");
    if (go && (cls == 0 || cls == 1 || cls == 2 || cls == 0x12)) go = call(GUID_SysMouse, GUID_SysMouse, 0x12 | (2 << 8), "Mouse");
    if (go && (cls == 0 || cls == 4 || cls == 0x14 || cls == 0x15 || cls == 0x16 || cls == 0x17 || cls == 0x18)) {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetJoysticks(&n);
        for (int i = 0; i < n && go; i++) {
            uint32_t dt = joy_devtype(ids[i]);
            if (cls >= 0x14 && (dt & 0xFF) != cls) continue;
            const char *nm = SDL_GetJoystickNameForID(ids[i]);
            go = call(joy_instance_guid(ids[i]), joy_product_guid(ids[i]), dt, nm ? nm : "Joystick");
        }
        SDL_free(ids);
    }
    gfree(buf);
    RETN(5, DI_OK);
}
M(di_GetDeviceStatus) { RETN(2, DI_OK); }
M(di_RunControlPanel) { RETN(3, DI_OK); }
M(di_Initialize) { RETN(3, DI_OK); }
M(di_FindDevice) { RETN(4, DIERR_NOTFOUND); }
M(di_EnumDevicesBySemantics) { RETN(6, DI_OK); }
M(di_ConfigureDevices) { RETN(5, DIERR_UNSUPPORTED); }

API(dinput8, DirectInput8Create, 5) {
    if (!g_vt_di) {
        HostFn di[11] = {di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus,
                         di_RunControlPanel, di_Initialize, di_FindDevice, di_EnumDevicesBySemantics, di_ConfigureDevices};
        g_vt_di = com_make_vtable("IDirectInput8A", di, 11);
        HostFn dv[32] = {d_QueryInterface, d_AddRef, d_Release, d_GetCapabilities, d_EnumObjects, d_GetProperty, d_SetProperty,
                         d_Acquire, d_Unacquire, d_GetDeviceState, d_GetDeviceData, d_SetDataFormat, d_SetEventNotification,
                         d_SetCooperativeLevel, d_GetObjectInfo, d_GetDeviceInfo, d_RunControlPanel, d_Initialize, d_CreateEffect,
                         d_EnumEffects, d_GetEffectInfo, d_GetForceFeedbackState, d_SendForceFeedbackCommand,
                         d_EnumCreatedEffectObjects, d_Escape, d_Poll, d_SendDeviceData, d_EnumEffectsInFile, d_WriteEffectToFile,
                         d_BuildActionMap, d_SetActionMap, d_GetImageInfo};
        g_vt_dev = com_make_vtable("IDirectInputDevice8A", dv, 32);
        g_di_obj = com_new_object(g_vt_di, 4);
        ui_ensure_sdl();
        ui_extra_event_hook = di_event_hook;
    }
    W32(ARG(3), g_di_obj);
    RETN(5, DI_OK);
}
