// Loader, dispatcher, threads and FPU helpers for the recompiled game.
#include "rt.h"
#include <sys/mman.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <execinfo.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <fenv.h>
#include <climits>
#include <string>
#include <SDL3/SDL.h>
#include "win32ui.h"

uint8_t *G;
int g_trace_api = 0;
std::string g_game_dir, g_user_dir;

extern "C" {
typedef struct { uint32_t a; GuestFn f; } FnEnt;
extern const unsigned g_fn_count;
extern const FnEnt g_fn_table[];
extern const uint32_t g_entry_point;
typedef struct { uint32_t slot; const char *name; } ImpEnt;
extern const unsigned g_imp_count;
extern const ImpEnt g_imp_table[];
}


void rtlog(const char *fmt, ...) {
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

// ---------------------------------------------------------------- API registry
struct Thunk { std::string name; HostFn fn; };
static std::vector<Thunk> &thunks() { static std::vector<Thunk> v; return v; }
static std::unordered_map<std::string, HostFn> &registry() { static std::unordered_map<std::string, HostFn> r; return r; }
static std::mutex g_thunk_mutex;

ApiReg::ApiReg(const char *name, HostFn fn) { registry()[name] = fn; }

HostFn lookup_api(const char *name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

uint32_t thunk_for(const char *name, HostFn fn) {
    std::lock_guard<std::mutex> l(g_thunk_mutex);
    thunks().push_back({name, fn});
    return THUNK_BASE + (uint32_t)(thunks().size() - 1) * THUNK_STRIDE;
}

static void missing_api(CPU *c, const std::string &name) {
    rtlog("FATAL: unimplemented API %s called (ret=%08x)", name.c_str(), R32(c->esp));
    abort();
}

// ---------------------------------------------------------------- dispatcher
extern "C" void call_indirect(CPU *c, uint32_t t) {
    if (t >= THUNK_BASE) {
        uint32_t id = (t - THUNK_BASE) / THUNK_STRIDE;
        Thunk *th;
        {
            std::lock_guard<std::mutex> l(g_thunk_mutex);
            if (id >= thunks().size()) { rtlog("FATAL: bad thunk %08x", t); abort(); }
            th = &thunks()[id];
        }
        if (g_trace_api) rtlog("[api] %s (ret %08x)", th->name.c_str(), R32(c->esp));
        if (!th->fn) missing_api(c, th->name);
        th->fn(c);
        return;
    }
    // binary search recompiled functions
    unsigned lo = 0, hi = g_fn_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_fn_table[mid].a < t) lo = mid + 1; else hi = mid;
    }
    if (lo < g_fn_count && g_fn_table[lo].a == t) { g_fn_table[lo].f(c); return; }
    rtlog("FATAL: indirect transfer to unknown guest address %08x (ret %08x)", t, R32(c->esp));
    abort();
}

extern "C" void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    rtlog("FATAL: untranslated code at %08x: %s (esp=%08x)", addr, what, c->esp);
    abort();
}

uint32_t guest_call(CPU *c, uint32_t fn, int nargs, const uint32_t *args, bool cdecl_cleanup) {
    uint32_t saved_esp = c->esp;
    for (int i = nargs - 1; i >= 0; i--) PUSH(c, args[i]);
    PUSH(c, GUEST_RETURN_MAGIC);
    call_indirect(c, fn);
    // stdcall callee popped its args; cdecl did not. Either way restore.
    (void)cdecl_cleanup;
    c->esp = saved_esp;
    return c->eax;
}

// ---------------------------------------------------------------- COM helpers
uint32_t com_make_vtable(const char *iface, const HostFn *methods, int count) {
    uint32_t vt = gmalloc(4 * count, true);
    for (int i = 0; i < count; i++) {
        char nm[96];
        snprintf(nm, sizeof nm, "%s::slot%d", iface, i);
        W32(vt + 4 * i, thunk_for(nm, methods[i]));
    }
    return vt;
}

uint32_t com_new_object(uint32_t vtable, uint32_t extra) {
    uint32_t o = gmalloc(4 + extra, true);
    W32(o, vtable);
    return o;
}

// ---------------------------------------------------------------- threads / TEB
static thread_local CPU *t_cpu;
CPU *cur_cpu() { return t_cpu; }
static std::mutex g_stack_mutex;
static uint32_t g_next_stack = GUEST_STACK_AREA;
static uint32_t g_next_tid = 1;
uint32_t g_peb;

static uint32_t alloc_stack() {
    std::lock_guard<std::mutex> l(g_stack_mutex);
    uint32_t s = g_next_stack;
    g_next_stack += GUEST_STACK_SIZE;
    if (g_next_stack > GUEST_HEAP_START) { rtlog("FATAL: out of stack area"); abort(); }
    return s;
}

CPU *new_cpu() {
    CPU *c = (CPU *)calloc(1, sizeof(CPU));
    uint32_t stack = alloc_stack();
    c->esp = stack + GUEST_STACK_SIZE - 64;
    c->fcw = 0x27f;            // 53-bit precision, round-to-nearest, all exceptions masked
    c->mxcsr = 0x1f80;
    uint32_t teb = gmalloc(0x1000, true);
    W32(teb + 0x00, 0xFFFFFFFF);            // SEH chain end
    W32(teb + 0x04, stack + GUEST_STACK_SIZE);
    W32(teb + 0x08, stack);
    W32(teb + 0x18, teb);
    W32(teb + 0x30, g_peb);
    {
        std::lock_guard<std::mutex> l(g_stack_mutex);
        c->tid = g_next_tid++;
    }
    W32(teb + 0x24, c->tid);
    W32(teb + 0x20, 1);
    c->fs_base = teb;
    return c;
}

void bind_cpu(CPU *c) { t_cpu = c; }

// ---------------------------------------------------------------- CPU helpers
extern "C" uint32_t rt_cpuid(CPU *c) {
    uint32_t leaf = c->eax;
    if (leaf == 0) { c->eax = 1; c->ebx = 0x756e6547; c->edx = 0x49656e69; c->ecx = 0x6c65746e; }
    else if (leaf == 1) { c->eax = 0x6fb; c->ebx = 0; c->ecx = 0x1; /* SSE3 */ c->edx = 0x0383fbff; /* FPU..SSE2, no HTT */ }
    else { c->eax = c->ebx = c->ecx = c->edx = 0; }
    return 0;
}

extern "C" uint64_t rt_rdtsc(void) { return mach_absolute_time(); }

static double round_mode(CPU *c, double v, int rc) {
    if (rc < 0) rc = (c->fcw >> 10) & 3;
    switch (rc) {
        case 0: return nearbyint(v);
        case 1: return floor(v);
        case 2: return ceil(v);
        default: return trunc(v);
    }
}

extern "C" int64_t rt_fist(CPU *c, double v, int rc, int bits) {
    double r = round_mode(c, v, rc);
    if (bits == 16) { if (!(r >= -32768.0 && r <= 32767.0)) return 0x8000; return (int64_t)r; }
    if (bits == 32) { if (!(r >= -2147483648.0 && r <= 2147483647.0)) return (int64_t)0x80000000u; return (int64_t)r; }
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)r;
}

extern "C" double rt_round(CPU *c, double v) { return round_mode(c, v, -1); }

extern "C" void rt_fxam(CPU *c) {
    double v = ST(0);
    c->fsw &= ~0x4700;
    if (std::signbit(v)) c->fsw |= 0x0200;
    if (std::isnan(v)) c->fsw |= 0x0100;
    else if (std::isinf(v)) c->fsw |= 0x0500;
    else if (v == 0) c->fsw |= 0x4000;
    else if (std::fpclassify(v) == FP_SUBNORMAL) c->fsw |= 0x4400;
    else c->fsw |= 0x0400;
}

extern "C" void rt_fnstenv(CPU *c, uint32_t a) {
    memset(H(a), 0, 28);
    W16(a, c->fcw); W16(a + 4, (uint16_t)((c->fsw & ~0x3800) | ((c->ftop & 7) << 11))); W16(a + 8, 0xffff);
}
extern "C" void rt_fldenv(CPU *c, uint32_t a) { c->fcw = R16(a); c->fsw = R16(a + 4); c->ftop = (c->fsw >> 11) & 7; }
extern "C" void rt_fnsave(CPU *c, uint32_t a) {
    rt_fnstenv(c, a);
    for (int i = 0; i < 8; i++) rt_store80(a + 28 + 10 * i, ST(i));
    c->fcw = 0x37f; c->fsw = 0; c->ftop = 0;
}
extern "C" void rt_frstor(CPU *c, uint32_t a) {
    rt_fldenv(c, a);
    for (int i = 0; i < 8; i++) ST(i) = rt_load80(a + 28 + 10 * i);
}

extern "C" double rt_load80(uint32_t a) {
    long double v;
    memcpy(&v, H(a), 10);   // x86_64 host long double is the same 80-bit format
    return (double)v;
}
extern "C" void rt_store80(uint32_t a, double v) {
    long double l = v;
    memcpy(H(a), &l, 10);
}

// ---------------------------------------------------------------- crash handler
static void on_crash(int sig, siginfo_t *si, void *) {
    uintptr_t fa = (uintptr_t)si->si_addr;
    CPU *c = t_cpu;
    if (fa >= (uintptr_t)G && fa < (uintptr_t)G + 0x100000000ull)
        rtlog("CRASH: signal %d at guest address %08x", sig, (uint32_t)(fa - (uintptr_t)G));
    else
        rtlog("CRASH: signal %d at host address %p", sig, (void *)fa);
    if (c) rtlog("  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x",
                 c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    void *bt[64];
    int n = backtrace(bt, 64);
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}

// ---------------------------------------------------------------- loader
void heap_init();
void win32_init();
void ensure_default_config();

static void load_image() {
    // Map the original exe's sections at their virtual addresses (exe has no relocations).
    std::string path = g_game_dir + "/FerrariVR_Hi.exe";
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { rtlog("FATAL: cannot open %s", path.c_str()); exit(1); }
    std::vector<uint8_t> file;
    fseek(f, 0, SEEK_END); file.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    fread(file.data(), 1, file.size(), f); fclose(f);
    auto u16 = [&](size_t o) { uint16_t v; memcpy(&v, &file[o], 2); return v; };
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, &file[o], 4); return v; };
    uint32_t pe = u32(0x3c);
    uint16_t nsec = u16(pe + 6), optsz = u16(pe + 20);
    uint32_t hdrsz = u32(pe + 24 + 60);
    memcpy(G + GUEST_IMAGE_BASE, file.data(), hdrsz);
    size_t sh = pe + 24 + optsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t va = u32(sh + 12), rawsz = u32(sh + 16), rawptr = u32(sh + 20), vsz = u32(sh + 8);
        uint32_t n = rawsz < vsz || vsz == 0 ? rawsz : vsz;
        memcpy(G + GUEST_IMAGE_BASE + va, file.data() + rawptr, n);
    }
    // IAT: point each slot at a thunk
    int missing = 0;
    for (unsigned i = 0; i < g_imp_count; i++) {
        HostFn fn = lookup_api(g_imp_table[i].name);
        if (!fn) missing++;
        W32(g_imp_table[i].slot, thunk_for(g_imp_table[i].name, fn));
    }
    rtlog("image loaded, %u imports (%d not implemented yet)", g_imp_count, missing);
}

// ---------------------------------------------------------------- game files location
// FVR_GAME_DIR, then the app bundle's Resources/game, then the folder next to the .app or binary,
// then the folder remembered from an earlier launch; otherwise ask once with a folder picker.
static bool has_game(const std::string &d) {
    if (d.empty()) return false;
    std::string p = d + "/FerrariVR_Hi.exe";
    return access(p.c_str(), R_OK) == 0;
}
static bool find_game_dir(const char *argv0) {
    if (const char *gd = getenv("FVR_GAME_DIR")) { g_game_dir = gd; return true; }
    char buf[PATH_MAX];
    std::string exe = realpath(argv0, buf) ? buf : argv0;
    std::string dir = exe.substr(0, exe.rfind('/'));
    std::string saved_file = g_user_dir + "/game_path.txt";
    std::vector<std::string> cand = {dir + "/../Resources/game", dir + "/../../..", dir + "/../../../game",
                                     dir + "/../../../../../extracted", dir + "/..", dir + "/../../extracted"};
    if (FILE *f = fopen(saved_file.c_str(), "r")) {
        char line[PATH_MAX] = {};
        if (fgets(line, sizeof line, f)) { std::string l = line; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back(); cand.insert(cand.begin() + 1, l); }
        fclose(f);
    }
    for (auto &c : cand) if (has_game(c)) { g_game_dir = realpath(c.c_str(), buf) ? buf : c; return true; }

    // ask
    ui_ensure_sdl();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Ferrari Virtual Race",
        "Choose the folder with the installed (or extracted) Windows version of Ferrari Virtual Race.\n"
        "It must contain FerrariVR_Hi.exe.", nullptr);
    for (;;) {
        struct Pick { bool done = false; std::string path; } pick;
        SDL_ShowOpenFolderDialog([](void *ud, const char *const *list, int) {
            Pick *p = (Pick *)ud;
            if (list && list[0]) p->path = list[0];
            p->done = true;
        }, &pick, nullptr, nullptr, false);
        while (!pick.done) { SDL_PumpEvents(); SDL_Delay(20); }
        if (pick.path.empty()) return false;
        if (has_game(pick.path)) {
            g_game_dir = pick.path;
            if (FILE *f = fopen(saved_file.c_str(), "w")) { fprintf(f, "%s\n", pick.path.c_str()); fclose(f); }
            return true;
        }
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Ferrari Virtual Race",
            "FerrariVR_Hi.exe was not found in that folder. If you only have the installer, extract it first:\n"
            "tools/extract_game.sh <installer.exe> <folder>", nullptr);
    }
}

int main(int argc, char **argv) {
    const char *trace = getenv("FVR_TRACE");
    g_trace_api = trace && *trace == '1';

    const char *home = getenv("HOME");
    g_user_dir = std::string(home ? home : "/tmp") + "/Library/Application Support/FerrariVirtualRace";
    std::string mk = "mkdir -p '" + g_user_dir + "'";
    system(mk.c_str());
    // Started from Finder (no terminal): keep the log in the user folder for bug reports.
    if (strstr(argv[0], ".app/Contents/MacOS/") && !isatty(2)) {
        freopen((g_user_dir + "/fvr.log").c_str(), "w", stderr);
        setvbuf(stderr, nullptr, _IOLBF, 0);
    }
    if (!find_game_dir(argv[0])) return 1;
    rtlog("game files: %s", g_game_dir.c_str());

    G = (uint8_t *)mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (G == MAP_FAILED) { perror("mmap guest space"); return 1; }
    mprotect(G, 0x10000, PROT_NONE);   // catch null derefs

    struct sigaction sa = {};
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

    heap_init();
    g_peb = gmalloc(0x1000, true);
    load_image();
    win32_init();
    ensure_default_config();

    CPU *c = new_cpu();
    bind_cpu(c);
    rtlog("starting guest at %08x", g_entry_point);
    PUSH(c, GUEST_RETURN_MAGIC);
    call_indirect(c, g_entry_point);
    rtlog("guest entry returned eax=%08x", c->eax);
    return (int)c->eax;
}
