// Guest x86-32 CPU state and memory access helpers used by recompiled code.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union {
    float f[4];
    double d[2];
    uint8_t b[16];
    uint16_t w[4 * 2];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
} XMM;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    // Flags kept as separate bytes; the recompiler only materialises the ones that are read.
    uint8_t cf, zf, sf, of, pf, af, df, _pad;
    XMM xmm[8];
    // x87: values kept as double (MSVC sets 53-bit precision by default).
    double st[8];
    uint64_t mm[8];
    uint32_t ftop;
    uint16_t fcw, fsw;
    uint32_t mxcsr;
    uint32_t fs_base;   // guest address of the thread's TEB
    uint32_t tid;
} CPU;

// Guest memory: a 4 GiB reservation; guest address A lives at G + A.
extern uint8_t *G;

static inline uint8_t  *H(uint32_t a) { return G + a; }
static inline uint8_t  R8 (uint32_t a) { return G[a]; }
static inline uint16_t R16(uint32_t a) { uint16_t v; memcpy(&v, G + a, 2); return v; }
static inline uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, G + a, 4); return v; }
static inline uint64_t R64(uint32_t a) { uint64_t v; memcpy(&v, G + a, 8); return v; }
static inline float    RF (uint32_t a) { float v; memcpy(&v, G + a, 4); return v; }
static inline double   RD (uint32_t a) { double v; memcpy(&v, G + a, 8); return v; }
static inline void W8 (uint32_t a, uint8_t v)  { G[a] = v; }
static inline void W16(uint32_t a, uint16_t v) { memcpy(G + a, &v, 2); }
static inline void W32(uint32_t a, uint32_t v) { memcpy(G + a, &v, 4); }
static inline void W64(uint32_t a, uint64_t v) { memcpy(G + a, &v, 8); }
static inline void WF (uint32_t a, float v)    { memcpy(G + a, &v, 4); }
static inline void WD (uint32_t a, double v)   { memcpy(G + a, &v, 8); }

static inline void PUSH(CPU *c, uint32_t v) { c->esp -= 4; W32(c->esp, v); }
static inline uint32_t POP(CPU *c) { uint32_t v = R32(c->esp); c->esp += 4; return v; }

static inline uint8_t PARITY(uint32_t v) { return !__builtin_parity(v & 0xff); }

// x87 stack helpers
#define ST(i) (c->st[(c->ftop + (i)) & 7])
static inline void FPUSH(CPU *c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; }
static inline double FPOP(CPU *c) { double v = c->st[c->ftop]; c->ftop = (c->ftop + 1) & 7; return v; }

typedef void (*GuestFn)(CPU *);

// Indirect control transfer: guest address -> host function (recompiled code or API thunk).
void call_indirect(CPU *c, uint32_t target);
// Called when recompiled code reaches something the recompiler could not translate.
void rt_unimpl(CPU *c, uint32_t addr, const char *what) __attribute__((noreturn));
// x87 / misc helpers implemented in the runtime
uint32_t rt_cpuid(CPU *c);
uint64_t rt_rdtsc(void);

// x87 helpers. rc: -1 = use FPU control word rounding, 3 = truncate.
int64_t rt_fist(CPU *c, double v, int rc, int bits);
double rt_round(CPU *c, double v);
void rt_fxam(CPU *c);
void rt_fnstenv(CPU *c, uint32_t a);
void rt_fldenv(CPU *c, uint32_t a);
void rt_fnsave(CPU *c, uint32_t a);
void rt_frstor(CPU *c, uint32_t a);
double rt_load80(uint32_t a);
void rt_store80(uint32_t a, double v);
// SSE conversions with x86 "integer indefinite" (0x80000000) on overflow/NaN.
static inline uint32_t rt_cvt_f2i_trunc(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static inline uint32_t rt_cvt_f2i_round(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return 0x80000000u;
    return (uint32_t)(int32_t)nearbyint(v);
}

#ifdef __cplusplus
}
#endif
