#include "shm.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ts {
namespace shm {

static Shared *g_shared = nullptr;
static bool g_isShared = false;
static std::once_flag g_once;

static void attach()
{
    const size_t sz = sizeof(Shared);
    void *p = nullptr;
#ifdef _WIN32
    char name[64];
    snprintf(name, sizeof name, "Local\\TwoStormsCLAP.%d", LAYOUT);
    HANDLE hm = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  (DWORD)((unsigned long long)sz >> 32), (DWORD)(sz & 0xffffffffu), name);
    if (hm) {
        p = MapViewOfFile(hm, FILE_MAP_ALL_ACCESS, 0, 0, sz);
        // the handle stays open for the life of the process (the block lives while any process has it)
        if (!p) CloseHandle(hm);
    }
#else
    char name[64];
    snprintf(name, sizeof name, "/TwoStormsCLAP.%d.%d", LAYOUT, (int)getuid());
    int fd = shm_open(name, O_RDWR | O_CREAT, 0600);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && (size_t)st.st_size < sz) {
            if (ftruncate(fd, (off_t)sz) != 0) { close(fd); fd = -1; }
        }
        if (fd >= 0) {
            void *m = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (m != MAP_FAILED) p = m;
        }
    }
#endif
    if (p) {
        g_isShared = true;
    } else {
        p = calloc(1, sz);
        g_isShared = false;
    }
    g_shared = (Shared *)p;
    Header &h = g_shared->h;
    // A fresh block is all zeros, which is every field's starting value. Fill in the header once.
    int64_t zero = 0;
    if (h.magic.compare_exchange_strong(zero, MAGIC)) {
        h.layout.store(LAYOUT);
        h.size.store((int64_t)sz);
        h.zoom.store(1);
    }
}

Shared *get()
{
    std::call_once(g_once, attach);
    return g_shared;
}

bool isShared()
{
    get();
    return g_isShared;
}

bool send(int s, int cmd, double x, double y, double z)
{
    if (s < 0 || s >= NSLOT) return false;
    Slot &sl = slot(s);
    // writers are main threads (possibly of several processes): a short spin lock
    for (int spin = 0;; ++spin) {
        int32_t exp = 0;
        if (sl.ringLock.compare_exchange_weak(exp, 1, std::memory_order_acquire)) break;
        if (spin > 100000) return false;
    }
    bool ok = false;
    int64_t w = sl.ringW.load(std::memory_order_relaxed), r = sl.ringR.load(std::memory_order_acquire);
    if (w - r < RINGN) {
        CmdEntry &e = sl.ring[w % RINGN];
        e.c.store(cmd, std::memory_order_relaxed);
        e.x.store(x, std::memory_order_relaxed);
        e.y.store(y, std::memory_order_relaxed);
        e.z.store(z, std::memory_order_relaxed);
        sl.ringW.store(w + 1, std::memory_order_release);
        ok = true;
    }
    sl.ringLock.store(0, std::memory_order_release);
    return ok;
}

static void putstr(char *dst, const char *src)
{
    size_t i = 0;
    if (src)
        for (; i < (size_t)NAMELEN - 1 && src[i]; ++i) dst[i] = src[i];
    for (; i < (size_t)NAMELEN; ++i) dst[i] = 0;
}

void writeNames(int s, int64_t id, int64_t order, int64_t flags, int64_t dstat, const char *tname,
                const char *prev, const char *dname)
{
    Slot &sl = slot(s);
    int64_t q = sl.nseq.load(std::memory_order_relaxed);
    sl.nseq.store(q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    putstr(sl.tname, tname);
    putstr(sl.prev, prev);
    putstr(sl.dname, dname);
    sl.norder.store(order, std::memory_order_relaxed);
    sl.nflags.store(flags, std::memory_order_relaxed);
    sl.dstat.store(dstat, std::memory_order_relaxed);
    sl.nid.store(id, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    sl.nseq.store(q + 2, std::memory_order_release);
}

bool readNames(int s, int64_t *id, char *tname, char *prev, char *dname)
{
    Slot &sl = slot(s);
    for (int tries = 0; tries < 8; ++tries) {
        int64_t a = sl.nseq.load(std::memory_order_acquire);
        if (a & 1) continue;
        if (tname) memcpy(tname, sl.tname, NAMELEN);
        if (prev) memcpy(prev, sl.prev, NAMELEN);
        if (dname) memcpy(dname, sl.dname, NAMELEN);
        if (id) *id = sl.nid.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sl.nseq.load(std::memory_order_relaxed) == a) {
            if (tname) tname[NAMELEN - 1] = 0;
            if (prev) prev[NAMELEN - 1] = 0;
            if (dname) dname[NAMELEN - 1] = 0;
            return true;
        }
    }
    return false;
}

} // namespace shm
} // namespace ts
