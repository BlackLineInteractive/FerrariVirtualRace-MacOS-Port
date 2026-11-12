// Shared runtime interface for the Win32 / DirectX replacement layer.
#pragma once
#include "cpu.h"
#include <string>
#include <cstdio>
#include <cstdarg>

// ---- guest memory layout -------------------------------------------------
constexpr uint32_t GUEST_IMAGE_BASE = 0x00400000;
constexpr uint32_t GUEST_STACK_AREA = 0x01000000;   // thread stacks live here
constexpr uint32_t GUEST_STACK_SIZE = 0x00400000;   // 4 MiB per thread
constexpr uint32_t GUEST_HEAP_START = 0x10000000;
constexpr uint32_t GUEST_HEAP_END   = 0xE0000000;
constexpr uint32_t THUNK_BASE       = 0xFF000000;   // fake addresses of host functions
constexpr uint32_t THUNK_STRIDE     = 16;
constexpr uint32_t GUEST_RETURN_MAGIC = 0xFEFEFEF0; // return address used for host->guest calls

// ---- API registry ---------------------------------------------------------
typedef void (*HostFn)(CPU *c);
struct ApiReg { ApiReg(const char *name, HostFn fn); };
// Define a stdcall API: API(kernel32, GetTickCount, 0) { RET(...); }
#define API(dll, name, nargs) \
    static void api_##dll##_##name(CPU *c); \
    static ApiReg reg_##dll##_##name(#dll ".dll!" #name, api_##dll##_##name); \
    static constexpr int NARGS_##dll##_##name = nargs; \
    static void api_##dll##_##name(CPU *c)
#define ARG(i) R32(c->esp + 4 + 4 * (i))
#define ARGF(i) RF(c->esp + 4 + 4 * (i))
// stdcall return: pop return address + n args
#define RETN(n, v) do { c->eax = (uint32_t)(v); c->esp += 4 + 4 * (n); return; } while (0)

uint32_t thunk_for(const char *name, HostFn fn);   // allocate a thunk address for a host function
HostFn   lookup_api(const char *name);

// call guest function at `fn` with stdcall/cdecl args (pushed right-to-left). Returns eax.
uint32_t guest_call(CPU *c, uint32_t fn, int nargs, const uint32_t *args, bool cdecl_cleanup = false);
CPU *cur_cpu();

// ---- guest heap -----------------------------------------------------------
uint32_t gmalloc(uint32_t size, bool zero = false);
void     gfree(uint32_t p);
uint32_t gsize(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t vm_alloc(uint32_t addr, uint32_t size);     // page-granular region allocation
void     vm_free(uint32_t addr);
uint32_t gstrdup(const char *s);

static inline const char *GS(uint32_t a) { return a ? (const char *)H(a) : nullptr; }
static inline const uint16_t *GW(uint32_t a) { return a ? (const uint16_t *)H(a) : nullptr; }
void gstrcpy(uint32_t dst, const char *src, uint32_t max);

// ---- misc -----------------------------------------------------------------
void rtlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int g_trace_api;
std::string host_path(const char *winpath);          // map a Windows path to a host path
extern std::string g_game_dir, g_user_dir;
void set_last_error(uint32_t e);

// COM helpers: build a guest object whose vtable points at host thunks.
// methods[i] implements vtable slot i; each receives `this` as ARG(0).
uint32_t com_make_vtable(const char *iface, const HostFn *methods, int count);
uint32_t com_new_object(uint32_t vtable, uint32_t extra_bytes);
