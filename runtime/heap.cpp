// Guest heap: region allocator (VirtualAlloc) + size-class allocator (HeapAlloc) inside guest memory.
#include "rt.h"
#include <map>
#include <sys/mman.h>
#include <mutex>
#include <vector>
#include <cstring>

static std::mutex g_vm_mutex;
static std::map<uint32_t, uint32_t> g_regions;   // start -> size (64K-aligned)

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

uint32_t vm_alloc(uint32_t addr, uint32_t size) {
    std::lock_guard<std::mutex> l(g_vm_mutex);
    size = align_up(size ? size : 1, 0x10000);
    if (addr) {
        addr &= ~0xFFFFu;
        // already inside an existing region? (commit of reserved memory)
        auto it = g_regions.upper_bound(addr);
        if (it != g_regions.begin()) {
            --it;
            if (addr >= it->first && addr + size <= it->first + it->second) return addr;
        }
        // free gap?
        auto nx = g_regions.lower_bound(addr);
        bool ok = addr >= GUEST_HEAP_START && addr + size <= GUEST_HEAP_END;
        if (ok && nx != g_regions.end() && nx->first < addr + size) ok = false;
        if (ok && nx != g_regions.begin()) {
            auto pv = std::prev(nx);
            if (pv->first + pv->second > addr) ok = false;
        }
        if (!ok) return 0;
        g_regions[addr] = size;
        memset(H(addr), 0, size);
        return addr;
    }
    uint32_t cur = GUEST_HEAP_START;
    for (auto &r : g_regions) {
        if (r.first >= cur + size) break;
        cur = std::max(cur, r.first + r.second);
    }
    if (cur + size > GUEST_HEAP_END || cur + size < cur) { rtlog("vm_alloc: out of guest memory (%u)", size); return 0; }
    g_regions[cur] = size;
    memset(H(cur), 0, size);
    return cur;
}

void vm_free(uint32_t addr) {
    std::lock_guard<std::mutex> l(g_vm_mutex);
    auto it = g_regions.find(addr);
    if (it != g_regions.end()) {
        madvise(H(it->first), it->second, MADV_FREE);
        g_regions.erase(it);
    }
}

// ------------------------------------------------------------ size-class allocator
// Block layout: [u32 size][u32 magic][8 pad] payload (16-byte aligned).
static constexpr uint32_t HDR = 16, MAGIC = 0xA110CA7E, BIG = 1u << 20;
static std::mutex g_heap_mutex;
static std::vector<uint32_t> g_free[64];
static uint32_t g_arena_cur, g_arena_end;

static int cls_of(uint32_t sz, uint32_t *rounded) {
    if (sz <= 1024) { uint32_t r = align_up(sz ? sz : 1, 16); *rounded = r; return (int)(r / 16) - 1; } // 0..63? 1024/16=64 -> 63
    uint32_t r = 2048; int c = 0;
    while (r < sz) { r <<= 1; c++; }
    *rounded = r;
    return -2 - c;   // large classes encoded negative
}

static std::vector<uint32_t> g_free_large[16];

uint32_t gmalloc(uint32_t size, bool zero) {
    if (size >= BIG) {
        uint32_t r = vm_alloc(0, size + HDR);
        if (!r) return 0;
        W32(r, size); W32(r + 4, MAGIC); W32(r + 8, 1);
        return r + HDR;
    }
    uint32_t rounded;
    int cl = cls_of(size, &rounded);
    uint32_t p = 0;
    {
        std::lock_guard<std::mutex> l(g_heap_mutex);
        auto &fl = cl >= 0 ? g_free[cl] : g_free_large[-2 - cl];
        if (!fl.empty()) { p = fl.back(); fl.pop_back(); }
        else {
            if (g_arena_cur + rounded + HDR > g_arena_end) {
                uint32_t a = vm_alloc(0, 8u << 20);
                if (!a) return 0;
                g_arena_cur = a; g_arena_end = a + (8u << 20);
            }
            p = g_arena_cur + HDR;
            g_arena_cur += rounded + HDR;
        }
    }
    W32(p - HDR, size); W32(p - HDR + 4, MAGIC); W32(p - HDR + 8, 0); W32(p - HDR + 12, rounded);
    if (zero) memset(H(p), 0, rounded);
    return p;
}

uint32_t gsize(uint32_t p) { return p ? R32(p - HDR) : 0; }

void gfree(uint32_t p) {
    if (!p) return;
    if (R32(p - HDR + 4) != MAGIC) { rtlog("gfree: bad pointer %08x", p); return; }
    W32(p - HDR + 4, 0);
    if (R32(p - HDR + 8) == 1) { vm_free(p - HDR); return; }
    uint32_t rounded = R32(p - HDR + 12), r2;
    int cl = cls_of(rounded, &r2);
    std::lock_guard<std::mutex> l(g_heap_mutex);
    (cl >= 0 ? g_free[cl] : g_free_large[-2 - cl]).push_back(p);
}

uint32_t grealloc(uint32_t p, uint32_t size) {
    if (!p) return gmalloc(size);
    uint32_t old = gsize(p);
    if (R32(p - HDR + 8) == 0 && size <= R32(p - HDR + 12)) { W32(p - HDR, size); return p; }
    uint32_t n = gmalloc(size);
    if (!n) return 0;
    memcpy(H(n), H(p), old < size ? old : size);
    gfree(p);
    return n;
}

uint32_t gstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = gmalloc(n);
    memcpy(H(p), s, n);
    return p;
}

void gstrcpy(uint32_t dst, const char *src, uint32_t max) {
    if (!max) return;
    size_t n = strlen(src);
    if (n >= max) n = max - 1;
    memcpy(H(dst), src, n);
    G[dst + n] = 0;
}

void heap_init() {}
