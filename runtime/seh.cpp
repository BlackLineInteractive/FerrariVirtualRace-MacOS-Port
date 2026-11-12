// Structured exception handling (C++ throw / RaiseException). Initial version: diagnose and stop.
#include "rt.h"
#include <cstdlib>

void seh_raise(CPU *c, uint32_t code, uint32_t flags, uint32_t nargs, uint32_t args) {
    rtlog("FATAL: RaiseException code=%08x flags=%x nargs=%u (ret %08x)", code, flags, nargs, R32(c->esp));
    if (code == 0xE06D7363 && nargs >= 3) {
        // MSVC C++ exception: args[2] = ThrowInfo*, look up the type name for the log
        uint32_t ti = R32(args + 8);
        uint32_t cta = ti ? R32(ti + 12) : 0;
        uint32_t ct = cta ? R32(cta + 4) : 0;
        uint32_t td = ct ? R32(ct + 4) : 0;
        if (td) rtlog("  C++ exception type: %s", GS(td + 8));
    }
    abort();
}
void seh_rtl_unwind(CPU *c) { rtlog("FATAL: RtlUnwind not implemented (ret %08x)", R32(c->esp)); abort(); }
