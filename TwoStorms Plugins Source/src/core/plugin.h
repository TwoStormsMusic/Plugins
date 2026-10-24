// The CLAP plug-in every TwoStorms plug-in is built on: audio ports (stereo in, optional stereo
// sidechain, stereo out, 32- or 64-bit), parameters (JSFX slider numbers as IDs, gestures, the
// window's edits reaching the host), state, latency, A/B sets, presets, the built-in Act, timers,
// and REAPER (placement, names, the drive link to a plug-in parameter).
//
// A plug-in derives from Plugin and provides its parameter table, its DSP (dspActivate / dspReset /
// dspProcess) and its window (createView). See src/plugins/compressor for the first one.
#pragma once
#include "act.h"
#include "params.h"
#include "presets.h"
#include "reaper_bridge.h"
#include <clap/clap.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ts {

namespace gui {
class Editor;
class View;
} // namespace gui

class Plugin;

// the host's transport for a block (the JSFX's tempo, play_state, beat_position, ts_num, ts_denom)
struct Transport {
    bool playing = false;
    double tempo = 0;        // 0: not given
    double beat = 0;         // song position in quarter notes
    int tsNum = 0, tsDen = 0;
};
// a MIDI message in a block (K.midi): note-ons and offs from CLAP notes are given as MIDI messages too
struct MidiEv { uint32_t time; uint8_t len; uint8_t d[3]; };

struct PluginKind {
    clap_plugin_descriptor desc;
    const ParamTable &(*table)();
    const std::vector<FactoryPreset> &(*factory)();
    const char *fileName;   // user-preset file ("Compressor")
    const char *title;      // the window's title ("TWOSTORMS COMPRESSOR")
    int kind, subkind;      // slot kind and subkind in shared memory
    bool sidechain;         // a stereo sidechain input (REAPER: channels 3-4)
    bool hasAB;             // A/B sets (and Swap)
    int abParam;            // the parameter that says which set plays (0 = none)
    Plugin *(*create)(const clap_host *host);
    int channels;           // main input/output channels: 0 = 2 (a stereo pair); the Act has 8
    bool midi;              // a MIDI input and output (everything passes through; the DSP sees the messages)
    int nch() const { return channels > 0 ? channels : 2; }
};

// a single-producer, single-consumer queue (the window -> the audio thread)
template <class T, int N> class Spsc {
public:
    bool push(const T &v)
    {
        unsigned w = w_.load(std::memory_order_relaxed), r = r_.load(std::memory_order_acquire);
        if (w - r >= (unsigned)N) return false;
        buf_[w % N] = v;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T &v)
    {
        unsigned r = r_.load(std::memory_order_relaxed), w = w_.load(std::memory_order_acquire);
        if (r == w) return false;
        v = buf_[r % N];
        r_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    T buf_[N];
    std::atomic<unsigned> w_{0}, r_{0};
};

class Plugin : public ActHost {
public:
    Plugin(const clap_host *host, const PluginKind &k);
    virtual ~Plugin();
    const clap_plugin *clapPlugin() const { return &plugin_; }

    const clap_host *const host;
    const PluginKind &K;
    const ParamTable &T;
    ParamValues P;
    Act act;
    ReaperBridge reaper;
    PresetStore userPresets;

    // ---------------- parameters, from the window (main thread) ----------------
    void beginEdit(int id);
    void edit(int id, double v);          // inside beginEdit / endEdit
    void endEdit(int id);
    void setOnce(int id, double v);       // a click: begin, set, end
    // the Act's parameters keep linked pairs on one fader (the JSFX's g_apply); gesture: inside a drag
    void apply(int id, double v, bool gesture = false);
    double get(int id) const { return P.get(id); }

    // ---------------- A/B ----------------
    double abValue(int id) const { return (id > 0 && id < MAXP) ? AB_[id].load(std::memory_order_relaxed) : 0; }
    void abSwap();
    void abCopy();

    // ---------------- presets ----------------
    void presetReset();
    void presetApply(const std::vector<std::pair<int, double>> &vals);   // from the defaults
    void presetFactory(int index);
    std::vector<UserPreset> presetUserList();
    bool presetUserApply(const std::string &name);
    bool presetUserSave(const std::string &name);
    bool presetUserExists(const std::string &name) const { return userPresets.exists(name); }
    bool presetUserDelete(const std::string &name);
    std::string presetMsg;
    double presetMsgT = -100;

    // ---------------- REAPER and the shared world ----------------
    Placement placement();               // where this instance is (copy)
    bool inReaper() const { return reaper.ok(); }
    // drive link to a plug-in parameter
    void driveLearn();                    // the next plug-in parameter that moves gets linked
    int driveStatus() const { return dstat_.load(); }
    std::string driveName();
    double sampleRate() const { return srate_; }
    int latencyNow() const { return latencyReported_; }

    // ---------------- ActHost ----------------
    double pv(int id) const override { return P.get(id); }
    void setFromAudio(int id, double v) override;
    double clampParam(int id, double v) const override { return T.clamp(id, v); }
    void putQuiet(int id, double v) override { P.put(id, v); }

    // the window changed something kept in the state that isn't a parameter (the Gain Stage's tables)
    void markDirty() { dirty_ = true; }

    // ---------------- the window ----------------
    virtual gui::View *createView() = 0;
    std::atomic<double> guiOpenBeat{0};

protected:
    // ---------------- what each plug-in provides ----------------
    virtual void dspActivate(double sr, uint32_t maxFrames) = 0;
    virtual void dspReset() = 0;
    // in: main input (K.nch() channels); sc: sidechain (2 channels) or null; out: K.nch() channels
    virtual void dspProcess(const double *const *in, const double *const *sc, double *const *out, int n) = 0;
    virtual int latencyFromParams(double sr) const { (void)sr; return 0; }   // main thread
    virtual void stateSaveExtra(std::string &s) { (void)s; }
    virtual void stateLoadExtra(const std::string &key, const std::string &val) { (void)key; (void)val; }
    virtual void stateLoadBegin() {}   // before a state's values come in
    virtual void onMainTimer() {}
    // a user preset's extras (the JSFX's pr_xsave / pr_xload; the IR Loader's file paths)
    virtual void presetExtraSave(std::vector<double> &x) { (void)x; }
    virtual void presetExtraLoad(const std::vector<double> &x) { (void)x; }
    // a parameter's default as the window resets it (double-click; a plug-in can make it depend on other settings)
    virtual double pdef(int id) const { return T.def(id); }
    // a window-only parameter that shows and edits part of a packed one (the EQ's ratio, hold and knee of the selected
    // band): the real parameter it changes (0: not one), its value, and the real parameter's value for a new one
    virtual int vpTarget(int id) const { (void)id; return 0; }
    virtual double vpGet(int id) const { (void)id; return 0; }
    virtual double vpPacked(int id, double v) const { (void)id; (void)v; return 0; }

    double srate_ = 44100;
    uint32_t maxFrames_ = 0;

    // the host's transport for this block, and this block's MIDI (see Transport, MidiEv)
    Transport tr;
    std::vector<MidiEv> midiIn;
    std::atomic<bool> active_{false};
    std::atomic<bool> processing_{false};

    // host extensions
    const clap_host_params *hParams_ = nullptr;
    const clap_host_latency *hLatency_ = nullptr;
    const clap_host_state *hState_ = nullptr;
    const clap_host_timer_support *hTimer_ = nullptr;
    const clap_host_posix_fd_support *hFd_ = nullptr;
    const clap_host_gui *hGui_ = nullptr;

    friend class gui::Editor;

private:
    clap_plugin plugin_;
    std::unique_ptr<gui::Editor> editor_;
    std::atomic<double> AB_[MAXP];

    // window edits on their way to the host
    struct GEv { uint8_t type; uint16_t id; double v; };   // type: 0 value, 1 begin, 2 end
    Spsc<GEv, 1024> guiQ_;
    std::atomic<double> lastProcessT_{0};
    void guiPush(uint8_t type, int id, double v);
    // audio-thread changes on their way to the host
    struct AEv { uint16_t id; double v; };
    AEv audioEv_[128];
    int nAudioEv_ = 0;
    std::atomic<bool> dirty_{false};
    void sendGui(const clap_output_events *out);
    void sendAudio(const clap_output_events *out);
    void handleIn(const clap_input_events *in);

    // buffers
    std::vector<double> buf_[2 * 8 + 2];   // inputs, outputs (8 each at most), sidechain

    // latency
    int latencyReported_ = 0;
    int latencyWant_ = 0;
    double latencyChangeT_ = 0;
    bool restartAsked_ = false;

    // main-thread housekeeping
    clap_id timerId_ = CLAP_INVALID_ID;
    std::thread fallbackTimer_;
    std::atomic<bool> fallbackRun_{false};
    int tick_ = 0;
    std::mutex plMutex_;
    Placement pl_;
    void mainTimer();
    void updatePlacement();
    int testOrder_ = -1;
    // drive link to a plug-in parameter
    std::string dTrack_, dFx_, dName_;
    int dParam_ = -1;
    std::atomic<int> dstat_{0};
    ReaperBridge::Touched dBase_;
    int64_t dLearnSeen_ = 0;
    double dLast_ = 1e9;
    bool dLearning_ = false;
    void driveTick();
    std::string presetDirChecked_;

    // ---------------- CLAP callbacks ----------------
    bool init();
    void destroy();
    bool activate(double sr, uint32_t minF, uint32_t maxF);
    void deactivate();
    bool startProcessing();
    void stopProcessing();
    void reset();
    clap_process_status process(const clap_process *p);
    const void *getExtension(const char *id);
    void onMainThread();
    // params
    uint32_t paramsCount();
    bool paramsInfo(uint32_t index, clap_param_info *info);
    bool paramsValue(clap_id id, double *v);
    bool paramsValueToText(clap_id id, double v, char *buf, uint32_t n);
    bool paramsTextToValue(clap_id id, const char *s, double *v);
    void paramsFlush(const clap_input_events *in, const clap_output_events *out);
    // state
    bool stateSave(const clap_ostream *s);
    bool stateLoad(const clap_istream *s);

    static Plugin *self(const clap_plugin *p) { return (Plugin *)p->plugin_data; }
    static const clap_plugin_audio_ports s_ports;
    static const clap_plugin_params s_params;
    static const clap_plugin_state s_state;
    static const clap_plugin_latency s_latency;
    static const clap_plugin_timer_support s_timer;
    static const clap_plugin_posix_fd_support s_fd;
    static const clap_plugin_gui s_gui;
    static const clap_plugin_note_ports s_notes;
    void midiCollect(const clap_input_events *in);
    void midiThrough(const clap_input_events *in, const clap_output_events *out);
};

} // namespace ts
