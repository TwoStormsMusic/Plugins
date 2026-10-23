#include "reaper_bridge.h"
#include "shm.h"
#include <clap/clap.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ts {

namespace {
struct MediaTrack;
struct MediaItem_Take;
struct ReaProject;
struct RGUID { uint32_t a; uint16_t b, c; uint8_t d[8]; };
struct reaper_plugin_info_t {
    int caller_version;
    void *hwnd_main;
    int (*Register)(const char *name, void *infostruct);
    void *(*GetFunc)(const char *name);
};
} // namespace

int64_t ReaperBridge::hashKey(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    h &= 0x3FFFFFFFFFFFFFFFULL;   // positive, and never mistaken for an instance ID's negative fallback
    return (int64_t)(h | 1);
}

bool ReaperBridge::attach(const clap_host *host)
{
    host_ = host;
    const reaper_plugin_info_t *rec = (const reaper_plugin_info_t *)host->get_extension(host, "cockos.reaper_extension");
    if (!rec || !rec->GetFunc) return false;
    getFunc_ = rec->GetFunc;
    ctx_ = (void *(*)(const clap_host *, int))getFunc_("clap_get_reaper_context");
    return true;
}

static std::string cleanFxName(const char *s)
{
    std::string n = s;
    static const char *pre[] = {"VST3i: ", "VST3: ", "VSTi: ", "VST: ", "CLAPi: ", "CLAP: ", "JS: ", "AUi: ", "AU: ", "LV2i: ", "LV2: ", "DX: "};
    for (const char *p : pre)
        if (n.rfind(p, 0) == 0) { n = n.substr(strlen(p)); break; }
    // "Name (Vendor)" -> "Name"
    if (!n.empty() && n.back() == ')') {
        size_t o = n.rfind(" (");
        if (o != std::string::npos && o > 0) n = n.substr(0, o);
    }
    return n;
}

bool ReaperBridge::placement(Placement &pl)
{
    pl = Placement();
    if (!getFunc_ || !ctx_) return false;
    MediaTrack *tr = (MediaTrack *)ctx_(host_, 1);
    MediaItem_Take *tk = (MediaItem_Take *)ctx_(host_, 2);
    int idx = (int)(intptr_t)ctx_(host_, 6);
    if (!tr && !tk) return false;
    if (idx < 0) return false;   // REAPER is moving it: ask again next time

    bool (*GetSetMediaItemTakeInfo_String)(MediaItem_Take *, const char *, char *, bool) = nullptr;
    MediaTrack *(*GetMediaItemTake_Track)(MediaItem_Take *) = nullptr;
    RGUID *(*GetTrackGUID)(MediaTrack *) = nullptr;
    void (*guidToString)(const RGUID *, char *) = nullptr;
    double (*GetMediaTrackInfo_Value)(MediaTrack *, const char *) = nullptr;
    bool (*GetTrackName)(MediaTrack *, char *, int) = nullptr;
    int (*TrackFX_GetCount)(MediaTrack *) = nullptr;
    int (*TrackFX_GetRecCount)(MediaTrack *) = nullptr;
    bool (*TrackFX_GetFXName)(MediaTrack *, int, char *, int) = nullptr;
    bool (*TrackFX_GetEnabled)(MediaTrack *, int) = nullptr;
    bool (*TrackFX_GetOffline)(MediaTrack *, int) = nullptr;
    int (*TakeFX_GetCount)(MediaItem_Take *) = nullptr;
    bool (*TakeFX_GetFXName)(MediaItem_Take *, int, char *, int) = nullptr;
    bool (*TakeFX_GetEnabled)(MediaItem_Take *, int) = nullptr;
    bool (*TakeFX_GetOffline)(MediaItem_Take *, int) = nullptr;
    fn(GetSetMediaItemTakeInfo_String, "GetSetMediaItemTakeInfo_String");
    fn(GetMediaItemTake_Track, "GetMediaItemTake_Track");
    fn(GetTrackGUID, "GetTrackGUID");
    fn(guidToString, "guidToString");
    fn(GetMediaTrackInfo_Value, "GetMediaTrackInfo_Value");
    fn(GetTrackName, "GetTrackName");
    fn(TrackFX_GetCount, "TrackFX_GetCount");
    fn(TrackFX_GetRecCount, "TrackFX_GetRecCount");
    fn(TrackFX_GetFXName, "TrackFX_GetFXName");
    fn(TrackFX_GetEnabled, "TrackFX_GetEnabled");
    fn(TrackFX_GetOffline, "TrackFX_GetOffline");
    fn(TakeFX_GetCount, "TakeFX_GetCount");
    fn(TakeFX_GetFXName, "TakeFX_GetFXName");
    fn(TakeFX_GetEnabled, "TakeFX_GetEnabled");
    fn(TakeFX_GetOffline, "TakeFX_GetOffline");

    char buf[512];
    if (tk) {
        pl.pflags |= shm::PL_TAKE;
        buf[0] = 0;
        if (GetSetMediaItemTakeInfo_String) GetSetMediaItemTakeInfo_String(tk, "GUID", buf, false);
        pl.track = hashKey(buf[0] ? buf : "take");
        if (!tr && GetMediaItemTake_Track) tr = GetMediaItemTake_Track(tk);
    } else {
        buf[0] = 0;
        if (GetTrackGUID && guidToString) guidToString(GetTrackGUID(tr), buf);
        pl.track = hashKey(buf[0] ? buf : "track");
    }
    if (tr && GetMediaTrackInfo_Value) pl.trackNo = (int64_t)GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER");
    if (pl.trackNo == -1) pl.trackName = "MASTER";
    else if (tr && GetTrackName && GetTrackName(tr, buf, sizeof buf)) pl.trackName = buf;
    bool input = (idx & 0x1000000) != 0;
    bool inContainer = (idx & 0x2000000) != 0;
    int local = idx & 0xFFFFFF;
    if (input) pl.pflags |= shm::PL_INPUT;
    if (inContainer) pl.pflags |= shm::PL_CONTAINER;
    pl.order = (input ? -100000 : 0) + (inContainer ? (double)idx / 65536.0 : local);
    pl.nflags = shm::NF_INPROJECT;
    if (tk) {
        if (TakeFX_GetEnabled && TakeFX_GetEnabled(tk, idx)) pl.nflags |= shm::NF_ENABLED;
        if (TakeFX_GetOffline && TakeFX_GetOffline(tk, idx)) pl.nflags |= shm::NF_OFFLINE;
        if (!inContainer) {
            if (local > 0 && TakeFX_GetFXName && TakeFX_GetFXName(tk, idx - 1, buf, sizeof buf)) pl.prevName = cleanFxName(buf);
            int n = TakeFX_GetCount ? TakeFX_GetCount(tk) : 0;
            for (int i = local + 1; i < n; ++i)
                if (TakeFX_GetEnabled && TakeFX_GetEnabled(tk, i) && !(TakeFX_GetOffline && TakeFX_GetOffline(tk, i))) pl.nflags |= shm::NF_TAIL;
        }
    } else if (tr) {
        // enabled: not bypassed itself, and its chain not bypassed (the FX button of the track)
        bool chainOn = !(GetMediaTrackInfo_Value && GetMediaTrackInfo_Value(tr, "I_FXEN") == 0);
        if (chainOn && TrackFX_GetEnabled && TrackFX_GetEnabled(tr, idx)) pl.nflags |= shm::NF_ENABLED;
        if (TrackFX_GetOffline && TrackFX_GetOffline(tr, idx)) pl.nflags |= shm::NF_OFFLINE;
        if (!inContainer) {
            if (local > 0 && TrackFX_GetFXName && TrackFX_GetFXName(tr, idx - 1, buf, sizeof buf)) pl.prevName = cleanFxName(buf);
            int n = input ? (TrackFX_GetRecCount ? TrackFX_GetRecCount(tr) : 0) : (TrackFX_GetCount ? TrackFX_GetCount(tr) : 0);
            int flag = input ? 0x1000000 : 0;
            for (int i = local + 1; i < n; ++i)
                if (TrackFX_GetEnabled && TrackFX_GetEnabled(tr, i | flag) && !(TrackFX_GetOffline && TrackFX_GetOffline(tr, i | flag)))
                    pl.nflags |= shm::NF_TAIL;
        }
    }
    pl.known = true;
    return true;
}

bool ReaperBridge::registerHook(const char *name, void *fnp)
{
    int (*reg)(const char *, void *) = nullptr;
    if (!getFunc_ || !fn(reg, "plugin_register")) return false;
    return reg(name, fnp) != 0;
}

std::string ReaperBridge::resourcePath()
{
    const char *(*GetResourcePath)() = nullptr;
    if (!getFunc_ || !fn(GetResourcePath, "GetResourcePath")) return std::string();
    const char *p = GetResourcePath();
    return p ? std::string(p) : std::string();
}

std::string ReaperBridge::version()
{
    const char *(*GetAppVersion)() = nullptr;
    if (!getFunc_ || !fn(GetAppVersion, "GetAppVersion")) return std::string();
    const char *p = GetAppVersion();
    return p ? std::string(p) : std::string();
}

void *ReaperBridge::findTrackByGuid(const std::string &g)
{
    int (*CountTracks)(ReaProject *) = nullptr;
    MediaTrack *(*GetTrack)(ReaProject *, int) = nullptr;
    MediaTrack *(*GetMasterTrack)(ReaProject *) = nullptr;
    RGUID *(*GetTrackGUID)(MediaTrack *) = nullptr;
    void (*guidToString)(const RGUID *, char *) = nullptr;
    if (!fn(CountTracks, "CountTracks") || !fn(GetTrack, "GetTrack") || !fn(GetMasterTrack, "GetMasterTrack") ||
        !fn(GetTrackGUID, "GetTrackGUID") || !fn(guidToString, "guidToString"))
        return nullptr;
    char buf[64];
    MediaTrack *m = GetMasterTrack(nullptr);
    if (m) { guidToString(GetTrackGUID(m), buf); if (g == buf) return m; }
    int n = CountTracks(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaTrack *t = GetTrack(nullptr, i);
        if (!t) continue;
        guidToString(GetTrackGUID(t), buf);
        if (g == buf) return t;
    }
    return nullptr;
}

bool ReaperBridge::feeds(int64_t from, int64_t to)
{
    if (!getFunc_) return false;
    int (*CountTracks)(ReaProject *) = nullptr;
    MediaTrack *(*GetTrack)(ReaProject *, int) = nullptr;
    MediaTrack *(*GetMasterTrack)(ReaProject *) = nullptr;
    RGUID *(*GetTrackGUID)(MediaTrack *) = nullptr;
    void (*guidToString)(const RGUID *, char *) = nullptr;
    int (*GetTrackNumSends)(MediaTrack *, int) = nullptr;
    double (*GetTrackSendInfo_Value)(MediaTrack *, int, int, const char *) = nullptr;
    MediaTrack *(*GetParentTrack)(MediaTrack *) = nullptr;
    double (*GetMediaTrackInfo_Value)(MediaTrack *, const char *) = nullptr;
    if (!fn(CountTracks, "CountTracks") || !fn(GetTrack, "GetTrack") || !fn(GetMasterTrack, "GetMasterTrack") ||
        !fn(GetTrackGUID, "GetTrackGUID") || !fn(guidToString, "guidToString") || !fn(GetTrackNumSends, "GetTrackNumSends") ||
        !fn(GetTrackSendInfo_Value, "GetTrackSendInfo_Value") || !fn(GetParentTrack, "GetParentTrack") ||
        !fn(GetMediaTrackInfo_Value, "GetMediaTrackInfo_Value"))
        return false;
    // the tracks behind the two keys
    std::vector<MediaTrack *> all;
    MediaTrack *master = GetMasterTrack(nullptr);
    if (master) all.push_back(master);
    int n = CountTracks(nullptr);
    for (int i = 0; i < n; ++i) all.push_back(GetTrack(nullptr, i));
    MediaTrack *a = nullptr, *b = nullptr;
    char buf[64];
    for (MediaTrack *t : all) {
        if (!t) continue;
        guidToString(GetTrackGUID(t), buf);
        int64_t k = hashKey(buf);
        if (k == from) a = t;
        if (k == to) b = t;
    }
    if (!a || !b || a == b) return false;
    // breadth-first through sends and parent folders (and the master send)
    std::vector<MediaTrack *> q{a}, seen{a};
    for (size_t qi = 0; qi < q.size() && qi < 4096; ++qi) {
        MediaTrack *t = q[qi];
        std::vector<MediaTrack *> next;
        int ns = GetTrackNumSends(t, 0);
        for (int s = 0; s < ns; ++s) {
            MediaTrack *d = (MediaTrack *)(intptr_t)GetTrackSendInfo_Value(t, 0, s, "P_DESTTRACK");
            if (d) next.push_back(d);
        }
        if (t != master && GetMediaTrackInfo_Value(t, "B_MAINSEND") >= 0.5) {
            MediaTrack *p = GetParentTrack(t);
            next.push_back(p ? p : master);
        }
        for (MediaTrack *d : next) {
            if (!d) continue;
            if (d == b) return true;
            bool was = false;
            for (MediaTrack *x : seen) if (x == d) { was = true; break; }
            if (!was) { seen.push_back(d); q.push_back(d); }
        }
    }
    return false;
}

ReaperBridge::Touched ReaperBridge::lastTouched()
{
    Touched t;
    if (!getFunc_) return t;
    bool (*GetLastTouchedFX)(int *, int *, int *) = nullptr;
    MediaTrack *(*GetTrack)(ReaProject *, int) = nullptr;
    MediaTrack *(*GetMasterTrack)(ReaProject *) = nullptr;
    RGUID *(*TrackFX_GetFXGUID)(MediaTrack *, int) = nullptr;
    RGUID *(*GetTrackGUID)(MediaTrack *) = nullptr;
    void (*guidToString)(const RGUID *, char *) = nullptr;
    bool (*TrackFX_GetFXName)(MediaTrack *, int, char *, int) = nullptr;
    bool (*TrackFX_GetParamName)(MediaTrack *, int, int, char *, int) = nullptr;
    double (*TrackFX_GetParamNormalized)(MediaTrack *, int, int) = nullptr;
    if (!fn(GetLastTouchedFX, "GetLastTouchedFX") || !fn(GetTrack, "GetTrack") || !fn(GetMasterTrack, "GetMasterTrack") ||
        !fn(TrackFX_GetFXGUID, "TrackFX_GetFXGUID") || !fn(GetTrackGUID, "GetTrackGUID") || !fn(guidToString, "guidToString") ||
        !fn(TrackFX_GetFXName, "TrackFX_GetFXName") || !fn(TrackFX_GetParamName, "TrackFX_GetParamName") ||
        !fn(TrackFX_GetParamNormalized, "TrackFX_GetParamNormalized"))
        return t;
    int trackno = 0, fxno = 0, param = 0;
    if (!GetLastTouchedFX(&trackno, &fxno, &param)) return t;
    if ((trackno >> 16) != 0) return t;   // item FX: not supported
    MediaTrack *tr = trackno == 0 ? GetMasterTrack(nullptr) : GetTrack(nullptr, trackno - 1);
    if (!tr) return t;
    RGUID *g = TrackFX_GetFXGUID(tr, fxno);
    if (!g) return t;
    char buf[256], nm[256], pn[256];
    guidToString(g, buf);
    t.fxGuid = buf;
    guidToString(GetTrackGUID(tr), buf);
    t.trackGuid = buf;
    t.param = param;
    nm[0] = pn[0] = 0;
    TrackFX_GetFXName(tr, fxno, nm, sizeof nm);
    TrackFX_GetParamName(tr, fxno, param, pn, sizeof pn);
    t.name = cleanFxName(nm) + ": " + pn;
    t.norm = TrackFX_GetParamNormalized(tr, fxno, param);
    t.ok = true;
    return t;
}

int ReaperBridge::findFxByGuid(void *track, const std::string &g)
{
    MediaTrack *tr = (MediaTrack *)track;
    int (*TrackFX_GetCount)(MediaTrack *) = nullptr;
    int (*TrackFX_GetRecCount)(MediaTrack *) = nullptr;
    RGUID *(*TrackFX_GetFXGUID)(MediaTrack *, int) = nullptr;
    void (*guidToString)(const RGUID *, char *) = nullptr;
    if (!fn(TrackFX_GetCount, "TrackFX_GetCount") || !fn(TrackFX_GetFXGUID, "TrackFX_GetFXGUID") || !fn(guidToString, "guidToString"))
        return -1;
    fn(TrackFX_GetRecCount, "TrackFX_GetRecCount");
    char buf[64];
    int n = TrackFX_GetCount(tr);
    for (int i = 0; i < n; ++i) {
        RGUID *x = TrackFX_GetFXGUID(tr, i);
        if (x) { guidToString(x, buf); if (g == buf) return i; }
    }
    int nr = TrackFX_GetRecCount ? TrackFX_GetRecCount(tr) : 0;
    for (int i = 0; i < nr; ++i) {
        RGUID *x = TrackFX_GetFXGUID(tr, i | 0x1000000);
        if (x) { guidToString(x, buf); if (g == buf) return i | 0x1000000; }
    }
    return -1;
}

// the dB number in a parameter's text ("-3.5 dB", "+2dB", "-inf dB")
static bool parseDb(const char *s, double *db)
{
    std::string l;
    for (const char *p = s; *p; ++p) l += (char)tolower((unsigned char)*p);
    if (l.find("db") == std::string::npos) return false;
    if (l.find("inf") != std::string::npos) { *db = l.find('-') != std::string::npos ? -200 : 200; return true; }
    const char *p = l.c_str();
    while (*p && !(isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.')) ++p;
    if (!*p) return false;
    char *e = nullptr;
    *db = strtod(p, &e);
    return e != p;
}

ReaperBridge::LinkState ReaperBridge::readDb(const std::string &trackGuid, const std::string &fxGuid, int param, double *db,
                                            std::string *name)
{
    if (!getFunc_) return L_GONE;
    bool (*TrackFX_GetFormattedParamValue)(MediaTrack *, int, int, char *, int) = nullptr;
    int (*TrackFX_GetNumParams)(MediaTrack *, int) = nullptr;
    bool (*TrackFX_GetFXName)(MediaTrack *, int, char *, int) = nullptr;
    bool (*TrackFX_GetParamName)(MediaTrack *, int, int, char *, int) = nullptr;
    if (!fn(TrackFX_GetFormattedParamValue, "TrackFX_GetFormattedParamValue") || !fn(TrackFX_GetNumParams, "TrackFX_GetNumParams"))
        return L_GONE;
    MediaTrack *tr = (MediaTrack *)findTrackByGuid(trackGuid);
    if (!tr) return L_GONE;
    int fx = findFxByGuid(tr, fxGuid);
    if (fx < 0 || param < 0 || param >= TrackFX_GetNumParams(tr, fx)) return L_GONE;
    char buf[256];
    buf[0] = 0;
    TrackFX_GetFormattedParamValue(tr, fx, param, buf, sizeof buf);
    if (name && fn(TrackFX_GetFXName, "TrackFX_GetFXName") && fn(TrackFX_GetParamName, "TrackFX_GetParamName")) {
        char nm[256], pn[256];
        nm[0] = pn[0] = 0;
        TrackFX_GetFXName(tr, fx, nm, sizeof nm);
        TrackFX_GetParamName(tr, fx, param, pn, sizeof pn);
        *name = cleanFxName(nm) + ": " + pn;
    }
    return parseDb(buf, db) ? L_OK : L_NOTDB;
}

ReaperBridge::LinkState ReaperBridge::writeDb(const std::string &trackGuid, const std::string &fxGuid, int param, double db)
{
    if (!getFunc_) return L_GONE;
    bool (*TrackFX_FormatParamValueNormalized)(MediaTrack *, int, int, double, char *, int) = nullptr;
    bool (*TrackFX_SetParamNormalized)(MediaTrack *, int, int, double) = nullptr;
    if (!fn(TrackFX_FormatParamValueNormalized, "TrackFX_FormatParamValueNormalized") ||
        !fn(TrackFX_SetParamNormalized, "TrackFX_SetParamNormalized"))
        return L_GONE;
    MediaTrack *tr = (MediaTrack *)findTrackByGuid(trackGuid);
    if (!tr) return L_GONE;
    int fx = findFxByGuid(tr, fxGuid);
    if (fx < 0) return L_GONE;
    char buf[256];
    auto at = [&](double nv, double *out) -> bool {
        buf[0] = 0;
        if (!TrackFX_FormatParamValueNormalized(tr, fx, param, nv, buf, sizeof buf)) return false;
        return parseDb(buf, out);
    };
    double lo = 0, hi = 1, vlo, vhi;
    if (!at(lo, &vlo) || !at(hi, &vhi)) return L_NOTDB;
    bool up = vhi >= vlo;
    double tgt = db;
    double minv = up ? vlo : vhi, maxv = up ? vhi : vlo;
    if (tgt <= minv) { TrackFX_SetParamNormalized(tr, fx, param, up ? 0 : 1); return L_OK; }
    if (tgt >= maxv) { TrackFX_SetParamNormalized(tr, fx, param, up ? 1 : 0); return L_OK; }
    for (int i = 0; i < 40; ++i) {
        double mid = 0.5 * (lo + hi), vm;
        if (!at(mid, &vm)) return L_NOTDB;
        if ((vm < tgt) == up) lo = mid;
        else hi = mid;
    }
    TrackFX_SetParamNormalized(tr, fx, param, 0.5 * (lo + hi));
    return L_OK;
}

} // namespace ts
