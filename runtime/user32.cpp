// user32 / gdi32 replacement on top of SDL3: windows, message queue, keyboard and mouse.
#include <algorithm>
#include "rt.h"
#include "win32ui.h"
#include <SDL3/SDL.h>
#include <deque>
#include <map>
#include <mutex>
#include <atomic>
#include <cstring>
#include <unistd.h>

// ------------------------------------------------------------------ constants
enum : uint32_t {
    WM_CREATE = 0x01, WM_DESTROY = 0x02, WM_MOVE = 0x03, WM_SIZE = 0x05, WM_ACTIVATE = 0x06,
    WM_SETFOCUS = 0x07, WM_KILLFOCUS = 0x08, WM_PAINT = 0x0F, WM_CLOSE = 0x10, WM_QUIT = 0x12,
    WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1C, WM_SETCURSOR = 0x20, WM_NCCREATE = 0x81,
    WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105,
    WM_SYSCOMMAND = 0x112, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
    WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208,
    WM_MOUSEWHEEL = 0x20A,
};
enum : uint32_t { SC_CLOSE = 0xF060 };

// ------------------------------------------------------------------ windows
struct Win {
    SDL_Window *sdl = nullptr;
    uint32_t wndproc = 0;
    std::string cls, title;
    uint32_t style = 0, exstyle = 0;
    int x = 0, y = 0, w = 0, h = 0;
};
struct WndClass { uint32_t wndproc; uint32_t style; };

static std::map<std::string, WndClass> g_classes;      // lower-case name -> class
static std::map<uint32_t, Win> g_windows;               // hwnd -> window
static uint32_t g_next_hwnd = 0x00010010;
static uint32_t g_main_hwnd, g_focus_hwnd;
static std::recursive_mutex g_ui_mutex;

static std::string lower(const char *s) { std::string r = s ? s : ""; for (auto &ch : r) ch = (char)tolower((unsigned char)ch); return r; }

static Win *win_of(uint32_t hwnd) { auto it = g_windows.find(hwnd); return it == g_windows.end() ? nullptr : &it->second; }
static uint32_t hwnd_of(SDL_WindowID id) {
    for (auto &kv : g_windows) if (kv.second.sdl && SDL_GetWindowID(kv.second.sdl) == id) return kv.first;
    return g_main_hwnd;
}

SDL_Window *ui_sdl_window(uint32_t hwnd) {
    Win *w = win_of(hwnd ? hwnd : g_main_hwnd);
    return w ? w->sdl : nullptr;
}
uint32_t ui_main_hwnd() { return g_main_hwnd; }

static bool g_sdl_ready;
void ui_ensure_sdl() {
    if (g_sdl_ready) return;
    SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO))
        rtlog("SDL_Init failed: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    g_sdl_ready = true;
}

// call the guest window procedure synchronously (SendMessage semantics)
static uint32_t send_msg(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Win *w = win_of(hwnd);
    if (!w || !w->wndproc) return 0;
    uint32_t args[4] = {hwnd, msg, wp, lp};
    return guest_call(cur_cpu(), w->wndproc, 4, args);
}

// ------------------------------------------------------------------ message queue
struct Msg { uint32_t hwnd, msg, wp, lp, time, x, y; };
static std::deque<Msg> g_queue;
static std::mutex g_queue_mutex;
static bool g_quit_posted;
static uint32_t g_quit_code;
static int g_mouse_x, g_mouse_y;
static uint32_t g_mouse_buttons;    // MK_* bits

static uint32_t now_ms() { return (uint32_t)SDL_GetTicks(); }
static void post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    std::lock_guard<std::mutex> l(g_queue_mutex);
    g_queue.push_back({hwnd, msg, wp, lp, now_ms(), (uint32_t)g_mouse_x, (uint32_t)g_mouse_y});
}

// ------------------------------------------------------------------ keyboard
static std::atomic<uint8_t> g_key_down[256], g_key_pressed[256];

static uint32_t vk_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s == SDL_SCANCODE_0) return '0';
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return 0x61 + (s - SDL_SCANCODE_KP_1);
    switch (s) {
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PRINTSCREEN: return 0x2C;
    case SDL_SCANCODE_SCROLLLOCK: return 0x91;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_LCTRL: return 0xA2;
    case SDL_SCANCODE_RCTRL: return 0xA3;
    case SDL_SCANCODE_LSHIFT: return 0xA0;
    case SDL_SCANCODE_RSHIFT: return 0xA1;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_LGUI: return 0xA4;
    case SDL_SCANCODE_RALT: case SDL_SCANCODE_RGUI: return 0xA5;
    default: return 0;
    }
}
// generic VK for left/right modifiers
static uint32_t vk_generic(uint32_t vk) {
    switch (vk) { case 0xA0: case 0xA1: return 0x10; case 0xA2: case 0xA3: return 0x11; case 0xA4: case 0xA5: return 0x12; }
    return vk;
}
static void set_key(uint32_t vk, bool down) {
    if (!vk || vk > 255) return;
    if (down && !g_key_down[vk]) g_key_pressed[vk] = 1;
    g_key_down[vk] = down;
}

// PC/AT set-1 scancode used for lParam bits 16..23 (DirectInput uses the same numbering)
uint32_t ui_dik_of(SDL_Scancode s);

static void on_key(const SDL_KeyboardEvent &k) {
    uint32_t vk = vk_of(k.scancode);
    if (!vk) return;
    uint32_t gvk = vk_generic(vk);
    bool was = g_key_down[gvk];
    set_key(vk, k.down);
    if (gvk != vk) set_key(gvk, k.down || g_key_down[vk ^ 1]);
    uint32_t dik = ui_dik_of(k.scancode);
    uint32_t lp = 1 | ((dik & 0xFF) << 16) | ((dik & 0x100) ? 1u << 24 : 0);
    if (!k.down) lp |= 0xC0000000u; else if (was) lp |= 0x40000000u;
    bool alt = g_key_down[0x12] && gvk != 0x12;
    uint32_t msg = k.down ? (alt ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt ? WM_SYSKEYUP : WM_KEYUP);
    // Cmd+Q / Alt+F4 -> close
    if (k.down && ((k.mod & SDL_KMOD_GUI) && k.scancode == SDL_SCANCODE_Q)) { post(g_main_hwnd, WM_CLOSE, 0, 0); return; }
    if (k.down && alt && vk == 0x73) { post(g_main_hwnd, WM_CLOSE, 0, 0); return; }
    post(hwnd_of(k.windowID), msg, gvk, lp);
    // characters that SDL text input does not report
    if (k.down && (vk == 0x0D || vk == 0x08 || vk == 0x1B || vk == 0x09)) post(hwnd_of(k.windowID), WM_CHAR, vk, lp);
}

static uint32_t utf8_to_cp1252(const char *&p) {
    unsigned char c0 = (unsigned char)*p++;
    uint32_t cp = c0;
    if (c0 >= 0xC0) {
        int n = c0 >= 0xF0 ? 3 : c0 >= 0xE0 ? 2 : 1;
        cp = c0 & (0x3F >> n);
        while (n-- && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    }
    return cp < 256 ? cp : '?';
}

// ------------------------------------------------------------------ event pump
static uint32_t mk_bits() {
    uint32_t m = g_mouse_buttons;
    if (g_key_down[0x10]) m |= 4;
    if (g_key_down[0x11]) m |= 8;
    return m;
}
static uint32_t pos_lp() { return ((uint32_t)(uint16_t)g_mouse_x) | ((uint32_t)(uint16_t)g_mouse_y << 16); }

// Debug: scripted key presses, FVR_KEYS="ms:Key[:holdms],..." (ms since first pump, SDL key names).
// Lets automated runs walk menus; DirectInput sees them through ui_key_injected().
static bool g_inject[SDL_SCANCODE_COUNT];
bool ui_key_injected(SDL_Scancode s) { return s < SDL_SCANCODE_COUNT && g_inject[s]; }
// Scripted runs ignore the real keyboard and mouse, so someone using the machine can't steer them.
bool ui_scripted() { static int v = -1; if (v < 0) v = getenv("FVR_KEYS") ? 1 : 0; return v; }
static void scripted_keys() {
    struct Ev { uint64_t t; SDL_Scancode sc; bool down; };
    static std::vector<Ev> evs; static size_t next; static uint64_t t0; static bool loaded;
    if (!loaded) {
        loaded = true;
        const char *k = getenv("FVR_KEYS");
        if (!k) return;
        t0 = SDL_GetTicks();
        std::string all = k;
        size_t pos = 0;
        while (pos < all.size()) {
            size_t end = all.find(',', pos); if (end == std::string::npos) end = all.size();
            std::string item = all.substr(pos, end - pos); pos = end + 1;
            size_t c1 = item.find(':'); if (c1 == std::string::npos) continue;
            size_t c2 = item.find(':', c1 + 1);
            uint64_t t = strtoull(item.c_str(), nullptr, 10);
            std::string name = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
            uint64_t hold = c2 == std::string::npos ? 120 : strtoull(item.c_str() + c2 + 1, nullptr, 10);
            SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
            if (sc == SDL_SCANCODE_UNKNOWN) { rtlog("FVR_KEYS: unknown key %s", name.c_str()); continue; }
            evs.push_back({t, sc, true}); evs.push_back({t + hold, sc, false});
        }
        std::stable_sort(evs.begin(), evs.end(), [](const Ev &a, const Ev &b) { return a.t < b.t; });
    }
    uint64_t now = SDL_GetTicks() - t0;
    if (next < evs.size() && evs[next].t <= now) {   // one change per pump so DirectInput polls see it
        const Ev &v = evs[next++];
        g_inject[v.sc] = v.down;
        SDL_KeyboardEvent k = {};
        k.type = v.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        k.scancode = v.sc; k.key = SDL_GetKeyFromScancode(v.sc, SDL_KMOD_NONE, false); k.down = v.down;
        if (getenv("FVR_TRACE")) rtlog("FVR_KEYS: %s %s at %llu ms", SDL_GetScancodeName(v.sc), v.down ? "down" : "up", (unsigned long long)now);
        on_key(k);
    }
}

void ui_pump() {
    scripted_keys();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ui_extra_event(e);   // DirectInput: wheel, joystick hot-plug
        if (ui_scripted() && ((e.type >= SDL_EVENT_KEY_DOWN && e.type < SDL_EVENT_MOUSE_MOTION) ||
                              (e.type >= SDL_EVENT_MOUSE_MOTION && e.type < SDL_EVENT_JOYSTICK_AXIS_MOTION))) continue;
        switch (e.type) {
        case SDL_EVENT_QUIT:
            post(g_main_hwnd, WM_CLOSE, 0, 0);
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            post(hwnd_of(e.window.windowID), WM_CLOSE, 0, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: {
            uint32_t h = hwnd_of(e.window.windowID);
            g_focus_hwnd = h;
            post(h, WM_ACTIVATEAPP, 1, 0);
            post(h, WM_ACTIVATE, 1, 0);
            post(h, WM_SETFOCUS, 0, 0);
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            uint32_t h = hwnd_of(e.window.windowID);
            for (int i = 0; i < 256; i++) g_key_down[i] = 0;
            g_mouse_buttons = 0;
            post(h, WM_KILLFOCUS, 0, 0);
            post(h, WM_ACTIVATE, 0, 0);
            post(h, WM_ACTIVATEAPP, 0, 0);
            break;
        }
        case SDL_EVENT_WINDOW_EXPOSED:
            post(hwnd_of(e.window.windowID), WM_PAINT, 0, 0);
            break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
            on_key(e.key);
            break;
        case SDL_EVENT_TEXT_INPUT: {
            const char *p = e.text.text;
            while (p && *p) post(hwnd_of(e.text.windowID), WM_CHAR, utf8_to_cp1252(p), 1);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            g_mouse_x = (int)e.motion.x; g_mouse_y = (int)e.motion.y;
            post(hwnd_of(e.motion.windowID), WM_MOUSEMOVE, mk_bits(), pos_lp());
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            bool d = e.button.down;
            g_mouse_x = (int)e.button.x; g_mouse_y = (int)e.button.y;
            uint32_t bit = 0, msg = 0, vk = 0;
            switch (e.button.button) {
            case SDL_BUTTON_LEFT:   bit = 1;    vk = 1; msg = d ? WM_LBUTTONDOWN : WM_LBUTTONUP; break;
            case SDL_BUTTON_RIGHT:  bit = 2;    vk = 2; msg = d ? WM_RBUTTONDOWN : WM_RBUTTONUP; break;
            case SDL_BUTTON_MIDDLE: bit = 0x10; vk = 4; msg = d ? WM_MBUTTONDOWN : WM_MBUTTONUP; break;
            default: break;
            }
            if (!msg) break;
            if (d) g_mouse_buttons |= bit; else g_mouse_buttons &= ~bit;
            set_key(vk, d);
            post(hwnd_of(e.button.windowID), msg, mk_bits(), pos_lp());
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            int delta = (int)(e.wheel.y * 120);
            post(hwnd_of(e.wheel.windowID), WM_MOUSEWHEEL, mk_bits() | ((uint32_t)(uint16_t)delta << 16), pos_lp());
            break;
        }
        default:
            break;
        }
    }
}

// ------------------------------------------------------------------ user32: windows
API(user32, EnumWindows, 2) { RETN(2, 1); }      // our process owns no other top-level windows

API(user32, RegisterClassA, 1) {
    uint32_t wc = ARG(0);
    uint32_t name = R32(wc + 36);
    std::string n = name < 0x10000 ? "#atom" + std::to_string(name) : lower(GS(name));
    g_classes[n] = {R32(wc + 4), R32(wc)};
    static uint16_t atom = 0xC000;
    RETN(1, atom++);
}

API(user32, CreateWindowExA, 12) {
    std::lock_guard<std::recursive_mutex> l(g_ui_mutex);
    uint32_t exstyle = ARG(0), cls = ARG(1), title = ARG(2), style = ARG(3);
    int x = (int)ARG(4), y = (int)ARG(5), w = (int)ARG(6), h = (int)ARG(7);
    uint32_t param = ARG(11);
    std::string cn = cls < 0x10000 ? "#atom" + std::to_string(cls) : lower(GS(cls));
    auto it = g_classes.find(cn);
    if (it == g_classes.end()) { rtlog("CreateWindowExA: unknown class %s", cn.c_str()); set_last_error(1407); RETN(12, 0); }
    ui_ensure_sdl();
    if (w <= 0 || (uint32_t)w == 0x80000000u) w = 800;
    if (h <= 0 || (uint32_t)h == 0x80000000u) h = 600;
    const char *t = GS(title) ? GS(title) : "";
    SDL_Window *sw = SDL_CreateWindow(t, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!sw) { rtlog("SDL_CreateWindow failed: %s", SDL_GetError()); RETN(12, 0); }
    uint32_t hwnd = g_next_hwnd; g_next_hwnd += 4;
    Win &wi = g_windows[hwnd];
    wi.sdl = sw; wi.wndproc = it->second.wndproc; wi.cls = cn; wi.title = t;
    wi.style = style; wi.exstyle = exstyle; wi.x = x; wi.y = y; wi.w = w; wi.h = h;
    if (!g_main_hwnd) g_main_hwnd = hwnd;
    SDL_StartTextInput(sw);
    rtlog("CreateWindowExA '%s' %dx%d style %08x -> %08x", t, w, h, style, hwnd);

    // CREATESTRUCTA on the guest stack area (heap is fine)
    uint32_t cs = gmalloc(48, true);
    W32(cs + 0, param); W32(cs + 4, 0x400000); W32(cs + 8, ARG(9)); W32(cs + 12, ARG(8));
    W32(cs + 16, h); W32(cs + 20, w); W32(cs + 24, y); W32(cs + 28, x);
    W32(cs + 32, style); W32(cs + 36, title); W32(cs + 40, cls); W32(cs + 44, exstyle);
    send_msg(hwnd, WM_NCCREATE, 0, cs);
    uint32_t r = send_msg(hwnd, WM_CREATE, 0, cs);
    gfree(cs);
    if ((int32_t)r == -1) { SDL_DestroyWindow(sw); g_windows.erase(hwnd); RETN(12, 0); }
    if (style & 0x10000000) {   // WS_VISIBLE
        SDL_ShowWindow(sw);
        send_msg(hwnd, WM_SIZE, 0, (uint32_t)w | ((uint32_t)h << 16));
    }
    RETN(12, hwnd);
}

API(user32, DestroyWindow, 1) {
    uint32_t hwnd = ARG(0);
    Win *w = win_of(hwnd);
    if (!w) RETN(1, 0);
    send_msg(hwnd, WM_DESTROY, 0, 0);
    w = win_of(hwnd);
    if (w && w->sdl) SDL_DestroyWindow(w->sdl);
    g_windows.erase(hwnd);
    if (hwnd == g_main_hwnd) g_main_hwnd = 0;
    RETN(1, 1);
}

API(user32, DefWindowProcA, 4) {
    uint32_t hwnd = ARG(0), msg = ARG(1), wp = ARG(2);
    switch (msg) {
    case WM_NCCREATE: RETN(4, 1);
    case WM_CLOSE: { uint32_t a[1] = {hwnd}; (void)a; send_msg(hwnd, WM_DESTROY, 0, 0); rtlog("window closed"); fflush(nullptr); _exit(0); }
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) post(hwnd, WM_CLOSE, 0, 0);
        RETN(4, 0);
    default: RETN(4, 0);
    }
}

API(user32, ShowWindow, 2) {
    Win *w = win_of(ARG(0));
    if (!w) RETN(2, 0);
    uint32_t cmd = ARG(1);
    bool was = !(SDL_GetWindowFlags(w->sdl) & SDL_WINDOW_HIDDEN);
    if (cmd == 0) SDL_HideWindow(w->sdl);
    else {
        SDL_ShowWindow(w->sdl);
        if (cmd == 6 || cmd == 2 || cmd == 7) SDL_MinimizeWindow(w->sdl);
        else if (cmd == 3) SDL_MaximizeWindow(w->sdl);
        else if (cmd == 9) SDL_RestoreWindow(w->sdl);
        if (!was) send_msg(ARG(0), WM_SIZE, 0, (uint32_t)w->w | ((uint32_t)w->h << 16));
    }
    RETN(2, was);
}
API(user32, UpdateWindow, 1) { RETN(1, 1); }
API(user32, BringWindowToTop, 1) { Win *w = win_of(ARG(0)); if (w) SDL_RaiseWindow(w->sdl); RETN(1, 1); }
API(user32, SetForegroundWindow, 1) { Win *w = win_of(ARG(0)); if (w) SDL_RaiseWindow(w->sdl); RETN(1, 1); }
API(user32, SetActiveWindow, 1) { uint32_t p = g_focus_hwnd; g_focus_hwnd = ARG(0); RETN(1, p); }
API(user32, SetFocus, 1) { uint32_t p = g_focus_hwnd; g_focus_hwnd = ARG(0); RETN(1, p); }
API(user32, GetFocus, 0) { RETN(0, g_focus_hwnd); }
API(user32, GetForegroundWindow, 0) { RETN(0, g_main_hwnd); }
API(user32, GetActiveWindow, 0) { RETN(0, g_main_hwnd); }
API(user32, IsWindow, 1) { RETN(1, win_of(ARG(0)) ? 1 : 0); }
API(user32, IsIconic, 1) { Win *w = win_of(ARG(0)); RETN(1, w && (SDL_GetWindowFlags(w->sdl) & SDL_WINDOW_MINIMIZED) ? 1 : 0); }
API(user32, FindWindowA, 2) { RETN(2, 0); }

API(user32, GetClientRect, 2) {
    Win *w = win_of(ARG(0));
    int cw = 0, ch = 0;
    if (w) SDL_GetWindowSize(w->sdl, &cw, &ch);
    W32(ARG(1), 0); W32(ARG(1) + 4, 0); W32(ARG(1) + 8, cw); W32(ARG(1) + 12, ch);
    RETN(2, w ? 1 : 0);
}
API(user32, GetWindowRect, 2) {
    Win *w = win_of(ARG(0));
    int x = 0, y = 0, cw = 0, ch = 0;
    if (w) { SDL_GetWindowPosition(w->sdl, &x, &y); SDL_GetWindowSize(w->sdl, &cw, &ch); }
    W32(ARG(1), x); W32(ARG(1) + 4, y); W32(ARG(1) + 8, x + cw); W32(ARG(1) + 12, y + ch);
    RETN(2, w ? 1 : 0);
}
// SDL windows have no Win32 frame: the client area equals the window size.
API(user32, AdjustWindowRectEx, 4) { RETN(4, 1); }
API(user32, AdjustWindowRect, 3) { RETN(3, 1); }
API(user32, ClientToScreen, 2) {
    Win *w = win_of(ARG(0));
    int x = 0, y = 0;
    if (w) SDL_GetWindowPosition(w->sdl, &x, &y);
    W32(ARG(1), R32(ARG(1)) + x); W32(ARG(1) + 4, R32(ARG(1) + 4) + y);
    RETN(2, 1);
}
API(user32, ScreenToClient, 2) {
    Win *w = win_of(ARG(0));
    int x = 0, y = 0;
    if (w) SDL_GetWindowPosition(w->sdl, &x, &y);
    W32(ARG(1), R32(ARG(1)) - x); W32(ARG(1) + 4, R32(ARG(1) + 4) - y);
    RETN(2, 1);
}
API(user32, SetWindowPos, 7) {
    Win *w = win_of(ARG(0));
    uint32_t fl = ARG(6);
    if (w) {
        if (!(fl & 2)) SDL_SetWindowPosition(w->sdl, (int)ARG(2), (int)ARG(3));
        if (!(fl & 1) && (int)ARG(4) > 0 && (int)ARG(5) > 0) { SDL_SetWindowSize(w->sdl, (int)ARG(4), (int)ARG(5)); w->w = ARG(4); w->h = ARG(5); }
    }
    RETN(7, 1);
}
API(user32, MoveWindow, 6) {
    Win *w = win_of(ARG(0));
    if (w) {
        SDL_SetWindowPosition(w->sdl, (int)ARG(1), (int)ARG(2));
        if ((int)ARG(3) > 0 && (int)ARG(4) > 0) { SDL_SetWindowSize(w->sdl, (int)ARG(3), (int)ARG(4)); w->w = ARG(3); w->h = ARG(4); }
    }
    RETN(6, 1);
}
API(user32, GetWindowLongA, 2) {
    Win *w = win_of(ARG(0));
    int idx = (int)ARG(1);
    uint32_t v = 0;
    if (w) v = idx == -16 ? w->style : idx == -20 ? w->exstyle : idx == -4 ? w->wndproc : 0;
    RETN(2, v);
}
API(user32, SetWindowLongA, 3) {
    Win *w = win_of(ARG(0));
    int idx = (int)ARG(1);
    uint32_t old = 0;
    if (w) {
        if (idx == -16) { old = w->style; w->style = ARG(2); }
        else if (idx == -20) { old = w->exstyle; w->exstyle = ARG(2); }
        else if (idx == -4) { old = w->wndproc; w->wndproc = ARG(2); }
    }
    RETN(3, old);
}
API(user32, GetClassNameA, 3) {
    Win *w = win_of(ARG(0));
    if (!w || !ARG(2)) RETN(3, 0);
    gstrcpy(ARG(1), w->cls.c_str(), ARG(2));
    RETN(3, (uint32_t)std::min<size_t>(w->cls.size(), ARG(2) - 1));
}
API(user32, GetWindowTextA, 3) {
    Win *w = win_of(ARG(0));
    if (!w || !ARG(2)) RETN(3, 0);
    gstrcpy(ARG(1), w->title.c_str(), ARG(2));
    RETN(3, (uint32_t)std::min<size_t>(w->title.size(), ARG(2) - 1));
}
API(user32, SetWindowTextA, 2) {
    Win *w = win_of(ARG(0));
    if (w) { w->title = GS(ARG(1)) ? GS(ARG(1)) : ""; SDL_SetWindowTitle(w->sdl, w->title.c_str()); }
    RETN(2, 1);
}

// ------------------------------------------------------------------ user32: messages
static bool take(uint32_t buf, bool remove) {
    std::lock_guard<std::mutex> l(g_queue_mutex);
    if (g_queue.empty()) return false;
    Msg m = g_queue.front();
    if (remove) g_queue.pop_front();
    W32(buf, m.hwnd); W32(buf + 4, m.msg); W32(buf + 8, m.wp); W32(buf + 12, m.lp);
    W32(buf + 16, m.time); W32(buf + 20, m.x); W32(buf + 24, m.y);
    return true;
}

API(user32, PeekMessageA, 5) {
    uint32_t buf = ARG(0);
    bool remove = ARG(4) & 1;
    if (g_sdl_ready) ui_pump();
    if (take(buf, remove)) RETN(5, 1);
    if (g_quit_posted) {
        W32(buf, 0); W32(buf + 4, WM_QUIT); W32(buf + 8, g_quit_code); W32(buf + 12, 0);
        if (remove) g_quit_posted = false;
        RETN(5, 1);
    }
    RETN(5, 0);
}
API(user32, GetMessageA, 4) {
    uint32_t buf = ARG(0);
    for (;;) {
        if (g_sdl_ready) ui_pump();
        if (take(buf, true)) RETN(4, R32(buf + 4) == WM_QUIT ? 0 : 1);
        if (g_quit_posted) { g_quit_posted = false; W32(buf + 4, WM_QUIT); W32(buf + 8, g_quit_code); RETN(4, 0); }
        if (g_sdl_ready) SDL_WaitEventTimeout(nullptr, 10); else SDL_Delay(10);
    }
}
API(user32, TranslateMessage, 1) { RETN(1, 0); }   // WM_CHAR is produced from SDL text input
API(user32, DispatchMessageA, 1) {
    uint32_t m = ARG(0);
    if (g_trace_api) rtlog("  msg %04x wp %08x lp %08x", R32(m + 4), R32(m + 8), R32(m + 12));
    uint32_t r = send_msg(R32(m), R32(m + 4), R32(m + 8), R32(m + 12));
    RETN(1, r);
}
API(user32, PostMessageA, 4) { post(ARG(0), ARG(1), ARG(2), ARG(3)); RETN(4, 1); }
API(user32, SendMessageA, 4) { RETN(4, send_msg(ARG(0), ARG(1), ARG(2), ARG(3))); }
API(user32, PostQuitMessage, 1) { g_quit_posted = true; g_quit_code = ARG(0); RETN(1, 0); }
API(user32, WaitForInputIdle, 2) { RETN(2, 0); }

// ------------------------------------------------------------------ user32: input
API(user32, GetAsyncKeyState, 1) {
    uint32_t vk = ARG(0) & 0xFF;
    if (g_sdl_ready) SDL_PumpEvents();
    uint32_t r = (g_key_down[vk] ? 0x8000 : 0) | (g_key_pressed[vk].exchange(0) ? 1 : 0);
    RETN(1, r);
}
API(user32, GetKeyState, 1) {
    uint32_t vk = ARG(0) & 0xFF;
    RETN(1, g_key_down[vk] ? 0xFFFF8000u : 0);
}
API(user32, GetCursorPos, 1) {
    float x = 0, y = 0;
    if (g_sdl_ready && !ui_scripted()) SDL_GetGlobalMouseState(&x, &y);
    W32(ARG(0), (int)x); W32(ARG(0) + 4, (int)y);
    RETN(1, 1);
}
API(user32, SetCursorPos, 2) {
    Win *w = win_of(g_main_hwnd);
    if (w) {
        int wx = 0, wy = 0;
        SDL_GetWindowPosition(w->sdl, &wx, &wy);
        SDL_WarpMouseInWindow(w->sdl, (float)((int)ARG(0) - wx), (float)((int)ARG(1) - wy));
    }
    RETN(2, 1);
}
static int g_cursor_count = 0;
API(user32, ShowCursor, 1) {
    g_cursor_count += ARG(0) ? 1 : -1;
    if (g_sdl_ready) { if (g_cursor_count >= 0) SDL_ShowCursor(); else SDL_HideCursor(); }
    RETN(1, (uint32_t)g_cursor_count);
}
static uint32_t g_cur_cursor;
API(user32, SetCursor, 1) { uint32_t p = g_cur_cursor; g_cur_cursor = ARG(0); RETN(1, p); }
API(user32, LoadCursorA, 2) { RETN(2, 0x00C00000 | (ARG(1) & 0xFFFF)); }
API(user32, LoadIconA, 2) { RETN(2, 0x00C10000 | (ARG(1) & 0xFFFF)); }
API(user32, DestroyIcon, 1) { RETN(1, 1); }

API(user32, GetSystemMetrics, 1) {
    int i = (int)ARG(0);
    ui_ensure_sdl();
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    int sw = dm ? dm->w : 1920, sh = dm ? dm->h : 1080;
    switch (i) {
    case 0: case 16: case 78: RETN(1, sw);   // SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN
    case 1: case 17: case 79: RETN(1, sh);
    case 4: RETN(1, 0);                      // SM_CYCAPTION
    case 5: case 6: case 7: case 8: case 32: case 33: RETN(1, 0); // borders / frames
    case 11: case 12: RETN(1, 32);           // SM_CXICON
    case 13: case 14: RETN(1, 32);           // SM_CXCURSOR
    case 19: RETN(1, 1);                     // SM_MOUSEPRESENT
    case 43: RETN(1, 3);                     // SM_CMOUSEBUTTONS
    case 80: RETN(1, 1);                     // SM_CMONITORS
    default: RETN(1, 0);
    }
}

API(user32, MessageBoxA, 4) {
    const char *text = GS(ARG(1)) ? GS(ARG(1)) : "";
    const char *cap = GS(ARG(2)) ? GS(ARG(2)) : "";
    uint32_t type = ARG(3);
    rtlog("MessageBox [%s] %s", cap, text);
    ui_ensure_sdl();
    SDL_MessageBoxButtonData b[3];
    int nb = 0;
    switch (type & 0xF) {
    case 1: b[nb++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "OK"}; b[nb++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"}; break;
    case 3: b[nb++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 6, "Yes"}; b[nb++] = {0, 7, "No"}; b[nb++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"}; break;
    case 4: b[nb++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 6, "Yes"}; b[nb++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 7, "No"}; break;
    default: b[nb++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "OK"}; break;
    }
    uint32_t icon = type & 0xF0;
    SDL_MessageBoxData md{};
    md.flags = icon == 0x10 ? SDL_MESSAGEBOX_ERROR : icon == 0x30 ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION;
    md.window = ui_sdl_window(ARG(0));
    md.title = cap; md.message = text; md.numbuttons = nb; md.buttons = b;
    int id = b[0].buttonID;
    if (!SDL_ShowMessageBox(&md, &id)) rtlog("SDL_ShowMessageBox: %s", SDL_GetError());
    RETN(4, (uint32_t)id);
}

// ------------------------------------------------------------------ user32/gdi32: painting
// The game draws everything through Direct3D; GDI is only used for trivial window painting.
API(user32, BeginPaint, 2) {
    uint32_t ps = ARG(1);
    memset(H(ps), 0, 64);
    W32(ps, 0x00D00001);
    Win *w = win_of(ARG(0));
    if (w) { int cw, ch; SDL_GetWindowSize(w->sdl, &cw, &ch); W32(ps + 16, cw); W32(ps + 20, ch); }
    RETN(2, 0x00D00001);
}
API(user32, EndPaint, 2) { RETN(2, 1); }
API(user32, GetDC, 1) { RETN(1, 0x00D00001); }
API(user32, ReleaseDC, 2) { RETN(2, 1); }
API(user32, FillRect, 3) { RETN(3, 1); }
API(user32, InvalidateRect, 3) { RETN(3, 1); }
API(user32, ValidateRect, 2) { RETN(2, 1); }

static uint32_t g_next_gdi = 0x00E00010;
API(gdi32, CreateCompatibleDC, 1) { RETN(1, g_next_gdi += 4); }
API(gdi32, CreateDCA, 4) { RETN(4, g_next_gdi += 4); }
API(gdi32, DeleteDC, 1) { RETN(1, 1); }
API(gdi32, CreateSolidBrush, 1) { RETN(1, g_next_gdi += 4); }
API(gdi32, GetStockObject, 1) { RETN(1, 0x00E00000 | ARG(0)); }
API(gdi32, SelectObject, 2) { RETN(2, 0x00E00001); }
API(gdi32, DeleteObject, 1) { RETN(1, 1); }
API(gdi32, BitBlt, 9) { RETN(9, 1); }
API(gdi32, GetDeviceCaps, 2) {
    int i = (int)ARG(1);
    ui_ensure_sdl();
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    switch (i) {
    case 8: RETN(2, dm ? dm->w : 1920);   // HORZRES
    case 10: RETN(2, dm ? dm->h : 1080);  // VERTRES
    case 12: RETN(2, 32);                 // BITSPIXEL
    case 14: RETN(2, 1);                  // PLANES
    case 88: case 90: RETN(2, 96);        // LOGPIXELSX/Y
    case 116: RETN(2, dm && dm->refresh_rate > 0 ? (uint32_t)dm->refresh_rate : 60); // VREFRESH
    default: RETN(2, 0);
    }
}
API(gdi32, CreateDIBSection, 6) {
    uint32_t bmi = ARG(1);
    int w = (int)R32(bmi + 4), h = (int)R32(bmi + 8);
    int bpp = R16(bmi + 14);
    if (h < 0) h = -h;
    uint32_t stride = ((uint32_t)(w * bpp + 31) / 32) * 4;
    uint32_t bits = gmalloc(stride * (uint32_t)h + 16, true);
    if (ARG(3)) W32(ARG(3), bits);
    RETN(6, g_next_gdi += 4);
}

void (*ui_extra_event_hook)(const SDL_Event &e);

// ------------------------------------------------------------------ DIK codes
uint32_t ui_dik_of(SDL_Scancode s) {
    static const struct { SDL_Scancode s; uint16_t d; } t[] = {
        {SDL_SCANCODE_ESCAPE,0x01},{SDL_SCANCODE_1,0x02},{SDL_SCANCODE_2,0x03},{SDL_SCANCODE_3,0x04},{SDL_SCANCODE_4,0x05},
        {SDL_SCANCODE_5,0x06},{SDL_SCANCODE_6,0x07},{SDL_SCANCODE_7,0x08},{SDL_SCANCODE_8,0x09},{SDL_SCANCODE_9,0x0A},
        {SDL_SCANCODE_0,0x0B},{SDL_SCANCODE_MINUS,0x0C},{SDL_SCANCODE_EQUALS,0x0D},{SDL_SCANCODE_BACKSPACE,0x0E},{SDL_SCANCODE_TAB,0x0F},
        {SDL_SCANCODE_Q,0x10},{SDL_SCANCODE_W,0x11},{SDL_SCANCODE_E,0x12},{SDL_SCANCODE_R,0x13},{SDL_SCANCODE_T,0x14},
        {SDL_SCANCODE_Y,0x15},{SDL_SCANCODE_U,0x16},{SDL_SCANCODE_I,0x17},{SDL_SCANCODE_O,0x18},{SDL_SCANCODE_P,0x19},
        {SDL_SCANCODE_LEFTBRACKET,0x1A},{SDL_SCANCODE_RIGHTBRACKET,0x1B},{SDL_SCANCODE_RETURN,0x1C},{SDL_SCANCODE_LCTRL,0x1D},
        {SDL_SCANCODE_A,0x1E},{SDL_SCANCODE_S,0x1F},{SDL_SCANCODE_D,0x20},{SDL_SCANCODE_F,0x21},{SDL_SCANCODE_G,0x22},
        {SDL_SCANCODE_H,0x23},{SDL_SCANCODE_J,0x24},{SDL_SCANCODE_K,0x25},{SDL_SCANCODE_L,0x26},{SDL_SCANCODE_SEMICOLON,0x27},
        {SDL_SCANCODE_APOSTROPHE,0x28},{SDL_SCANCODE_GRAVE,0x29},{SDL_SCANCODE_LSHIFT,0x2A},{SDL_SCANCODE_BACKSLASH,0x2B},
        {SDL_SCANCODE_Z,0x2C},{SDL_SCANCODE_X,0x2D},{SDL_SCANCODE_C,0x2E},{SDL_SCANCODE_V,0x2F},{SDL_SCANCODE_B,0x30},
        {SDL_SCANCODE_N,0x31},{SDL_SCANCODE_M,0x32},{SDL_SCANCODE_COMMA,0x33},{SDL_SCANCODE_PERIOD,0x34},{SDL_SCANCODE_SLASH,0x35},
        {SDL_SCANCODE_RSHIFT,0x36},{SDL_SCANCODE_KP_MULTIPLY,0x37},{SDL_SCANCODE_LALT,0x38},{SDL_SCANCODE_SPACE,0x39},
        {SDL_SCANCODE_CAPSLOCK,0x3A},{SDL_SCANCODE_F1,0x3B},{SDL_SCANCODE_F2,0x3C},{SDL_SCANCODE_F3,0x3D},{SDL_SCANCODE_F4,0x3E},
        {SDL_SCANCODE_F5,0x3F},{SDL_SCANCODE_F6,0x40},{SDL_SCANCODE_F7,0x41},{SDL_SCANCODE_F8,0x42},{SDL_SCANCODE_F9,0x43},
        {SDL_SCANCODE_F10,0x44},{SDL_SCANCODE_NUMLOCKCLEAR,0x45},{SDL_SCANCODE_SCROLLLOCK,0x46},{SDL_SCANCODE_KP_7,0x47},
        {SDL_SCANCODE_KP_8,0x48},{SDL_SCANCODE_KP_9,0x49},{SDL_SCANCODE_KP_MINUS,0x4A},{SDL_SCANCODE_KP_4,0x4B},{SDL_SCANCODE_KP_5,0x4C},
        {SDL_SCANCODE_KP_6,0x4D},{SDL_SCANCODE_KP_PLUS,0x4E},{SDL_SCANCODE_KP_1,0x4F},{SDL_SCANCODE_KP_2,0x50},{SDL_SCANCODE_KP_3,0x51},
        {SDL_SCANCODE_KP_0,0x52},{SDL_SCANCODE_KP_PERIOD,0x53},{SDL_SCANCODE_F11,0x57},{SDL_SCANCODE_F12,0x58},
        {SDL_SCANCODE_KP_ENTER,0x9C},{SDL_SCANCODE_RCTRL,0x9D},{SDL_SCANCODE_KP_DIVIDE,0xB5},{SDL_SCANCODE_PRINTSCREEN,0xB7},
        {SDL_SCANCODE_RALT,0xB8},{SDL_SCANCODE_PAUSE,0xC5},{SDL_SCANCODE_HOME,0xC7},{SDL_SCANCODE_UP,0xC8},{SDL_SCANCODE_PAGEUP,0xC9},
        {SDL_SCANCODE_LEFT,0xCB},{SDL_SCANCODE_RIGHT,0xCD},{SDL_SCANCODE_END,0xCF},{SDL_SCANCODE_DOWN,0xD0},{SDL_SCANCODE_PAGEDOWN,0xD1},
        {SDL_SCANCODE_INSERT,0xD2},{SDL_SCANCODE_DELETE,0xD3},{SDL_SCANCODE_LGUI,0xDB},{SDL_SCANCODE_RGUI,0xDC},
    };
    for (auto &e : t) if (e.s == s) return e.d;
    return 0;
}
