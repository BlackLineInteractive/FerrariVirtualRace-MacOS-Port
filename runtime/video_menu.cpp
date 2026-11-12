// The port's own entries in the game's "Video settings" menu.
//
// The menu (FerrariVR_Hi.exe 0x501300) keeps its entries in a std::map<int, Entry> ordered by id
// and draws them in that order. An Entry is { std::string name; std::vector<std::string> values;
// int current; } (0x30 bytes, MSVC 2008 layout: values at +0x1c, current at +0x2c). The game adds
// ids 0..10 (Display mode .. Shadow detail level) with map[id] (0x503c50), std::string::assign
// (0x46d4f0) and vector::push_back (0x498100), then clamps every entry's current value.
// fvr_hook_video_items runs right after the last entry is added and adds ours with the same
// functions; fvr_hook_video_apply runs while the menu writes its choices to Config.ini and stores
// ours in options.txt (fvr_option_set), where they take effect at once.
#include "rt.h"
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

float fvr_option(const char *name, float def);
void fvr_option_set(const char *name, float value);

namespace {

const uint32_t kMapIndex = 0x503c50;     // thiscall(ecx = &key) map::operator[](map *): Entry *
const uint32_t kAssign = 0x46d4f0;       // thiscall(ecx = string) assign(const string *, pos, count)
const uint32_t kPushBack = 0x498100;     // vector<string>::push_back(const string *), esi = vector

struct Item {
    int id;
    const char *name;
    const char *option;
    std::vector<const char *> labels;
    std::vector<float> values;           // option value of each label
};
const std::vector<Item> &items() {
    static const std::vector<Item> v = {
        {11, "Ray tracing", "rt", {"off", "on"}, {0, 1}},
        {12, "PBR materials", "pbr", {"off", "on"}, {0, 1}},
        {13, "Ambient occlusion", "ao", {"off", "low", "medium", "high"}, {0, 0.4f, 0.7f, 1.0f}},
        {14, "Screen reflections", "ssr", {"off", "low", "medium", "high"}, {0, 0.4f, 0.7f, 1.0f}},
    };
    return v;
}

// A temporary std::string in guest memory holding s (freed with free_string).
uint32_t make_string(const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    uint32_t o = gmalloc(0x1c, true);
    if (len <= 15) {
        memcpy(H(o + 4), s, len + 1);
        W32(o + 0x18, 15);
    } else {
        uint32_t b = gmalloc(len + 1, true);
        memcpy(H(b), s, len + 1);
        W32(o + 4, b);
        W32(o + 0x18, len);
    }
    W32(o + 0x14, len);
    return o;
}
void free_string(uint32_t o) {
    if (R32(o + 0x18) > 15) gfree(R32(o + 4));
    gfree(o);
}

struct Regs { uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp; };
Regs save(CPU *c) { return {c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->esp}; }
void restore(CPU *c, const Regs &r) {
    c->eax = r.eax; c->ebx = r.ebx; c->ecx = r.ecx; c->edx = r.edx; c->esi = r.esi; c->edi = r.edi; c->ebp = r.ebp; c->esp = r.esp;
}

uint32_t entry(CPU *c, uint32_t map, int id) {
    static uint32_t key;
    if (!key) key = gmalloc(4, true);
    W32(key, (uint32_t)id);
    c->ecx = key;
    uint32_t a[1] = {map};
    return guest_call(c, kMapIndex, 1, a);
}

int current_index(const Item &it) {
    float v = fvr_option(it.option, 0);
    int best = 0;
    for (size_t i = 1; i < it.values.size(); i++)
        if (fabsf(it.values[i] - v) < fabsf(it.values[best] - v)) best = (int)i;
    return best;
}

}   // namespace

// 0x50279a, replacing "mov eax, [ebp + 4]" (ebp = the entries map).
extern "C" void fvr_hook_video_items(CPU *c) {
    Regs r = save(c);
    uint32_t map = c->ebp;
    for (const Item &it : items()) {
        uint32_t e = entry(c, map, it.id);
        uint32_t name = make_string(it.name);
        c->ecx = e;
        uint32_t a[3] = {name, 0, 0xFFFFFFFFu};
        guest_call(c, kAssign, 3, a);
        free_string(name);
        // the menu object lives on, so the entry may already have its values from an earlier visit
        uint32_t first = R32(e + 0x20), last = R32(e + 0x24);
        if (first == last)
            for (const char *l : it.labels) {
                uint32_t s = make_string(l);
                c->esi = e + 0x1c;
                uint32_t b[1] = {s};
                guest_call(c, kPushBack, 1, b);
                free_string(s);
            }
        W32(e + 0x2c, (uint32_t)current_index(it));
    }
    restore(c, r);
    c->eax = R32(c->ebp + 4);
}

// 0x502d41, replacing "push esi" (esi = the entries map) while the menu saves its choices.
extern "C" void fvr_hook_video_apply(CPU *c) {
    Regs r = save(c);
    uint32_t map = c->esi;
    for (const Item &it : items()) {
        uint32_t e = entry(c, map, it.id);
        int cur = (int)R32(e + 0x2c);
        if (cur < 0 || cur >= (int)it.values.size()) continue;
        if (current_index(it) != cur || fvr_option(it.option, -1) < 0) {
            fvr_option_set(it.option, it.values[cur]);
            rtlog("menu: %s = %g", it.option, it.values[cur]);
        }
    }
    restore(c, r);
    c->esp -= 4;
    W32(c->esp, c->esi);
}

// 0x50378c, replacing "cmp eax, 0xa": when an entry is selected, the menu shows its values only
// for ids up to 10; ours go up to 14.
extern "C" void fvr_hook_video_limit(CPU *c) {
    uint32_t a = c->eax, b = 14;
    c->zf = a == b;
    c->cf = a < b;
}

// 0x503926, replacing "cmp ebx, 0xa": the same limit when a value is chosen.
extern "C" void fvr_hook_video_limit2(CPU *c) {
    uint32_t a = c->ebx, b = 14;
    c->zf = a == b;
    c->cf = a < b;
}

// The game spaces its 11 entries 32 x 1.2 units apart from y = 182 (screen y ~ 98 + 1.56 x units at
// 1080 lines), the last one at about 500, just above the red band. With more entries the list
// starts right below the title (y ~ 117) and the step and the font shrink so that the last entry
// stays where Shadow detail level used to.
static float list_step_scale(uint32_t n) {
    if (n <= 11) return 1.0f;
    float k = (500.0f - 117.0f) / ((float)(n - 1) * 38.4f);
    return k < 1.0f ? k : 1.0f;
}
static float g_font_scale = 1.0f;   // for the next list the menu builds (fvr_hook_list_font)
// 0x500d62, replacing "movss xmm0, [0x64d7e8]" (32.0, each entry's height); edi = the menu.
extern "C" void fvr_hook_video_step(CPU *c) {
    XMM *d = &c->xmm[0];
    d->q[0] = d->q[1] = 0;
    d->f[0] = 32.0f * list_step_scale(R32(c->edi + 0x2a8));
}
// 0x500d92, replacing "movss xmm0, [0x620a28]" (2.0, added to the list's top y of 180).
extern "C" void fvr_hook_video_top(CPU *c) {
    XMM *d = &c->xmm[0];
    d->q[0] = d->q[1] = 0;
    uint32_t n = R32(c->edi + 0x2a8);
    d->f[0] = n > 11 ? 2.0f - 65.0f : 2.0f;
    g_font_scale = list_step_scale(n);
}
// 0x51f701 (the menus' list builder), replacing "movss xmm1, [0x620a04]" (42.0, the text size):
// smaller text for the Video settings list when it has more entries; 1 for every other list.
extern "C" void fvr_hook_list_font(CPU *c) {
    XMM *d = &c->xmm[1];
    d->q[0] = d->q[1] = 0;
    d->f[0] = 42.0f * g_font_scale;
    g_font_scale = 1.0f;
}
