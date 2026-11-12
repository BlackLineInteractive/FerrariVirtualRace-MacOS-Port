// kernel32 / advapi32 / shell32 / shlwapi / ole32 / winmm replacements.
#include "rt.h"
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <ctime>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <fnmatch.h>
#include <strings.h>

CPU *new_cpu();
void bind_cpu(CPU *c);

static const char *GAME_WIN_DIR = "C:\\FVR";
static const char *USER_WIN_DIR = "C:\\USER";

// ======================================================================= errors
void set_last_error(uint32_t e) { CPU *c = cur_cpu(); if (c) W32(c->fs_base + 0x34, e); }
API(kernel32, GetLastError, 0) { RETN(0, R32(c->fs_base + 0x34)); }
API(kernel32, SetLastError, 1) { W32(c->fs_base + 0x34, ARG(0)); RETN(1, 0); }

// ======================================================================= paths
static std::string g_cwd = GAME_WIN_DIR;

static bool istarts(const std::string &s, const char *p) { return strncasecmp(s.c_str(), p, strlen(p)) == 0; }

// resolve each component case-insensitively under `root`
static std::string ci_resolve(const std::string &root, const std::string &rel) {
    std::string cur = root;
    size_t i = 0;
    while (i <= rel.size()) {
        size_t j = rel.find('/', i);
        if (j == std::string::npos) j = rel.size();
        std::string comp = rel.substr(i, j - i);
        i = j + 1;
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") { cur = cur.substr(0, cur.rfind('/')); continue; }
        std::string cand = cur + "/" + comp;
        struct stat st;
        if (stat(cand.c_str(), &st) != 0) {
            if (DIR *d = opendir(cur.c_str())) {
                while (dirent *e = readdir(d))
                    if (strcasecmp(e->d_name, comp.c_str()) == 0) { cand = cur + "/" + e->d_name; break; }
                closedir(d);
            }
        }
        cur = cand;
    }
    return cur;
}

static std::string norm_win(const char *p) {
    std::string s(p ? p : "");
    for (auto &ch : s) if (ch == '/') ch = '\\';
    if (s.size() >= 2 && s[1] == ':') return s;
    if (!s.empty() && s[0] == '\\') return "C:" + s;
    return g_cwd + "\\" + s;
}

// read path: overlay (user dir) first, then game dir. write path: always overlay for game files.
static std::string map_path(const char *winpath, bool for_write) {
    std::string w = norm_win(winpath);
    std::string rel;
    std::string root;
    bool game = false;
    if (istarts(w, GAME_WIN_DIR) && (w.size() == strlen(GAME_WIN_DIR) || w[strlen(GAME_WIN_DIR)] == '\\')) {
        rel = w.substr(strlen(GAME_WIN_DIR)); game = true;
    } else if (istarts(w, USER_WIN_DIR)) {
        rel = w.substr(strlen(USER_WIN_DIR)); root = g_user_dir + "/docs";
    } else {
        rel = w.substr(2); root = g_user_dir + "/drive_c";
    }
    for (auto &ch : rel) if (ch == '\\') ch = '/';
    if (game) {
        std::string ov = ci_resolve(g_user_dir + "/overlay", rel);
        struct stat st;
        if (for_write || stat(ov.c_str(), &st) == 0) {
            if (for_write) {
                std::string dir = ov.substr(0, ov.rfind('/'));
                std::string cmd = "mkdir -p '" + dir + "'";
                system(cmd.c_str());
                // copy-on-write: seed overlay with the original file
                std::string orig = ci_resolve(g_game_dir, rel);
                if (stat(ov.c_str(), &st) != 0 && stat(orig.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                    std::string cp = "cp '" + orig + "' '" + ov + "'";
                    system(cp.c_str());
                }
            }
            return ov;
        }
        return ci_resolve(g_game_dir, rel);
    }
    std::string r = ci_resolve(root, rel);
    if (for_write) {
        std::string cmd = "mkdir -p '" + r.substr(0, r.rfind('/')) + "'";
        system(cmd.c_str());
    }
    return r;
}

std::string host_path(const char *winpath) { return map_path(winpath, false); }

// ======================================================================= handles
enum HType { H_FILE, H_FIND, H_THREAD, H_STD, H_HEAP, H_OTHER };
struct Handle {
    HType t;
    int fd = -1;
    std::vector<std::string> find_names; size_t find_pos = 0; std::string find_dir;
    pthread_t thr{}; bool suspended = false; std::mutex m; std::condition_variable cv; CPU *cpu = nullptr;
    uint32_t start = 0, param = 0; bool done = false;
};
static std::mutex g_h_mutex;
static std::unordered_map<uint32_t, Handle *> g_handles;
static uint32_t g_next_handle = 0x100;

static uint32_t new_handle(Handle *h) {
    std::lock_guard<std::mutex> l(g_h_mutex);
    uint32_t v = g_next_handle; g_next_handle += 4;
    g_handles[v] = h;
    return v;
}
static Handle *get_handle(uint32_t v) {
    std::lock_guard<std::mutex> l(g_h_mutex);
    auto it = g_handles.find(v);
    return it == g_handles.end() ? nullptr : it->second;
}

static uint32_t h_stdin, h_stdout, h_stderr;

API(kernel32, CloseHandle, 1) {
    Handle *h = get_handle(ARG(0));
    if (h && h->t == H_FILE && h->fd >= 0) close(h->fd);
    if (h && h->t != H_THREAD && h->t != H_STD) {
        std::lock_guard<std::mutex> l(g_h_mutex);
        g_handles.erase(ARG(0));
        delete h;
    }
    RETN(1, 1);
}

// ======================================================================= files
static uint32_t open_file(const std::string &win, uint32_t access, uint32_t disp) {
    bool write = access & 0x40000000;
    std::string hp = map_path(win.c_str(), write);
    int flags = write ? ((access & 0x80000000) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
        case 1: flags |= O_CREAT | O_EXCL; break;   // CREATE_NEW
        case 2: flags |= O_CREAT | O_TRUNC; break;  // CREATE_ALWAYS
        case 3: break;                              // OPEN_EXISTING
        case 4: flags |= O_CREAT; break;            // OPEN_ALWAYS
        case 5: flags |= O_TRUNC; break;            // TRUNCATE_EXISTING
    }
    int fd = open(hp.c_str(), flags, 0644);
    if (g_trace_api) rtlog("  CreateFile %s -> %s fd=%d", win.c_str(), hp.c_str(), fd);
    if (fd < 0) { set_last_error(errno == ENOENT ? 2 : 5); return 0xFFFFFFFF; }
    struct stat st; fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) { close(fd); set_last_error(5); return 0xFFFFFFFF; }
    Handle *h = new Handle{H_FILE}; h->fd = fd;
    set_last_error(0);
    return new_handle(h);
}

API(kernel32, CreateFileA, 7) { RETN(7, open_file(GS(ARG(0)), ARG(1), ARG(4))); }
API(kernel32, CreateFileW, 7) {
    std::string s; for (const uint16_t *w = GW(ARG(0)); *w; w++) s += (char)(*w < 128 ? *w : '_');
    RETN(7, open_file(s, ARG(1), ARG(4)));
}

API(kernel32, ReadFile, 5) {
    Handle *h = get_handle(ARG(0));
    if (!h || h->fd < 0) { if (ARG(3)) W32(ARG(3), 0); RETN(5, 0); }
    ssize_t n = read(h->fd, H(ARG(1)), ARG(2));
    if (ARG(3)) W32(ARG(3), n < 0 ? 0 : (uint32_t)n);
    RETN(5, n >= 0);
}
API(kernel32, WriteFile, 5) {
    Handle *h = get_handle(ARG(0));
    if (!h) RETN(5, 0);
    ssize_t n;
    if (h->t == H_STD) n = write(2, H(ARG(1)), ARG(2));
    else n = write(h->fd, H(ARG(1)), ARG(2));
    if (ARG(3)) W32(ARG(3), n < 0 ? 0 : (uint32_t)n);
    RETN(5, n >= 0);
}
API(kernel32, SetFilePointer, 4) {
    Handle *h = get_handle(ARG(0));
    if (!h || h->fd < 0) RETN(4, 0xFFFFFFFF);
    int64_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (int64_t)(((uint64_t)R32(ARG(2)) << 32) | ARG(1));
    off_t r = lseek(h->fd, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    if (ARG(2)) W32(ARG(2), (uint32_t)((uint64_t)r >> 32));
    RETN(4, (uint32_t)r);
}
API(kernel32, GetFileSize, 2) {
    Handle *h = get_handle(ARG(0));
    struct stat st;
    if (!h || h->fd < 0 || fstat(h->fd, &st)) RETN(2, 0xFFFFFFFF);
    if (ARG(1)) W32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    RETN(2, (uint32_t)st.st_size);
}
API(kernel32, FlushFileBuffers, 1) { RETN(1, 1); }
API(kernel32, SetEndOfFile, 1) {
    Handle *h = get_handle(ARG(0));
    if (!h || h->fd < 0) RETN(1, 0);
    ftruncate(h->fd, lseek(h->fd, 0, SEEK_CUR));
    RETN(1, 1);
}
API(kernel32, GetFileType, 1) {
    Handle *h = get_handle(ARG(0));
    RETN(1, !h ? 0 : h->t == H_STD ? 2 : 1);
}
API(kernel32, DeleteFileA, 1) { RETN(1, unlink(map_path(GS(ARG(0)), true).c_str()) == 0); }
API(kernel32, CreateDirectoryA, 2) {
    std::string p = map_path(GS(ARG(0)), true);
    int r = mkdir(p.c_str(), 0755);
    if (r) set_last_error(errno == EEXIST ? 183 : 3);
    RETN(2, r == 0);
}
API(kernel32, GetFileAttributesA, 1) {
    struct stat st;
    std::string p = host_path(GS(ARG(0)));
    if (stat(p.c_str(), &st)) { set_last_error(2); RETN(1, 0xFFFFFFFF); }
    RETN(1, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
}

static void fill_find_data(uint32_t fd, const std::string &dir, const std::string &name) {
    memset(H(fd), 0, 320);
    struct stat st{};
    stat((dir + "/" + name).c_str(), &st);
    W32(fd, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
    for (int i = 0; i < 3; i++) { W32(fd + 4 + 8 * i, (uint32_t)ft); W32(fd + 8 + 8 * i, (uint32_t)(ft >> 32)); }
    W32(fd + 28, (uint32_t)((uint64_t)st.st_size >> 32));
    W32(fd + 32, (uint32_t)st.st_size);
    gstrcpy(fd + 44, name.c_str(), 260);
}

API(kernel32, FindFirstFileA, 2) {
    std::string w = norm_win(GS(ARG(0)));
    size_t bs = w.rfind('\\');
    std::string dirw = w.substr(0, bs), pat = w.substr(bs + 1);
    Handle *h = new Handle{H_FIND};
    // merge listing of game dir and overlay
    std::vector<std::string> dirs = {host_path(dirw.c_str())};
    if (istarts(dirw, GAME_WIN_DIR)) {
        std::string rel = dirw.substr(strlen(GAME_WIN_DIR));
        for (auto &ch : rel) if (ch == '\\') ch = '/';
        dirs.push_back(ci_resolve(g_user_dir + "/overlay", rel));
        dirs.push_back(ci_resolve(g_game_dir, rel));
    }
    for (auto &d : dirs) {
        if (DIR *dp = opendir(d.c_str())) {
            while (dirent *e = readdir(dp)) {
                if (fnmatch(pat.c_str(), e->d_name, FNM_CASEFOLD) != 0 && pat != "*.*") continue;
                bool dup = false;
                for (auto &n : h->find_names) if (strcasecmp(n.c_str(), e->d_name) == 0) dup = true;
                if (!dup) { h->find_names.push_back(e->d_name); if (h->find_dir.empty()) h->find_dir = d; }
            }
            closedir(dp);
            if (h->find_dir.empty()) h->find_dir = d;
        }
    }
    if (h->find_names.empty()) { delete h; set_last_error(2); RETN(2, 0xFFFFFFFF); }
    fill_find_data(ARG(1), h->find_dir, h->find_names[0]);
    h->find_pos = 1;
    RETN(2, new_handle(h));
}
API(kernel32, FindNextFileA, 2) {
    Handle *h = get_handle(ARG(0));
    if (!h || h->find_pos >= h->find_names.size()) { set_last_error(18); RETN(2, 0); }
    fill_find_data(ARG(1), h->find_dir, h->find_names[h->find_pos++]);
    RETN(2, 1);
}
API(kernel32, FindClose, 1) {
    Handle *h = get_handle(ARG(0));
    if (h) { std::lock_guard<std::mutex> l(g_h_mutex); g_handles.erase(ARG(0)); delete h; }
    RETN(1, 1);
}

API(kernel32, GetCurrentDirectoryA, 2) {
    if (ARG(0) > g_cwd.size()) gstrcpy(ARG(1), g_cwd.c_str(), ARG(0));
    RETN(2, g_cwd.size() + (ARG(0) > g_cwd.size() ? 0 : 1));
}
API(kernel32, SetCurrentDirectoryA, 1) {
    std::string w = norm_win(GS(ARG(0)));
    while (w.size() > 3 && w.back() == '\\') w.pop_back();
    g_cwd = w;
    RETN(1, 1);
}
API(kernel32, GetModuleFileNameA, 3) {
    std::string p = std::string(GAME_WIN_DIR) + "\\FerrariVR_Hi.exe";
    gstrcpy(ARG(1), p.c_str(), ARG(2));
    RETN(3, std::min<uint32_t>(p.size(), ARG(2) - 1));
}
API(shlwapi, PathRemoveFileSpecA, 1) {
    char *s = (char *)H(ARG(0));
    char *b = strrchr(s, '\\');
    if (b) { if (b == s + 2 && s[1] == ':') b[1] = 0; else *b = 0; RETN(1, 1); }
    RETN(1, 0);
}
API(shell32, SHGetFolderPathA, 5) {
    gstrcpy(ARG(4), USER_WIN_DIR, 260);
    std::string d = g_user_dir + "/docs";
    mkdir(d.c_str(), 0755);
    RETN(5, 0);
}

// ======================================================================= std handles / console
API(kernel32, GetStdHandle, 1) {
    int32_t n = (int32_t)ARG(0);
    RETN(1, n == -10 ? h_stdin : n == -11 ? h_stdout : h_stderr);
}
API(kernel32, SetStdHandle, 2) { RETN(2, 1); }
API(kernel32, GetConsoleMode, 2) { RETN(2, 0); }
API(kernel32, GetConsoleCP, 0) { RETN(0, 1252); }
API(kernel32, GetConsoleOutputCP, 0) { RETN(0, 1252); }
API(kernel32, WriteConsoleA, 5) { fwrite(H(ARG(1)), 1, ARG(2), stderr); if (ARG(3)) W32(ARG(3), ARG(2)); RETN(5, 1); }
API(kernel32, WriteConsoleW, 5) {
    for (uint32_t i = 0; i < ARG(2); i++) fputc((char)R16(ARG(1) + 2 * i), stderr);
    if (ARG(3)) W32(ARG(3), ARG(2));
    RETN(5, 1);
}
API(kernel32, SetHandleCount, 1) { RETN(1, ARG(0)); }

// ======================================================================= heap / memory
API(kernel32, GetProcessHeap, 0) { RETN(0, 0x00F00000); }
API(kernel32, HeapCreate, 3) { RETN(3, 0x00F00000); }
API(kernel32, HeapDestroy, 1) { RETN(1, 1); }
API(kernel32, HeapAlloc, 3) { RETN(3, gmalloc(ARG(2), ARG(1) & 8)); }
API(kernel32, HeapFree, 3) { gfree(ARG(2)); RETN(3, 1); }
API(kernel32, HeapSize, 3) { RETN(3, gsize(ARG(2))); }
API(kernel32, HeapReAlloc, 4) {
    uint32_t old = gsize(ARG(2));
    if ((ARG(1) & 0x10) /*IN_PLACE_ONLY*/ && ARG(3) > old) RETN(4, 0);
    uint32_t p = grealloc(ARG(2), ARG(3));
    if (p && (ARG(1) & 8) && ARG(3) > old) memset(H(p + old), 0, ARG(3) - old);
    RETN(4, p);
}
API(kernel32, VirtualAlloc, 4) { RETN(4, vm_alloc(ARG(0), ARG(1))); }
API(kernel32, VirtualFree, 3) { if (ARG(2) & 0x8000) vm_free(ARG(0)); RETN(3, 1); }
API(kernel32, GlobalMemoryStatus, 1) {
    uint32_t p = ARG(0);
    W32(p, 32); W32(p + 4, 30); W32(p + 8, 0x7FFFFFFF); W32(p + 12, 0x60000000);
    W32(p + 16, 0x7FFFFFFF); W32(p + 20, 0x60000000); W32(p + 24, 0x7FFE0000); W32(p + 28, 0x60000000);
    RETN(1, 0);
}
API(kernel32, GlobalMemoryStatusEx, 1) {
    uint32_t p = ARG(0);
    W32(p + 4, 30);
    W64(p + 8, 4ull << 30); W64(p + 16, 3ull << 30); W64(p + 24, 8ull << 30); W64(p + 32, 6ull << 30);
    W64(p + 40, 0x7FFE0000ull); W64(p + 48, 0x60000000ull); W64(p + 56, 0);
    RETN(1, 1);
}

// ======================================================================= TLS / sync
static std::mutex g_tls_mutex;
static uint64_t g_tls_used;
API(kernel32, TlsAlloc, 0) {
    std::lock_guard<std::mutex> l(g_tls_mutex);
    for (int i = 0; i < 64; i++) if (!(g_tls_used & (1ull << i))) { g_tls_used |= 1ull << i; RETN(0, i); }
    RETN(0, 0xFFFFFFFF);
}
API(kernel32, TlsFree, 1) { std::lock_guard<std::mutex> l(g_tls_mutex); g_tls_used &= ~(1ull << ARG(0)); RETN(1, 1); }
API(kernel32, TlsGetValue, 1) { W32(c->fs_base + 0x34, 0); RETN(1, R32(c->fs_base + 0xE10 + 4 * ARG(0))); }
API(kernel32, TlsSetValue, 2) { W32(c->fs_base + 0xE10 + 4 * ARG(0), ARG(1)); RETN(2, 1); }

static std::mutex g_cs_mutex;
static std::unordered_map<uint32_t, std::recursive_mutex *> g_cs;
static std::recursive_mutex *cs_get(uint32_t a) {
    std::lock_guard<std::mutex> l(g_cs_mutex);
    auto &m = g_cs[a];
    if (!m) m = new std::recursive_mutex;
    return m;
}
API(kernel32, InitializeCriticalSection, 1) { memset(H(ARG(0)), 0, 24); W32(ARG(0) + 4, 0xFFFFFFFF); cs_get(ARG(0)); RETN(1, 0); }
API(kernel32, DeleteCriticalSection, 1) { RETN(1, 0); }
API(kernel32, EnterCriticalSection, 1) { cs_get(ARG(0))->lock(); RETN(1, 0); }
API(kernel32, LeaveCriticalSection, 1) { cs_get(ARG(0))->unlock(); RETN(1, 0); }
API(kernel32, InterlockedIncrement, 1) { RETN(1, __atomic_add_fetch((uint32_t *)H(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
API(kernel32, InterlockedDecrement, 1) { RETN(1, __atomic_sub_fetch((uint32_t *)H(ARG(0)), 1, __ATOMIC_SEQ_CST)); }

// ======================================================================= threads
static void *thread_main(void *arg) {
    Handle *h = (Handle *)arg;
    {
        std::unique_lock<std::mutex> l(h->m);
        h->cv.wait(l, [h] { return !h->suspended; });
    }
    bind_cpu(h->cpu);
    uint32_t a = h->param;
    guest_call(h->cpu, h->start, 1, &a);
    h->done = true;
    return nullptr;
}
API(kernel32, CreateThread, 6) {
    Handle *h = new Handle{H_THREAD};
    h->cpu = new_cpu();
    h->start = ARG(2); h->param = ARG(3);
    h->suspended = ARG(4) & 4;
    uint32_t hv = new_handle(h);
    if (ARG(5)) W32(ARG(5), h->cpu->tid);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8 << 20);
    pthread_create(&h->thr, &at, thread_main, h);
    RETN(6, hv);
}
API(kernel32, ResumeThread, 1) {
    Handle *h = get_handle(ARG(0));
    if (h && h->t == H_THREAD) { std::lock_guard<std::mutex> l(h->m); h->suspended = false; h->cv.notify_all(); }
    RETN(1, 1);
}
API(kernel32, SetThreadPriority, 2) { RETN(2, 1); }
API(kernel32, Sleep, 1) { usleep(ARG(0) ? ARG(0) * 1000 : 0); if (!ARG(0)) sched_yield(); RETN(1, 0); }
API(kernel32, GetCurrentThreadId, 0) { RETN(0, c->tid); }
API(kernel32, GetCurrentProcessId, 0) { RETN(0, 0x1234); }
API(kernel32, GetCurrentProcess, 0) { RETN(0, 0xFFFFFFFF); }
API(kernel32, ExitProcess, 1) { rtlog("ExitProcess(%u)", ARG(0)); fflush(nullptr); _exit((int)ARG(0)); }
API(kernel32, TerminateProcess, 2) { rtlog("TerminateProcess(%u)", ARG(1)); fflush(nullptr); _exit((int)ARG(1)); }
API(kernel32, CreateProcessA, 10) { rtlog("CreateProcessA(%s, %s) not supported", GS(ARG(0)) ? GS(ARG(0)) : "", GS(ARG(1)) ? GS(ARG(1)) : ""); set_last_error(2); RETN(10, 0); }

// ======================================================================= process info
static uint32_t g_cmdline, g_env_a, g_env_w;
API(kernel32, GetCommandLineA, 0) { RETN(0, g_cmdline); }
API(kernel32, GetEnvironmentStrings, 0) { RETN(0, g_env_a); }
API(kernel32, GetEnvironmentStringsW, 0) { RETN(0, g_env_w); }
API(kernel32, FreeEnvironmentStringsA, 1) { RETN(1, 1); }
API(kernel32, FreeEnvironmentStringsW, 1) { RETN(1, 1); }
API(kernel32, GetStartupInfoA, 1) { memset(H(ARG(0)), 0, 68); W32(ARG(0), 68); RETN(1, 0); }
API(kernel32, GetVersionExA, 1) {
    uint32_t p = ARG(0);
    W32(p + 4, 5); W32(p + 8, 1); W32(p + 12, 2600); W32(p + 16, 2);
    gstrcpy(p + 20, "Service Pack 3", 128);
    if (R32(p) >= 156) { W16(p + 148, 3); W16(p + 150, 0); W16(p + 152, 0x100); G[p + 154] = 1; }
    RETN(1, 1);
}
API(kernel32, IsDebuggerPresent, 0) { RETN(0, 0); }
static uint32_t g_unhandled_filter;
API(kernel32, SetUnhandledExceptionFilter, 1) { uint32_t o = g_unhandled_filter; g_unhandled_filter = ARG(0); RETN(1, o); }
API(kernel32, UnhandledExceptionFilter, 1) { rtlog("UnhandledExceptionFilter called"); RETN(1, 1); }
API(kernel32, GetComputerNameA, 2) { gstrcpy(ARG(0), "MAC", R32(ARG(1))); W32(ARG(1), 3); RETN(2, 1); }
API(advapi32, GetUserNameA, 2) { gstrcpy(ARG(0), "Player", R32(ARG(1))); W32(ARG(1), 7); RETN(2, 1); }

// RaiseException: C++ throw (0xE06D7363) etc. Full SEH dispatch is implemented in seh.cpp.
void seh_raise(CPU *c, uint32_t code, uint32_t flags, uint32_t nargs, uint32_t args);
API(kernel32, RaiseException, 4) { seh_raise(c, ARG(0), ARG(1), ARG(2), ARG(3)); RETN(4, 0); }
void seh_rtl_unwind(CPU *c);
API(kernel32, RtlUnwind, 4) { seh_rtl_unwind(c); }

// ======================================================================= modules
struct Mod { const char *name; uint32_t h; };
static const Mod g_mods[] = {
    {"kernel32.dll", 0x7C800000}, {"user32.dll", 0x7E410000}, {"gdi32.dll", 0x77F10000},
    {"advapi32.dll", 0x77DD0000}, {"ole32.dll", 0x774E0000}, {"d3d9.dll", 0x4FDD0000},
    {"d3dx9_37.dll", 0x4FE00000}, {"dinput8.dll", 0x4FE10000}, {"dsound.dll", 0x73F10000},
    {"ddraw.dll", 0x73760000}, {"winmm.dll", 0x76B40000}, {"shell32.dll", 0x7C9C0000},
    {"shlwapi.dll", 0x77F60000}, {"mscoree.dll", 0x79000000},
};
static const char *mod_name(uint32_t h) {
    for (auto &m : g_mods) if (m.h == h) return m.name;
    return nullptr;
}
static uint32_t mod_handle(const char *n) {
    if (!n) return GUEST_IMAGE_BASE;
    std::string s(n);
    for (auto &ch : s) ch = (char)tolower(ch);
    if (s.find('.') == std::string::npos) s += ".dll";
    size_t b = s.find_last_of("\\/");
    if (b != std::string::npos) s = s.substr(b + 1);
    for (auto &m : g_mods) if (s == m.name) return m.h;
    return 0;
}
API(kernel32, GetModuleHandleA, 1) {
    uint32_t h = mod_handle(GS(ARG(0)));
    if (h == mod_handle("mscoree.dll") && ARG(0)) h = 0;  // CRT probes for the CLR: say "not loaded"
    RETN(1, h);
}
API(kernel32, LoadLibraryA, 1) {
    uint32_t h = mod_handle(GS(ARG(0)));
    if (!h) { rtlog("LoadLibraryA(%s): not available", GS(ARG(0))); set_last_error(126); }
    RETN(1, h);
}
static std::mutex g_gpa_mutex;
static std::map<std::string, uint32_t> g_gpa_cache;
API(kernel32, GetProcAddress, 2) {
    const char *mn = mod_name(ARG(0));
    if (!mn || ARG(1) < 0x10000) RETN(2, 0);
    std::string key = std::string(mn) + "!" + GS(ARG(1));
    HostFn fn = lookup_api(key.c_str());
    if (!fn) { if (g_trace_api) rtlog("  GetProcAddress(%s) -> NULL", key.c_str()); RETN(2, 0); }
    std::lock_guard<std::mutex> l(g_gpa_mutex);
    auto &t = g_gpa_cache[key];
    if (!t) t = thunk_for(strdup(key.c_str()), fn);
    RETN(2, t);
}
// optional APIs the MSVC 2005 CRT looks up dynamically
API(kernel32, InitializeCriticalSectionAndSpinCount, 2) { cs_get(ARG(0)); memset(H(ARG(0)), 0, 24); RETN(2, 1); }
API(kernel32, EncodePointer, 1) { RETN(1, ARG(0)); }
API(kernel32, DecodePointer, 1) { RETN(1, ARG(0)); }
API(kernel32, IsProcessorFeaturePresent, 1) { RETN(1, ARG(0) == 6 || ARG(0) == 10 || ARG(0) == 3); }

// ======================================================================= locale / code pages
API(kernel32, GetACP, 0) { RETN(0, 1252); }
API(kernel32, GetOEMCP, 0) { RETN(0, 437); }
API(kernel32, IsValidCodePage, 1) { RETN(1, ARG(0) == 1252 || ARG(0) == 437 || ARG(0) == 65001); }
API(kernel32, GetCPInfo, 2) {
    memset(H(ARG(1)), 0, 20);
    W32(ARG(1), 1); G[ARG(1) + 4] = '?';
    RETN(2, 1);
}
API(kernel32, GetUserDefaultLCID, 0) { RETN(0, 0x409); }
API(kernel32, IsValidLocale, 2) { RETN(2, 1); }
API(kernel32, EnumSystemLocalesA, 2) { RETN(2, 1); }
static const char *locale_str(uint32_t type) {
    switch (type & 0xFFFF) {
        case 0x1004: return "1252";      // IDEFAULTANSICODEPAGE
        case 0x000B: return "437";       // IDEFAULTCODEPAGE
        case 0x0001: return "0409";      // ILANGUAGE
        case 0x0059: return "en";        // SISO639LANGNAME
        case 0x005A: return "US";        // SISO3166CTRYNAME
        case 0x0003: return "ENU";       // SABBREVLANGNAME
        case 0x0007: return "USA";       // SABBREVCTRYNAME
        case 0x000E: return ".";         // SDECIMAL
        case 0x000F: return ",";         // STHOUSAND
        case 0x0010: return "3;0";       // SGROUPING
        case 0x0014: return "$";
        default: return "";
    }
}
API(kernel32, GetLocaleInfoA, 4) {
    const char *s = locale_str(ARG(1));
    uint32_t n = strlen(s) + 1;
    if (ARG(3) == 0) RETN(4, n);
    gstrcpy(ARG(2), s, ARG(3));
    RETN(4, n);
}
API(kernel32, GetLocaleInfoW, 4) {
    const char *s = locale_str(ARG(1));
    uint32_t n = strlen(s) + 1;
    if (ARG(3) == 0) RETN(4, n);
    for (uint32_t i = 0; i < n && i < ARG(3); i++) W16(ARG(2) + 2 * i, (uint8_t)s[i]);
    RETN(4, n);
}
API(kernel32, MultiByteToWideChar, 6) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3);
    if (n < 0) n = strlen(GS(src)) + 1;
    if (ARG(5) == 0) RETN(6, n);
    int32_t m = n < (int32_t)ARG(5) ? n : (int32_t)ARG(5);
    for (int32_t i = 0; i < m; i++) W16(ARG(4) + 2 * i, R8(src + i));
    RETN(6, m);
}
API(kernel32, WideCharToMultiByte, 8) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3);
    if (n < 0) { n = 0; while (R16(src + 2 * n)) n++; n++; }
    if (ARG(5) == 0) RETN(8, n);
    int32_t m = n < (int32_t)ARG(5) ? n : (int32_t)ARG(5);
    for (int32_t i = 0; i < m; i++) { uint16_t w = R16(src + 2 * i); W8(ARG(4) + i, w < 256 ? (uint8_t)w : '?'); }
    if (ARG(7)) W32(ARG(7), 0);
    RETN(8, m);
}
static uint16_t ctype1(uint32_t ch) {
    uint16_t t = 0;
    if (ch < 128) {
        if (isupper(ch)) t |= 1; if (islower(ch)) t |= 2; if (isdigit(ch)) t |= 4; if (isspace(ch)) t |= 8;
        if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ' || ch == '\t') t |= 0x40;
        if (isxdigit(ch)) t |= 0x80; if (isalpha(ch)) t |= 0x100;
    } else if (ch >= 0xC0 && ch != 0xD7 && ch != 0xF7) t = 0x100 | (ch < 0xE0 ? 1 : 2);
    else if (ch == 0xA0) t = 0x48;
    else t = 0x10;
    return t;
}
API(kernel32, GetStringTypeA, 5) {
    int32_t n = (int32_t)ARG(3); if (n < 0) n = strlen(GS(ARG(2))) + 1;
    for (int32_t i = 0; i < n; i++) W16(ARG(4) + 2 * i, ctype1(R8(ARG(2) + i)));
    RETN(5, 1);
}
API(kernel32, GetStringTypeW, 4) {
    int32_t n = (int32_t)ARG(2); if (n < 0) { n = 0; while (R16(ARG(1) + 2 * n)) n++; n++; }
    for (int32_t i = 0; i < n; i++) W16(ARG(3) + 2 * i, ctype1(R16(ARG(1) + 2 * i)));
    RETN(4, 1);
}
static uint32_t map_case(uint32_t flags, uint32_t ch) {
    if (flags & 0x100) return (ch < 128) ? tolower(ch) : (ch >= 0xC0 && ch < 0xDF && ch != 0xD7) ? ch + 32 : ch;
    if (flags & 0x200) return (ch < 128) ? toupper(ch) : (ch >= 0xE0 && ch < 0xFF && ch != 0xF7) ? ch - 32 : ch;
    return ch;
}
API(kernel32, LCMapStringA, 6) {
    int32_t n = (int32_t)ARG(3); if (n < 0) n = strlen(GS(ARG(2))) + 1;
    if (ARG(5) == 0) RETN(6, n);
    for (int32_t i = 0; i < n && i < (int32_t)ARG(5); i++) W8(ARG(4) + i, map_case(ARG(1), R8(ARG(2) + i)));
    RETN(6, n);
}
API(kernel32, LCMapStringW, 6) {
    int32_t n = (int32_t)ARG(3); if (n < 0) { n = 0; while (R16(ARG(2) + 2 * n)) n++; n++; }
    if (ARG(5) == 0) RETN(6, n);
    for (int32_t i = 0; i < n && i < (int32_t)ARG(5); i++) W16(ARG(4) + 2 * i, map_case(ARG(1), R16(ARG(2) + 2 * i)));
    RETN(6, n);
}
static uint32_t cmp_result(int r) { return r < 0 ? 1 : r > 0 ? 3 : 2; }
API(kernel32, CompareStringA, 6) {
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    std::string a = n1 < 0 ? GS(ARG(2)) : std::string(GS(ARG(2)), n1);
    std::string b = n2 < 0 ? GS(ARG(4)) : std::string(GS(ARG(4)), n2);
    RETN(6, cmp_result((ARG(1) & 1) ? strcasecmp(a.c_str(), b.c_str()) : strcmp(a.c_str(), b.c_str())));
}
API(kernel32, CompareStringW, 6) {
    auto rd = [&](uint32_t p, int32_t n) { std::string s; for (int32_t i = 0; n < 0 ? R16(p + 2 * i) : i < n; i++) s += (char)R16(p + 2 * i); return s; };
    std::string a = rd(ARG(2), (int32_t)ARG(3)), b = rd(ARG(4), (int32_t)ARG(5));
    RETN(6, cmp_result((ARG(1) & 1) ? strcasecmp(a.c_str(), b.c_str()) : strcmp(a.c_str(), b.c_str())));
}

// ======================================================================= time
static uint64_t now_ns() {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
API(kernel32, GetTickCount, 0) { RETN(0, (uint32_t)(now_ns() / 1000000)); }
API(winmm, timeGetTime, 0) { RETN(0, (uint32_t)(now_ns() / 1000000)); }
API(winmm, timeBeginPeriod, 1) { RETN(1, 0); }
API(winmm, timeEndPeriod, 1) { RETN(1, 0); }
API(kernel32, QueryPerformanceFrequency, 1) { W64(ARG(0), 1000000000ull); RETN(1, 1); }
API(kernel32, QueryPerformanceCounter, 1) { W64(ARG(0), now_ns()); RETN(1, 1); }
API(kernel32, GetSystemTimeAsFileTime, 1) {
    struct timeval tv; gettimeofday(&tv, nullptr);
    uint64_t ft = ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + tv.tv_usec * 10;
    W64(ARG(0), ft);
    RETN(1, 0);
}
API(kernel32, GetLocalTime, 1) {
    struct timeval tv; gettimeofday(&tv, nullptr);
    struct tm t; localtime_r(&tv.tv_sec, &t);
    uint32_t p = ARG(0);
    W16(p, t.tm_year + 1900); W16(p + 2, t.tm_mon + 1); W16(p + 4, t.tm_wday); W16(p + 6, t.tm_mday);
    W16(p + 8, t.tm_hour); W16(p + 10, t.tm_min); W16(p + 12, t.tm_sec); W16(p + 14, tv.tv_usec / 1000);
    RETN(1, 0);
}
API(kernel32, CompareFileTime, 2) {
    uint64_t a = R64(ARG(0)), b = R64(ARG(1));
    RETN(2, a < b ? 0xFFFFFFFF : a > b ? 1 : 0);
}

// ======================================================================= INI files
struct Ini { std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> secs; };
static std::mutex g_ini_mutex;

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::string ini_path(uint32_t file) {
    std::string f = GS(file);
    if (f.find('\\') == std::string::npos && f.find(':') == std::string::npos) f = "C:\\WINDOWS\\" + f;
    return f;
}
static Ini ini_load(const std::string &win) {
    Ini ini;
    FILE *f = fopen(host_path(win.c_str()).c_str(), "rb");
    if (!f) return ini;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == ';') continue;
        if (l[0] == '[') { size_t e = l.find(']'); ini.secs.push_back({l.substr(1, e - 1), {}}); continue; }
        size_t eq = l.find('=');
        if (eq == std::string::npos || ini.secs.empty()) continue;
        std::string v = trim(l.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        ini.secs.back().second.push_back({trim(l.substr(0, eq)), v});
    }
    fclose(f);
    return ini;
}
static const std::string *ini_get(const Ini &ini, const char *sec, const char *key) {
    for (auto &s : ini.secs)
        if (strcasecmp(s.first.c_str(), sec) == 0)
            for (auto &kv : s.second)
                if (strcasecmp(kv.first.c_str(), key) == 0) return &kv.second;
    return nullptr;
}
static uint32_t put_multisz(uint32_t buf, uint32_t size, const std::vector<std::string> &items) {
    uint32_t o = 0;
    if (size < 2) return 0;
    for (auto &s : items) {
        if (o + s.size() + 2 > size) break;
        memcpy(H(buf + o), s.c_str(), s.size() + 1);
        o += s.size() + 1;
    }
    W8(buf + o, 0);
    return o;
}
API(kernel32, GetPrivateProfileStringA, 6) {
    std::lock_guard<std::mutex> l(g_ini_mutex);
    Ini ini = ini_load(ini_path(ARG(5)));
    uint32_t buf = ARG(3), size = ARG(4);
    if (!ARG(0)) { std::vector<std::string> v; for (auto &s : ini.secs) v.push_back(s.first); RETN(6, put_multisz(buf, size, v)); }
    if (!ARG(1)) {
        std::vector<std::string> v;
        for (auto &s : ini.secs) if (strcasecmp(s.first.c_str(), GS(ARG(0))) == 0) for (auto &kv : s.second) v.push_back(kv.first);
        RETN(6, put_multisz(buf, size, v));
    }
    const std::string *v = ini_get(ini, GS(ARG(0)), GS(ARG(1)));
    std::string r = v ? *v : (ARG(2) ? GS(ARG(2)) : "");
    if (g_trace_api) rtlog("  ini %s [%s] %s = %s%s", GS(ARG(5)), GS(ARG(0)), GS(ARG(1)), r.c_str(), v ? "" : " (default)");
    if (size == 0) RETN(6, 0);
    gstrcpy(buf, r.c_str(), size);
    RETN(6, std::min<uint32_t>(r.size(), size - 1));
}
API(kernel32, GetPrivateProfileIntA, 4) {
    std::lock_guard<std::mutex> l(g_ini_mutex);
    Ini ini = ini_load(ini_path(ARG(3)));
    const std::string *v = ini_get(ini, GS(ARG(0)), GS(ARG(1)));
    if (g_trace_api) rtlog("  ini %s [%s] %s = %s (default %d)", GS(ARG(3)), GS(ARG(0)), GS(ARG(1)), v ? v->c_str() : "<none>", (int)ARG(2));
    RETN(4, v ? (uint32_t)strtol(v->c_str(), nullptr, 10) : ARG(2));
}
API(kernel32, GetPrivateProfileSectionNamesA, 3) {
    std::lock_guard<std::mutex> l(g_ini_mutex);
    Ini ini = ini_load(ini_path(ARG(2)));
    std::vector<std::string> v; for (auto &s : ini.secs) v.push_back(s.first);
    RETN(3, put_multisz(ARG(0), ARG(1), v));
}
API(kernel32, WritePrivateProfileStringA, 4) {
    std::lock_guard<std::mutex> l(g_ini_mutex);
    std::string win = ini_path(ARG(3));
    Ini ini = ini_load(win);
    const char *sec = GS(ARG(0)), *key = GS(ARG(1)), *val = GS(ARG(2));
    auto *s = (decltype(&ini.secs[0]))nullptr;
    for (auto &x : ini.secs) if (strcasecmp(x.first.c_str(), sec) == 0) s = &x;
    if (!key) { if (s) ini.secs.erase(ini.secs.begin() + (s - &ini.secs[0])); }
    else {
        if (!s) { ini.secs.push_back({sec, {}}); s = &ini.secs.back(); }
        bool found = false;
        for (size_t i = 0; i < s->second.size(); i++)
            if (strcasecmp(s->second[i].first.c_str(), key) == 0) {
                if (val) s->second[i].second = val; else s->second.erase(s->second.begin() + i);
                found = true; break;
            }
        if (!found && val) s->second.push_back({key, val});
    }
    FILE *f = fopen(map_path(win.c_str(), true).c_str(), "wb");
    if (!f) RETN(4, 0);
    for (auto &x : ini.secs) {
        fprintf(f, "[%s]\r\n", x.first.c_str());
        for (auto &kv : x.second) fprintf(f, "%s=%s\r\n", kv.first.c_str(), kv.second.c_str());
        fprintf(f, "\r\n");
    }
    fclose(f);
    RETN(4, 1);
}

// ======================================================================= ole32
API(ole32, CoInitialize, 1) { RETN(1, 0); }
API(ole32, CoUninitialize, 0) { RETN(0, 0); }
API(ole32, CoCreateInstance, 5) {
    uint32_t g = ARG(0);
    rtlog("CoCreateInstance {%08x-%04x-%04x-...} not available", R32(g), R16(g + 4), R16(g + 6));
    W32(ARG(4), 0);
    RETN(5, 0x80040154);
}

// ======================================================================= init
void win32_init() {
    h_stdin = new_handle(new Handle{H_STD});
    h_stdout = new_handle(new Handle{H_STD});
    h_stderr = new_handle(new Handle{H_STD});
    g_cmdline = gstrdup("\"C:\\FVR\\FerrariVR_Hi.exe\"");
    g_env_a = gmalloc(64, true);
    gstrcpy(g_env_a, "PATH=C:\\WINDOWS", 64);
    g_env_w = gmalloc(128, true);
    const char *e = "PATH=C:\\WINDOWS";
    for (int i = 0; e[i]; i++) W16(g_env_w + 2 * i, e[i]);
}
