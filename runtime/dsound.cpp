// DirectSound 8 replacement: software mixer on an SDL3 audio stream (48 kHz float stereo).
// Sound buffers live in guest memory so Lock() can hand out guest pointers directly.
#include "rt.h"
#include <SDL3/SDL.h>
#include <mutex>
#include <vector>
#include <map>
#include <cmath>
#include <cstring>
#include <algorithm>

enum : uint32_t { DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057, DSERR_NOINTERFACE = 0x80004002, DSERR_CONTROLUNAVAIL = 0x8878001E };
static constexpr int OUT_RATE = 48000;

struct Vec3 { float x, y, z; };

struct SharedMem { uint32_t mem = 0, size = 0; int refs = 1; };

struct DSBuf {
    uint32_t guest = 0, guest3d = 0, guest_notify = 0;
    int refs = 1;
    bool primary = false;
    SharedMem *data = nullptr;
    uint16_t channels = 2, bits = 16, align = 4;
    uint32_t rate = 22050, base_rate = 22050;
    uint32_t flags = 0;
    int32_t volume = 0, pan = 0;
    double pos = 0;               // in frames
    bool playing = false, looping = false;
    // 3D
    Vec3 p3{0, 0, 0}, v3{0, 0, 0};
    float min_dist = 1.0f, max_dist = 1e9f;
    uint32_t mode3d = 0;
    // derived gains (updated under the mixer lock)
    float gl = 1, gr = 1;
};

static std::mutex g_mix;
static std::vector<DSBuf *> g_bufs;
static std::map<uint32_t, DSBuf *> g_by_guest;      // guest object (buffer, 3d, notify) -> buffer
static SDL_AudioStream *g_stream;
static uint32_t g_vt_ds, g_vt_buf, g_vt_3d, g_vt_listener, g_vt_notify;
static uint32_t g_ds_obj, g_listener_obj;
static int g_ds_refs = 1;
// listener
static Vec3 L_pos{0, 0, 0}, L_front{0, 0, 1}, L_top{0, 1, 0};
static float L_rolloff = 1.0f, L_distf = 1.0f;

static float db_to_lin(int32_t v) { if (v <= -10000) return 0.0f; return powf(10.0f, v / 2000.0f); }

static void update_gains(DSBuf *b) {
    float g = db_to_lin(b->volume);
    float l = 1, r = 1;
    if (b->pan > 0) l = db_to_lin(-b->pan); else if (b->pan < 0) r = db_to_lin(b->pan);
    if ((b->flags & 0x10) && b->mode3d != 2) {
        Vec3 d = b->p3;
        if (b->mode3d == 0) d = {b->p3.x - L_pos.x, b->p3.y - L_pos.y, b->p3.z - L_pos.z};
        float dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) * L_distf;
        float md = b->min_dist * L_distf;
        float att = 1.0f;
        if (dist > md) {
            float dd = std::min(dist, b->max_dist * L_distf);
            att = md / (md + L_rolloff * (dd - md));
        }
        g *= att;
        // right vector = top x front (left-handed D3D coordinates)
        Vec3 f = L_front, t = L_top;
        Vec3 rt{t.y * f.z - t.z * f.y, t.z * f.x - t.x * f.z, t.x * f.y - t.y * f.x};
        float rl = sqrtf(rt.x * rt.x + rt.y * rt.y + rt.z * rt.z);
        if (b->mode3d == 1) rt = {1, 0, 0}, rl = 1;
        if (dist > 1e-4f && rl > 1e-6f) {
            float s = (d.x * rt.x + d.y * rt.y + d.z * rt.z) / (rl * dist / L_distf);   // -1 left .. +1 right
            s = std::max(-1.0f, std::min(1.0f, s));
            float a = (s + 1.0f) * 0.25f * (float)M_PI;     // constant power pan
            l *= cosf(a) * 1.41421356f; r *= sinf(a) * 1.41421356f;
            l = std::min(l, 1.0f); r = std::min(r, 1.0f);
        }
    }
    b->gl = g * l; b->gr = g * r;
}

static inline float sample_at(const DSBuf *b, const uint8_t *base, uint32_t frame, int ch) {
    if (b->bits == 16) { int16_t s; memcpy(&s, base + frame * b->align + ch * 2, 2); return s / 32768.0f; }
    return (base[frame * b->align + ch] - 128) / 128.0f;
}

static void SDLCALL audio_cb(void *, SDL_AudioStream *stream, int additional, int) {
    int frames = additional / 8;
    if (frames <= 0) return;
    std::vector<float> out((size_t)frames * 2, 0.0f);
    {
        std::lock_guard<std::mutex> l(g_mix);
        for (DSBuf *b : g_bufs) {
            if (!b->playing || b->primary || !b->data || !b->data->size || !b->align) continue;
            uint32_t nframes = b->data->size / b->align;
            if (!nframes) continue;
            const uint8_t *base = H(b->data->mem);
            double step = (double)b->rate / OUT_RATE;
            double pos = b->pos;
            float gl = b->gl, gr = b->gr;
            for (int i = 0; i < frames; i++) {
                uint32_t i0 = (uint32_t)pos;
                if (i0 >= nframes) {
                    if (b->looping) { pos = fmod(pos, (double)nframes); i0 = (uint32_t)pos; }
                    else { b->playing = false; pos = 0; break; }
                }
                uint32_t i1 = i0 + 1;
                if (i1 >= nframes) i1 = b->looping ? 0 : i0;
                float t = (float)(pos - i0);
                float sl, sr;
                if (b->channels == 1) {
                    float s = sample_at(b, base, i0, 0) * (1 - t) + sample_at(b, base, i1, 0) * t;
                    sl = sr = s;
                } else {
                    sl = sample_at(b, base, i0, 0) * (1 - t) + sample_at(b, base, i1, 0) * t;
                    sr = sample_at(b, base, i0, 1) * (1 - t) + sample_at(b, base, i1, 1) * t;
                }
                out[2 * i] += sl * gl;
                out[2 * i + 1] += sr * gr;
                pos += step;
            }
            b->pos = pos;
        }
    }
    for (auto &s : out) s = std::max(-1.0f, std::min(1.0f, s));
    SDL_PutAudioStreamData(stream, out.data(), (int)(out.size() * sizeof(float)));
}

static bool open_audio() {
    if (g_stream) return true;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, OUT_RATE};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, nullptr);
    if (!g_stream) { rtlog("dsound: cannot open audio: %s", SDL_GetError()); return false; }
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

static DSBuf *buf_of(uint32_t g) {
    auto it = g_by_guest.find(g);
    return it == g_by_guest.end() ? nullptr : it->second;
}

static void destroy_buf(DSBuf *b) {
    g_bufs.erase(std::remove(g_bufs.begin(), g_bufs.end(), b), g_bufs.end());
    for (uint32_t g : {b->guest, b->guest3d, b->guest_notify}) if (g) { g_by_guest.erase(g); gfree(g); }
    if (b->data && --b->data->refs == 0) { if (b->data->mem) gfree(b->data->mem); delete b->data; }
    if (b->guest == g_listener_obj) g_listener_obj = 0;
    delete b;
}

#define M(name) static void name(CPU *c)
#define LOCK std::lock_guard<std::mutex> lk_(g_mix)

// ---------------------------------------------------------------- IDirectSoundBuffer8
static uint32_t make_sub(DSBuf *b, uint32_t vt) {
    uint32_t o = com_new_object(vt, 4);
    g_by_guest[o] = b;
    return o;
}
M(b_QueryInterface) {
    LOCK;
    DSBuf *b = buf_of(ARG(0));
    uint32_t iid = R32(ARG(1));
    uint32_t r = 0;
    if (!b) RETN(3, DSERR_INVALIDPARAM);
    if (iid == 0x279AFA86) { if (!b->guest3d) b->guest3d = make_sub(b, g_vt_3d); r = b->guest3d; }
    else if (iid == 0x279AFA84) {
        if (!g_listener_obj) { g_listener_obj = com_new_object(g_vt_listener, 4); g_by_guest[g_listener_obj] = b; }
        r = g_listener_obj;
    } else if (iid == 0xB0210783) { if (!b->guest_notify) b->guest_notify = make_sub(b, g_vt_notify); r = b->guest_notify; }
    else if (iid == 0x6825A449 || iid == 0x279AFA85 || iid == 0) r = b->guest;
    if (!r) { rtlog("dsound: QueryInterface unknown IID %08x", iid); W32(ARG(2), 0); RETN(3, DSERR_NOINTERFACE); }
    b->refs++;
    W32(ARG(2), r);
    RETN(3, DS_OK);
}
M(b_AddRef) { LOCK; DSBuf *b = buf_of(ARG(0)); if (b) b->refs++; RETN(1, b ? b->refs : 0); }
M(b_Release) {
    LOCK;
    DSBuf *b = buf_of(ARG(0));
    if (!b) RETN(1, 0);
    int r = --b->refs;
    if (r <= 0) { destroy_buf(b); r = 0; }
    RETN(1, (uint32_t)r);
}
M(b_GetCaps) {
    LOCK; DSBuf *b = buf_of(ARG(0)); uint32_t p = ARG(1);
    W32(p + 4, b->flags | 8); W32(p + 8, b->data ? b->data->size : 0); W32(p + 12, 0); W32(p + 16, 0);
    RETN(2, DS_OK);
}
M(b_GetCurrentPosition) {
    LOCK; DSBuf *b = buf_of(ARG(0));
    uint32_t play = 0, write = 0;
    if (b && b->data && b->data->size) {
        play = ((uint32_t)b->pos * b->align) % b->data->size;
        uint32_t lead = (uint32_t)(b->rate / 100) * b->align;     // ~10 ms
        write = b->playing ? (play + lead) % b->data->size : play;
    }
    if (ARG(1)) W32(ARG(1), play);
    if (ARG(2)) W32(ARG(2), write);
    RETN(3, DS_OK);
}
M(b_GetFormat) {
    LOCK; DSBuf *b = buf_of(ARG(0));
    uint32_t p = ARG(1);
    if (p && ARG(2) >= 16) {
        W16(p, 1); W16(p + 2, b->channels); W32(p + 4, b->base_rate); W32(p + 8, b->base_rate * b->align);
        W16(p + 12, b->align); W16(p + 14, b->bits);
        if (ARG(2) >= 18) W16(p + 16, 0);
    }
    if (ARG(3)) W32(ARG(3), 18);
    RETN(4, DS_OK);
}
M(b_GetVolume) { LOCK; W32(ARG(1), (uint32_t)buf_of(ARG(0))->volume); RETN(2, DS_OK); }
M(b_GetPan) { LOCK; W32(ARG(1), (uint32_t)buf_of(ARG(0))->pan); RETN(2, DS_OK); }
M(b_GetFrequency) { LOCK; W32(ARG(1), buf_of(ARG(0))->rate); RETN(2, DS_OK); }
M(b_GetStatus) {
    LOCK; DSBuf *b = buf_of(ARG(0));
    W32(ARG(1), b && b->playing ? (1 | (b->looping ? 4 : 0)) : 0);
    RETN(2, DS_OK);
}
M(b_Initialize) { RETN(3, DS_OK); }
M(b_Lock) {
    LOCK; DSBuf *b = buf_of(ARG(0));
    uint32_t off = ARG(1), bytes = ARG(2), fl = ARG(7);
    uint32_t size = b->data ? b->data->size : 0;
    if (!size) RETN(8, DSERR_INVALIDPARAM);
    if (fl & 1) { uint32_t play = ((uint32_t)b->pos * b->align) % size; off = b->playing ? (play + (b->rate / 100) * b->align) % size : play; }
    if (fl & 2) { off = 0; bytes = size; }
    if (bytes > size) bytes = size;
    off %= size;
    uint32_t l1 = std::min(bytes, size - off), l2 = bytes - l1;
    W32(ARG(3), b->data->mem + off); W32(ARG(4), l1);
    if (ARG(5)) W32(ARG(5), l2 ? b->data->mem : 0);
    if (ARG(6)) W32(ARG(6), l2);
    RETN(8, DS_OK);
}
M(b_Play) {
    LOCK; DSBuf *b = buf_of(ARG(0));
    if (b->primary) RETN(4, DS_OK);
    b->playing = true; b->looping = ARG(3) & 1;
    update_gains(b);
    RETN(4, DS_OK);
}
M(b_SetCurrentPosition) {
    LOCK; DSBuf *b = buf_of(ARG(0));
    if (b->align) b->pos = (double)(ARG(1) / b->align);
    RETN(2, DS_OK);
}
M(b_SetFormat) {
    LOCK; DSBuf *b = buf_of(ARG(0));
    if (b->primary && ARG(1)) {
        uint32_t p = ARG(1);
        b->channels = R16(p + 2); b->base_rate = b->rate = R32(p + 4); b->bits = R16(p + 14); b->align = R16(p + 12);
    }
    RETN(2, DS_OK);
}
M(b_SetVolume) { LOCK; DSBuf *b = buf_of(ARG(0)); b->volume = std::max(-10000, std::min(0, (int32_t)ARG(1))); update_gains(b); RETN(2, DS_OK); }
M(b_SetPan) { LOCK; DSBuf *b = buf_of(ARG(0)); b->pan = std::max(-10000, std::min(10000, (int32_t)ARG(1))); update_gains(b); RETN(2, DS_OK); }
M(b_SetFrequency) {
    LOCK; DSBuf *b = buf_of(ARG(0));
    uint32_t f = ARG(1);
    b->rate = f ? std::max<uint32_t>(100, std::min<uint32_t>(200000, f)) : b->base_rate;
    RETN(2, DS_OK);
}
M(b_Stop) { LOCK; DSBuf *b = buf_of(ARG(0)); b->playing = false; RETN(1, DS_OK); }
M(b_Unlock) { RETN(5, DS_OK); }
M(b_Restore) { RETN(1, DS_OK); }
M(b_SetFX) { RETN(4, DSERR_CONTROLUNAVAIL); }
M(b_AcquireResources) { RETN(4, DS_OK); }
M(b_GetObjectInPath) { RETN(5, DSERR_NOINTERFACE); }

// ---------------------------------------------------------------- IDirectSound3DBuffer
static Vec3 vec_args(CPU *c, int i) { return {ARGF(i), ARGF(i + 1), ARGF(i + 2)}; }
M(b3_GetAllParameters) { RETN(2, DS_OK); }
M(b3_GetConeAngles) { if (ARG(1)) W32(ARG(1), 360); if (ARG(2)) W32(ARG(2), 360); RETN(3, DS_OK); }
M(b3_GetConeOrientation) { WF(ARG(1), 0); WF(ARG(1) + 4, 0); WF(ARG(1) + 8, 1); RETN(2, DS_OK); }
M(b3_GetConeOutsideVolume) { W32(ARG(1), 0); RETN(2, DS_OK); }
M(b3_GetMaxDistance) { LOCK; WF(ARG(1), buf_of(ARG(0))->max_dist); RETN(2, DS_OK); }
M(b3_GetMinDistance) { LOCK; WF(ARG(1), buf_of(ARG(0))->min_dist); RETN(2, DS_OK); }
M(b3_GetMode) { LOCK; W32(ARG(1), buf_of(ARG(0))->mode3d); RETN(2, DS_OK); }
M(b3_GetPosition) { LOCK; DSBuf *b = buf_of(ARG(0)); WF(ARG(1), b->p3.x); WF(ARG(1) + 4, b->p3.y); WF(ARG(1) + 8, b->p3.z); RETN(2, DS_OK); }
M(b3_GetVelocity) { LOCK; DSBuf *b = buf_of(ARG(0)); WF(ARG(1), b->v3.x); WF(ARG(1) + 4, b->v3.y); WF(ARG(1) + 8, b->v3.z); RETN(2, DS_OK); }
M(b3_SetAllParameters) {
    LOCK; DSBuf *b = buf_of(ARG(0)); uint32_t p = ARG(1);
    b->p3 = {RF(p + 4), RF(p + 8), RF(p + 12)}; b->v3 = {RF(p + 16), RF(p + 20), RF(p + 24)};
    b->min_dist = RF(p + 56); b->max_dist = RF(p + 60); b->mode3d = R32(p + 64);
    update_gains(b);
    RETN(3, DS_OK);
}
M(b3_SetConeAngles) { RETN(4, DS_OK); }
M(b3_SetConeOrientation) { RETN(5, DS_OK); }
M(b3_SetConeOutsideVolume) { RETN(3, DS_OK); }
M(b3_SetMaxDistance) { LOCK; DSBuf *b = buf_of(ARG(0)); b->max_dist = ARGF(1); update_gains(b); RETN(3, DS_OK); }
M(b3_SetMinDistance) { LOCK; DSBuf *b = buf_of(ARG(0)); b->min_dist = std::max(1e-3f, ARGF(1)); update_gains(b); RETN(3, DS_OK); }
M(b3_SetMode) { LOCK; DSBuf *b = buf_of(ARG(0)); b->mode3d = ARG(1); update_gains(b); RETN(3, DS_OK); }
M(b3_SetPosition) { LOCK; DSBuf *b = buf_of(ARG(0)); b->p3 = vec_args(c, 1); update_gains(b); RETN(5, DS_OK); }
M(b3_SetVelocity) { LOCK; DSBuf *b = buf_of(ARG(0)); b->v3 = vec_args(c, 1); RETN(5, DS_OK); }

// ---------------------------------------------------------------- IDirectSound3DListener
static void update_all() { for (DSBuf *b : g_bufs) if (b->flags & 0x10) update_gains(b); }
M(l_GetAllParameters) { RETN(2, DS_OK); }
M(l_GetDistanceFactor) { WF(ARG(1), L_distf); RETN(2, DS_OK); }
M(l_GetDopplerFactor) { WF(ARG(1), 1.0f); RETN(2, DS_OK); }
M(l_GetOrientation) { WF(ARG(1), L_front.x); WF(ARG(1) + 4, L_front.y); WF(ARG(1) + 8, L_front.z); WF(ARG(2), L_top.x); WF(ARG(2) + 4, L_top.y); WF(ARG(2) + 8, L_top.z); RETN(3, DS_OK); }
M(l_GetPosition) { WF(ARG(1), L_pos.x); WF(ARG(1) + 4, L_pos.y); WF(ARG(1) + 8, L_pos.z); RETN(2, DS_OK); }
M(l_GetRolloffFactor) { WF(ARG(1), L_rolloff); RETN(2, DS_OK); }
M(l_GetVelocity) { WF(ARG(1), 0); WF(ARG(1) + 4, 0); WF(ARG(1) + 8, 0); RETN(2, DS_OK); }
M(l_SetAllParameters) {
    LOCK; uint32_t p = ARG(1);
    L_pos = {RF(p + 4), RF(p + 8), RF(p + 12)};
    L_distf = RF(p + 28); L_rolloff = RF(p + 32);
    L_front = {RF(p + 40), RF(p + 44), RF(p + 48)}; L_top = {RF(p + 52), RF(p + 56), RF(p + 60)};
    update_all();
    RETN(3, DS_OK);
}
M(l_SetDistanceFactor) { LOCK; L_distf = ARGF(1) > 0 ? ARGF(1) : 1.0f; update_all(); RETN(3, DS_OK); }
M(l_SetDopplerFactor) { RETN(3, DS_OK); }
M(l_SetOrientation) { LOCK; L_front = vec_args(c, 1); L_top = vec_args(c, 4); update_all(); RETN(8, DS_OK); }
M(l_SetPosition) { LOCK; L_pos = vec_args(c, 1); update_all(); RETN(5, DS_OK); }
M(l_SetRolloffFactor) { LOCK; L_rolloff = ARGF(1); update_all(); RETN(3, DS_OK); }
M(l_SetVelocity) { RETN(5, DS_OK); }
M(l_CommitDeferredSettings) { RETN(1, DS_OK); }
M(l_AddRef) { RETN(1, 1); }
M(l_Release) { RETN(1, 0); }

// ---------------------------------------------------------------- IDirectSoundNotify
M(n_SetNotificationPositions) { rtlog("dsound: position notifications requested (%u) - not supported", ARG(1)); RETN(3, DS_OK); }

// ---------------------------------------------------------------- IDirectSound8
M(ds_QueryInterface) { W32(ARG(2), ARG(0)); g_ds_refs++; RETN(3, DS_OK); }
M(ds_AddRef) { RETN(1, ++g_ds_refs); }
M(ds_Release) { RETN(1, g_ds_refs > 1 ? --g_ds_refs : 0); }
static DSBuf *new_buf() {
    DSBuf *b = new DSBuf();
    b->guest = com_new_object(g_vt_buf, 4);
    g_by_guest[b->guest] = b;
    g_bufs.push_back(b);
    return b;
}
M(ds_CreateSoundBuffer) {
    uint32_t d = ARG(1);
    uint32_t flags = R32(d + 4), bytes = R32(d + 8), wfx = R32(d + 16);
    LOCK;
    DSBuf *b = new_buf();
    b->flags = flags;
    b->data = new SharedMem();
    if (flags & 1) {   // primary buffer
        b->primary = true;
        b->channels = 2; b->bits = 16; b->align = 4; b->rate = b->base_rate = OUT_RATE;
    } else {
        if (!wfx) { destroy_buf(b); RETN(4, DSERR_INVALIDPARAM); }
        b->channels = R16(wfx + 2); b->rate = b->base_rate = R32(wfx + 4); b->align = R16(wfx + 12); b->bits = R16(wfx + 14);
        if (R16(wfx) != 1 || (b->bits != 8 && b->bits != 16) || b->channels < 1 || b->channels > 2)
            rtlog("dsound: unsupported format tag %u ch %u bits %u", R16(wfx), b->channels, b->bits);
        b->data->size = bytes;
        b->data->mem = gmalloc(bytes + 16, true);
        if (b->bits == 8) memset(H(b->data->mem), 0x80, bytes);
    }
    update_gains(b);
    W32(ARG(2), b->guest);
    RETN(4, DS_OK);
}
M(ds_GetCaps) {
    uint32_t p = ARG(1);
    uint32_t sz = R32(p);
    memset(H(p + 4), 0, sz > 4 ? sz - 4 : 0);
    W32(p + 4, 0x0F5F);              // DSCAPS: primary/secondary mono/stereo 8/16, continuous rate
    W32(p + 8, 100); W32(p + 12, 200000);
    W32(p + 16, 1);
    W32(p + 20, 256); W32(p + 24, 256); W32(p + 28, 256);
    W32(p + 32, 256); W32(p + 36, 256); W32(p + 40, 256);
    RETN(2, DS_OK);
}
M(ds_DuplicateSoundBuffer) {
    LOCK;
    DSBuf *src = buf_of(ARG(1));
    if (!src) RETN(3, DSERR_INVALIDPARAM);
    DSBuf *b = new_buf();
    uint32_t g = b->guest;
    *b = *src;
    b->guest = g; b->guest3d = 0; b->guest_notify = 0; b->refs = 1;
    b->playing = false; b->pos = 0;
    b->data->refs++;
    W32(ARG(2), b->guest);
    RETN(3, DS_OK);
}
M(ds_SetCooperativeLevel) { RETN(3, DS_OK); }
M(ds_Compact) { RETN(1, DS_OK); }
M(ds_GetSpeakerConfig) { W32(ARG(1), 4); RETN(2, DS_OK); }   // DSSPEAKER_STEREO
M(ds_SetSpeakerConfig) { RETN(2, DS_OK); }
M(ds_Initialize) { RETN(2, DS_OK); }
M(ds_VerifyCertification) { W32(ARG(1), 0); RETN(2, DS_OK); }

static void init_vtables() {
    if (g_vt_ds) return;
    HostFn ds[12] = {ds_QueryInterface, ds_AddRef, ds_Release, ds_CreateSoundBuffer, ds_GetCaps, ds_DuplicateSoundBuffer,
                     ds_SetCooperativeLevel, ds_Compact, ds_GetSpeakerConfig, ds_SetSpeakerConfig, ds_Initialize, ds_VerifyCertification};
    g_vt_ds = com_make_vtable("IDirectSound8", ds, 12);
    HostFn b[24] = {b_QueryInterface, b_AddRef, b_Release, b_GetCaps, b_GetCurrentPosition, b_GetFormat, b_GetVolume, b_GetPan,
                    b_GetFrequency, b_GetStatus, b_Initialize, b_Lock, b_Play, b_SetCurrentPosition, b_SetFormat, b_SetVolume,
                    b_SetPan, b_SetFrequency, b_Stop, b_Unlock, b_Restore, b_SetFX, b_AcquireResources, b_GetObjectInPath};
    g_vt_buf = com_make_vtable("IDirectSoundBuffer8", b, 24);
    HostFn b3[21] = {b_QueryInterface, b_AddRef, b_Release, b3_GetAllParameters, b3_GetConeAngles, b3_GetConeOrientation,
                     b3_GetConeOutsideVolume, b3_GetMaxDistance, b3_GetMinDistance, b3_GetMode, b3_GetPosition, b3_GetVelocity,
                     b3_SetAllParameters, b3_SetConeAngles, b3_SetConeOrientation, b3_SetConeOutsideVolume, b3_SetMaxDistance,
                     b3_SetMinDistance, b3_SetMode, b3_SetPosition, b3_SetVelocity};
    g_vt_3d = com_make_vtable("IDirectSound3DBuffer", b3, 21);
    HostFn l[18] = {b_QueryInterface, l_AddRef, l_Release, l_GetAllParameters, l_GetDistanceFactor, l_GetDopplerFactor,
                    l_GetOrientation, l_GetPosition, l_GetRolloffFactor, l_GetVelocity, l_SetAllParameters, l_SetDistanceFactor,
                    l_SetDopplerFactor, l_SetOrientation, l_SetPosition, l_SetRolloffFactor, l_SetVelocity, l_CommitDeferredSettings};
    g_vt_listener = com_make_vtable("IDirectSound3DListener", l, 18);
    HostFn n[4] = {b_QueryInterface, b_AddRef, b_Release, n_SetNotificationPositions};
    g_vt_notify = com_make_vtable("IDirectSoundNotify", n, 4);
}

static void api_DirectSoundCreate8(CPU *c) {
    init_vtables();
    if (!open_audio()) { W32(ARG(1), 0); RETN(3, 0x88780078); }   // DSERR_NODRIVER
    if (!g_ds_obj) g_ds_obj = com_new_object(g_vt_ds, 4);
    else g_ds_refs++;
    W32(ARG(1), g_ds_obj);
    rtlog("DirectSoundCreate8 -> %08x", g_ds_obj);
    RETN(3, DS_OK);
}
static ApiReg reg_dsc8("dsound.dll!#11", api_DirectSoundCreate8);
static ApiReg reg_dsc8n("dsound.dll!DirectSoundCreate8", api_DirectSoundCreate8);
