// REAPER, when the host is REAPER: where this instance sits (track, chain, position), names for the
// Gain Stage and the windows, the user-preset folder, and the drive link to a plug-in parameter.
// In the JSFX suite a helper script did this; a CLAP plug-in asks REAPER itself. Main thread only.
// In any other host every call just reports "unknown".
#pragma once
#include <cstdint>
#include <string>

struct clap_host;

namespace ts {

struct Placement {
    bool known = false;
    int64_t track = 0;     // chain key (hash of the track or take GUID)
    int64_t trackNo = 0;   // 1-based, -1 master
    int64_t pflags = 0;    // shm::PL_*
    double order = 0;      // chain order (input FX first)
    int64_t nflags = 0;    // shm::NF_* (without NF_REFFED)
    std::string trackName; // "MASTER" for the master track
    std::string prevName;  // the plug-in before this one
};

class ReaperBridge {
public:
    bool attach(const clap_host *host);   // true inside REAPER
    bool ok() const { return getFunc_ != nullptr; }

    bool placement(Placement &pl);
    std::string resourcePath();
    std::string version();
    // does the track of chain key `from` feed the track of chain key `to` (sends, folders)?
    bool feeds(int64_t from, int64_t to);

    // ---- drive link to a plug-in parameter ----
    // the parameter last touched in REAPER: its plug-in's GUID, track GUID, index and display name
    struct Touched {
        bool ok = false;
        std::string fxGuid, trackGuid, name;
        int param = -1;
        double norm = 0;
    };
    Touched lastTouched();
    // the linked parameter's value in dB (from its displayed text); false if gone or not in dB
    enum LinkState { L_OK, L_GONE, L_NOTDB };
    LinkState readDb(const std::string &trackGuid, const std::string &fxGuid, int param, double *db, std::string *name);
    // move it to `db` (searching the normalized value whose text reads that many dB)
    LinkState writeDb(const std::string &trackGuid, const std::string &fxGuid, int param, double db);

    static int64_t hashKey(const char *s);
    // plugin_register (hooks such as "hwnd_info"; "-name" removes one)
    bool registerHook(const char *name, void *fn);

private:
    const clap_host *host_ = nullptr;
    void *(*getFunc_)(const char *) = nullptr;
    void *(*ctx_)(const clap_host *, int) = nullptr;
    template <class T> bool fn(T &f, const char *name)
    {
        f = (T)getFunc_(name);
        return f != nullptr;
    }
    void *findTrackByGuid(const std::string &g);
    int findFxByGuid(void *track, const std::string &g);
};

} // namespace ts
