#include "plugin.h"
#include <algorithm>
#include "../gui/editor.h"
#include "clock.h"
#include "eel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define TS_HAS_SSE 1
#endif

namespace ts {

// flush denormals to zero on the audio thread (the JSFX's ext_nodenorm leaves it to the host)
struct FtzGuard {
#ifdef TS_HAS_SSE
    unsigned old;
    FtzGuard() : old(_mm_getcsr()) { _mm_setcsr(old | 0x8040); }
    ~FtzGuard() { _mm_setcsr(old); }
#endif
};

// ======================================================================== construction
Plugin::Plugin(const clap_host *h, const PluginKind &k)
    : host(h), K(k), T(k.table()), P(k.table()), act(*this, k.kind, k.subkind)
{
    for (auto &a : AB_) a.store(0.0, std::memory_order_relaxed);
    for (int id : T.abIds()) AB_[id].store(P.get(id), std::memory_order_relaxed);
    plugin_.desc = &K.desc;
    plugin_.plugin_data = this;
    plugin_.init = [](const clap_plugin *p) { return self(p)->init(); };
    plugin_.destroy = [](const clap_plugin *p) { self(p)->destroy(); };
    plugin_.activate = [](const clap_plugin *p, double sr, uint32_t a, uint32_t b) { return self(p)->activate(sr, a, b); };
    plugin_.deactivate = [](const clap_plugin *p) { self(p)->deactivate(); };
    plugin_.start_processing = [](const clap_plugin *p) { return self(p)->startProcessing(); };
    plugin_.stop_processing = [](const clap_plugin *p) { self(p)->stopProcessing(); };
    plugin_.reset = [](const clap_plugin *p) { self(p)->reset(); };
    plugin_.process = [](const clap_plugin *p, const clap_process *pr) { return self(p)->process(pr); };
    plugin_.get_extension = [](const clap_plugin *p, const char *id) { return self(p)->getExtension(id); };
    plugin_.on_main_thread = [](const clap_plugin *p) { self(p)->onMainThread(); };
    userPresets.setup(K.fileName, "");
    // tests: TS_TEST_PRESETCHECK lists every factory preset value that is not a preset setting or is out of range
    if (getenv("TS_TEST_PRESETCHECK")) {
        std::vector<bool> in(MAXP, false);
        for (int p : T.presetIds()) in[p] = true;
        const auto &f = K.factory();
        int bad = 0;
        for (const auto &fp : f)
            for (auto &kv : parsePresetString(fp.values)) {
                const ParamDef *d = T.get(kv.first);
                bool ok = kv.first > 0 && kv.first < MAXP && in[kv.first] && d && kv.second >= d->min - 1e-9 && kv.second <= d->max + 1e-9;
                if (!ok) {
                    fprintf(stderr, "[preset] %s: %s: %d=%g%s\n", K.fileName, fp.path.c_str(), kv.first, kv.second,
                            !d ? " (no such setting)" : (!in[kv.first] ? " (not in presets)" : " (out of range)"));
                    ++bad;
                }
            }
        fprintf(stderr, "[preset] %s: %d presets, %d problems\n", K.fileName, (int)f.size(), bad);
    }
}

Plugin::~Plugin() {}

bool Plugin::init()
{
    hParams_ = (const clap_host_params *)host->get_extension(host, CLAP_EXT_PARAMS);
    hLatency_ = (const clap_host_latency *)host->get_extension(host, CLAP_EXT_LATENCY);
    hState_ = (const clap_host_state *)host->get_extension(host, CLAP_EXT_STATE);
    hTimer_ = (const clap_host_timer_support *)host->get_extension(host, CLAP_EXT_TIMER_SUPPORT);
    hFd_ = (const clap_host_posix_fd_support *)host->get_extension(host, CLAP_EXT_POSIX_FD_SUPPORT);
    hGui_ = (const clap_host_gui *)host->get_extension(host, CLAP_EXT_GUI);
    if (reaper.attach(host)) userPresets.setup(K.fileName, reaper.resourcePath());
    // housekeeping on the main thread, 20 times a second
    if (hTimer_ && hTimer_->register_timer && hTimer_->register_timer(host, 50, &timerId_)) {
    } else {
        // no timer from the host: ask for main-thread callbacks from a small thread
        timerId_ = CLAP_INVALID_ID;
        fallbackRun_ = true;
        fallbackTimer_ = std::thread([this] {
            while (fallbackRun_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (fallbackRun_) host->request_callback(host);
            }
        });
    }
    return true;
}

void Plugin::destroy()
{
    if (editor_) { editor_->destroy(); editor_.reset(); }
    if (timerId_ != CLAP_INVALID_ID && hTimer_) hTimer_->unregister_timer(host, timerId_);
    if (fallbackRun_) {
        fallbackRun_ = false;
        if (fallbackTimer_.joinable()) fallbackTimer_.join();
    }
    // give the slot back
    if (act.myslot >= 0) shm::slot(act.myslot).owner.store(0);
    delete this;
}

bool Plugin::activate(double sr, uint32_t, uint32_t maxF)
{
    srate_ = sr;
    maxFrames_ = maxF;
    for (auto &b : buf_) b.assign(maxF > 0 ? maxF : 4096, 0.0);
    act.init(sr);
    dspActivate(sr, maxF);
    latencyReported_ = latencyFromParams(sr);
    latencyWant_ = latencyReported_;
    restartAsked_ = false;
    active_ = true;
    return true;
}

void Plugin::deactivate() { active_ = false; }
bool Plugin::startProcessing() { processing_ = true; return true; }
void Plugin::stopProcessing() { processing_ = false; }
void Plugin::reset() { dspReset(); }

// ======================================================================== audio
void Plugin::handleIn(const clap_input_events *in)
{
    if (!in) return;
    uint32_t n = in->size(in);
    bool gains = false;
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header *h = in->get(in, i);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (h->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value *e = (const clap_event_param_value *)h;
            int id = (int)e->param_id;
            if (!T.get(id)) continue;
            P.put(id, T.clamp(id, e->value));
            if (id >= P_GAIN && id < P_GAIN + 8) gains = true;
        }
    }
    // @slider: a host change to one side of a linked pair moves the other side too
    if (gains) act.sliderSync();
}

void Plugin::sendGui(const clap_output_events *out)
{
    GEv e;
    while (guiQ_.pop(e)) {
        const ParamDef *d = T.get(e.id);
        if (!d || !(d->flags & PF_HOST) || !out) continue;
        if (e.type == 0) {
            clap_event_param_value ev{};
            ev.header.size = sizeof ev;
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.param_id = e.id;
            ev.value = e.v;
            ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1;
            out->try_push(out, &ev.header);
        } else {
            clap_event_param_gesture ev{};
            ev.header.size = sizeof ev;
            ev.header.type = e.type == 1 ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.param_id = e.id;
            out->try_push(out, &ev.header);
        }
    }
}

void Plugin::sendAudio(const clap_output_events *out)
{
    for (int i = 0; i < nAudioEv_; ++i) {
        if (!out) break;
        clap_event_param_gesture g{};
        g.header.size = sizeof g;
        g.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        g.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
        g.param_id = audioEv_[i].id;
        out->try_push(out, &g.header);
        clap_event_param_value ev{};
        ev.header.size = sizeof ev;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.param_id = audioEv_[i].id;
        ev.value = audioEv_[i].v;
        ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1;
        out->try_push(out, &ev.header);
        g.header.type = CLAP_EVENT_PARAM_GESTURE_END;
        out->try_push(out, &g.header);
    }
    nAudioEv_ = 0;
}

// a parameter set by the audio thread (Auto, Set threshold, the Gain Stage, a drive link)
void Plugin::setFromAudio(int id, double v)
{
    P.put(id, v);
    const ParamDef *d = T.get(id);
    if (d && (d->flags & PF_HOST)) {
        for (int i = 0; i < nAudioEv_; ++i)
            if (audioEv_[i].id == id) { audioEv_[i].v = v; return; }
        if (nAudioEv_ < 128) audioEv_[nAudioEv_++] = {(uint16_t)id, v};
    } else {
        dirty_ = true;
    }
}

clap_process_status Plugin::process(const clap_process *pr)
{
    FtzGuard ftz;
    lastProcessT_.store(now_sec(), std::memory_order_relaxed);
    const int n = (int)pr->frames_count;
    handleIn(pr->in_events);
    sendGui(pr->out_events);
    // the transport, and this block's MIDI
    if (const clap_event_transport *t = pr->transport) {
        tr.playing = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
        tr.tempo = (t->flags & CLAP_TRANSPORT_HAS_TEMPO) ? t->tempo : 0;
        if (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) tr.beat = (double)t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
        if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) { tr.tsNum = t->tsig_num; tr.tsDen = t->tsig_denom; }
    } else {
        tr.playing = false;
        tr.tempo = 0;
    }
    if (K.midi) midiCollect(pr->in_events);
    if (n <= 0) return CLAP_PROCESS_CONTINUE;
    if ((uint32_t)n > buf_[0].size())
        for (auto &b : buf_) b.resize(n);   // never expected (maxFrames), but never overrun

    // inputs to double: in[0..nch), out[0..nch), sidechain (2)
    const int NC = K.nch();
    double *in[8], *out[8];
    for (int c = 0; c < NC; ++c) { in[c] = buf_[c].data(); out[c] = buf_[8 + c].data(); }
    double *sc[2] = {buf_[16].data(), buf_[17].data()};
    bool haveSc = false;
    auto load = [&](const clap_audio_buffer &b, double **dst, int nc) {
        for (int c = 0; c < nc; ++c) {
            // a mono input feeds both sides of a stereo plug-in; missing channels are silent
            int src = (int)b.channel_count > c ? c : (nc == 2 ? (int)b.channel_count - 1 : -1);
            if (src < 0) { memset(dst[c], 0, sizeof(double) * n); continue; }
            if (b.data64 && b.data64[src]) memcpy(dst[c], b.data64[src], sizeof(double) * n);
            else if (b.data32 && b.data32[src]) { const float *s = b.data32[src]; for (int i = 0; i < n; ++i) dst[c][i] = s[i]; }
            else memset(dst[c], 0, sizeof(double) * n);
        }
    };
    if (pr->audio_inputs_count >= 1) load(pr->audio_inputs[0], in, NC);
    else for (int c = 0; c < NC; ++c) memset(in[c], 0, sizeof(double) * n);
    if (K.sidechain && pr->audio_inputs_count >= 2 && pr->audio_inputs[1].channel_count > 0) {
        load(pr->audio_inputs[1], sc, 2);
        haveSc = true;
    }
    if (K.sidechain && !haveSc) { memset(sc[0], 0, sizeof(double) * n); memset(sc[1], 0, sizeof(double) * n); }

    dspProcess(in, K.sidechain ? sc : nullptr, out, n);

    if (pr->audio_outputs_count >= 1) {
        const clap_audio_buffer &b = pr->audio_outputs[0];
        for (uint32_t c = 0; c < b.channel_count; ++c) {
            const double *s = out[(int)c < NC ? (int)c : NC - 1];
            if ((int)c >= NC && NC != 2) {   // more host channels than ours: silence
                if (b.data64 && b.data64[c]) memset(b.data64[c], 0, sizeof(double) * n);
                else if (b.data32 && b.data32[c]) memset(b.data32[c], 0, sizeof(float) * n);
                continue;
            }
            if (b.data64 && b.data64[c]) memcpy(b.data64[c], s, sizeof(double) * n);
            else if (b.data32 && b.data32[c]) { float *d = b.data32[c]; for (int i = 0; i < n; ++i) d[i] = (float)s[i]; }
        }
    }
    sendAudio(pr->out_events);
    // MIDI passes through (after this block's parameter events, which are all at its start)
    if (K.midi) midiThrough(pr->in_events, pr->out_events);
    return CLAP_PROCESS_CONTINUE;
}

// ======================================================================== MIDI
void Plugin::midiCollect(const clap_input_events *in)
{
    midiIn.clear();
    if (!in) return;
    uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header *h = in->get(in, i);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        MidiEv m{h->time, 0, {0, 0, 0}};
        if (h->type == CLAP_EVENT_MIDI) {
            const clap_event_midi *e = (const clap_event_midi *)h;
            m.d[0] = e->data[0]; m.d[1] = e->data[1]; m.d[2] = e->data[2];
            uint8_t st = m.d[0] & 0xF0;
            m.len = (st == 0xC0 || st == 0xD0) ? 2 : 3;
        } else if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) {
            const clap_event_note *e = (const clap_event_note *)h;
            if (e->key < 0 || e->key > 127) continue;
            int ch = e->channel >= 0 && e->channel < 16 ? e->channel : 0;
            int vel = (int)std::lround(e->velocity * 127);
            if (h->type == CLAP_EVENT_NOTE_ON) vel = std::max(1, std::min(127, vel));
            else vel = std::max(0, std::min(127, vel));
            m.d[0] = (uint8_t)((h->type == CLAP_EVENT_NOTE_ON ? 0x90 : 0x80) | ch);
            m.d[1] = (uint8_t)e->key;
            m.d[2] = (uint8_t)vel;
            m.len = 3;
        } else continue;
        if (midiIn.size() < 1024) midiIn.push_back(m);
    }
}

void Plugin::midiThrough(const clap_input_events *in, const clap_output_events *out)
{
    if (!in || !out) return;
    uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header *h = in->get(in, i);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        switch (h->type) {
        case CLAP_EVENT_MIDI: case CLAP_EVENT_MIDI_SYSEX: case CLAP_EVENT_MIDI2: case CLAP_EVENT_NOTE_ON: case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE: case CLAP_EVENT_NOTE_EXPRESSION:
            out->try_push(out, h);
            break;
        default:
            break;
        }
    }
}

// ======================================================================== the window's edits
void Plugin::guiPush(uint8_t type, int id, double v)
{
    const ParamDef *d = T.get(id);
    if (!d) return;
    if (!(d->flags & PF_HOST)) {
        if (type == 0) dirty_ = true;
        return;
    }
    guiQ_.push({type, (uint16_t)id, v});
    // not processing (stopped transport, a sleeping track): ask the host to flush
    if (hParams_ && now_sec() - lastProcessT_.load(std::memory_order_relaxed) > 0.1) hParams_->request_flush(host);
}

void Plugin::beginEdit(int id) { guiPush(1, id, 0); }
void Plugin::edit(int id, double v)
{
    v = T.clamp(id, v);
    P.put(id, v);
    guiPush(0, id, v);
}
void Plugin::endEdit(int id) { guiPush(2, id, 0); }
void Plugin::setOnce(int id, double v)
{
    beginEdit(id);
    edit(id, v);
    endEdit(id);
}

// set a parameter of this instance, keeping linked pairs on one fader (g_apply)
void Plugin::apply(int p, double v, bool gesture)
{
    auto set = [&](int id, double x) { if (gesture) edit(id, x); else setOnce(id, x); };
    v = T.clamp(p, v);
    set(p, v);
    auto used = [&](int c) { return c < 2 + 2 * iclamp(iround(P.get(P_NCH)), 0, 3) && P.get(P_USED + c) >= 0.5; };
    auto linked = [&](int k) { return P.get(P_LINK + k) >= 0.5 && used(k * 2) && used(k * 2 + 1); };
    if (p >= P_GAIN && p < P_GAIN + 8) {
        int c = p - P_GAIN, k = c / 2, pc = c - (c % 2) + (1 - c % 2);
        if (linked(k)) set(P_GAIN + pc, v);
    }
    if (p >= P_LINK && p < P_LINK + 4) {
        int k = p - P_LINK;
        if (linked(k)) set(P_GAIN + k * 2 + 1, P.get(P_GAIN + k * 2));
    }
    if (p >= P_USED && p < P_USED + 8) {
        int c = p - P_USED, k = c / 2, pc = c - (c % 2) + (1 - c % 2);
        if (v >= 0.5 && linked(k)) set(P_GAIN + c, P.get(P_GAIN + pc));
    }
}

// ======================================================================== A/B
// the other A/B set: swap it with the playing one
void Plugin::abSwap()
{
    for (int p : T.abIds()) {
        double t = P.get(p);
        setOnce(p, AB_[p].load(std::memory_order_relaxed));
        AB_[p].store(t, std::memory_order_relaxed);
    }
    if (K.abParam > 0) setOnce(K.abParam, P.get(K.abParam) < 0.5 ? 1 : 0);
    dirty_ = true;
}

// copy the playing set into the other one
void Plugin::abCopy()
{
    for (int p : T.abIds()) AB_[p].store(P.get(p), std::memory_order_relaxed);
    dirty_ = true;
}

// ======================================================================== presets
void Plugin::presetReset()
{
    for (int p : T.presetIds()) setOnce(p, T.def(p));
}

void Plugin::presetApply(const std::vector<std::pair<int, double>> &vals)
{
    // from every setting's default, so a preset always sounds the same
    std::vector<double> v(MAXP, 0.0);
    std::vector<bool> in(MAXP, false);
    for (int p : T.presetIds()) { v[p] = T.def(p); in[p] = true; }
    for (auto &kv : vals)
        if (kv.first > 0 && kv.first < MAXP && in[kv.first]) v[kv.first] = T.clamp(kv.first, kv.second);
    for (int p : T.presetIds()) setOnce(p, v[p]);
}

void Plugin::presetFactory(int index)
{
    const auto &f = K.factory();
    if (index < 0 || index >= (int)f.size()) return;
    presetApply(parsePresetString(f[index].values));
}

std::vector<UserPreset> Plugin::presetUserList() { return userPresets.load(); }

bool Plugin::presetUserApply(const std::string &name)
{
    for (auto &p : userPresets.load())
        if (p.name == name) {
            presetApply(p.values);
            presetExtraLoad(p.extra);
            return true;
        }
    return false;
}

bool Plugin::presetUserSave(const std::string &name)
{
    UserPreset u;
    u.name = name;
    for (int p : T.presetIds()) u.values.push_back({p, P.get(p)});
    presetExtraSave(u.extra);
    bool full = false;
    bool ok = userPresets.save(u, &full);
    presetMsg = ok ? "preset saved" : (full ? "40 user presets kept: delete one first" : "the preset could not be saved");
    presetMsgT = now_sec();
    if (ok) shm::get()->h.presetv.fetch_add(1);
    return ok;
}

bool Plugin::presetUserDelete(const std::string &name)
{
    bool ok = userPresets.remove(name);
    presetMsg = ok ? "preset deleted" : "the preset could not be deleted";
    presetMsgT = now_sec();
    if (ok) shm::get()->h.presetv.fetch_add(1);
    return ok;
}

// ======================================================================== main thread
Placement Plugin::placement()
{
    std::lock_guard<std::mutex> l(plMutex_);
    return pl_;
}

void Plugin::updatePlacement()
{
    Placement pl;
    bool known = reaper.placement(pl);
    // tests outside REAPER: TS_TEST_CHAIN puts every instance of the process in one chain, in the
    // order they were made
    static const char *testChain = getenv("TS_TEST_CHAIN");
    if (!known && testChain) {
        static std::atomic<int> made{0};
        if (testOrder_ < 0) testOrder_ = made++;
        pl.known = known = true;
        pl.track = ReaperBridge::hashKey(testChain);
        pl.trackNo = 1;
        pl.order = testOrder_;
        pl.nflags = shm::NF_INPROJECT | shm::NF_ENABLED;
        pl.trackName = "Test chain";
    }
    if (known) {
        act.plTrack.store(pl.track, std::memory_order_relaxed);
        act.plTrackNo.store(pl.trackNo, std::memory_order_relaxed);
        act.plFlags.store(pl.pflags, std::memory_order_relaxed);
        act.plOrder.store(pl.order, std::memory_order_relaxed);
        act.plKnown.store(1, std::memory_order_relaxed);
    }
    int s = act.slotPub.load(std::memory_order_relaxed);
    int64_t id = act.idPub.load(std::memory_order_relaxed);
    // only into a slot that is still this instance's (and only what REAPER just told: a failed look keeps the last)
    bool mine = s >= 0 && id >= 100000 && shm::slot(s).owner.load() == id && shm::slot(s).nonce.load() == act.mynonce;
    if (mine && (known || !reaper.ok())) {
        // NF_REFFED: this track feeds the reference's track (changing this gain would move the reference)
        int64_t ref = (int64_t)P.get(P_REF);
        if (known && ref >= 100000) {
            int mx = (int)std::min<int64_t>(shm::NSLOT, shm::get()->h.maxslot.load());
            for (int r = 0; r < mx; ++r) {
                if (shm::slot(r).owner.load() == ref) {
                    int64_t rt = shm::slot(r).track.load();
                    if (rt != pl.track && reaper.feeds(pl.track, rt)) pl.nflags |= shm::NF_REFFED;
                    break;
                }
            }
        }
        shm::writeNames(s, id, (int64_t)pl.order, known ? pl.nflags : 0, dstat_.load(), pl.trackName.c_str(),
                        pl.prevName.c_str(), dName_.c_str());
    }
    std::lock_guard<std::mutex> l(plMutex_);
    pl_ = pl;
}

void Plugin::driveLearn()
{
    if (!reaper.ok()) return;
    dBase_ = reaper.lastTouched();
    dLearning_ = true;
    dstat_ = shm::DS_LEARNING;
    dName_.clear();
}

std::string Plugin::driveName()
{
    return dName_;
}

// Drive link to a plug-in parameter: when this Act's gain moves (by hand, Auto, Match, automation;
// not by drive links), the linked parameter moves the other way by as many dB.
void Plugin::driveTick()
{
    if (!reaper.ok()) return;
    bool isParam = iround(P.get(P_DRIVE)) == DRIVE_PARAM;
    int s = act.slotPub.load(std::memory_order_relaxed);
    // a window asked to learn (its own, or the Gain Stage's)
    if (s >= 0) {
        int64_t dl = shm::slot(s).dlearn.load();
        if (dl != dLearnSeen_) { dLearnSeen_ = dl; if (isParam) driveLearn(); }
    }
    if (!isParam) {
        dLearning_ = false;
        if (dstat_ != shm::DS_NONE) dstat_ = shm::DS_NONE;
        dLast_ = 1e9;
        return;
    }
    if (dLearning_) {
        ReaperBridge::Touched t = reaper.lastTouched();
        bool moved = t.ok && (t.fxGuid != dBase_.fxGuid || t.param != dBase_.param || t.norm != dBase_.norm);
        if (moved) {
            // not this plug-in's own parameters
            Placement pl = placement();
            double db;
            std::string nm;
            dTrack_ = t.trackGuid;
            dFx_ = t.fxGuid;
            dParam_ = t.param;
            ReaperBridge::LinkState ls = reaper.readDb(dTrack_, dFx_, dParam_, &db, &nm);
            dName_ = nm.empty() ? t.name : nm;
            dstat_ = ls == ReaperBridge::L_OK ? shm::DS_LINKED : (ls == ReaperBridge::L_NOTDB ? shm::DS_NOTDB : shm::DS_GONE);
            dLearning_ = false;
            dLast_ = 1e9;
            dirty_ = true;
            (void)pl;
        }
        return;
    }
    if (dFx_.empty()) { dstat_ = shm::DS_LEARNING; dBase_ = reaper.lastTouched(); dLearning_ = true; return; }
    // the first used channel's gain, minus what drive links did to it
    if (s < 0) return;
    shm::Slot &b = shm::slot(s);
    double g = b.p[P_GAIN].load(), dr = b.drvs[0].load();
    double gv = g > GFLOOR ? g - dr : GFLOOR;
    if (dLast_ > 1e8 || gv <= GFLOOR || dLast_ <= GFLOOR) { dLast_ = gv; }
    else if (gv != dLast_) {
        double delta = gv - dLast_;
        dLast_ = gv;
        if (P.get(P_DRIVEON) >= 0.5) {
            double cur;
            std::string nm;
            ReaperBridge::LinkState ls = reaper.readDb(dTrack_, dFx_, dParam_, &cur, &nm);
            if (ls == ReaperBridge::L_OK) ls = reaper.writeDb(dTrack_, dFx_, dParam_, cur - delta);
            if (!nm.empty()) dName_ = nm;
            dstat_ = ls == ReaperBridge::L_OK ? shm::DS_LINKED : (ls == ReaperBridge::L_NOTDB ? shm::DS_NOTDB : shm::DS_GONE);
        }
    }
}

void Plugin::mainTimer()
{
    ++tick_;
    if (dirty_.exchange(false) && hState_) hState_->mark_dirty(host);
    // the main thread's heartbeat: the slot stays this instance's while it is bypassed (no audio, no Act heartbeat)
    {
        int s = act.slotPub.load(std::memory_order_relaxed);
        int64_t id = act.idPub.load(std::memory_order_relaxed);
        if (s >= 0 && id >= 100000 && shm::slot(s).owner.load() == id && shm::slot(s).nonce.load() == act.mynonce)
            shm::slot(s).mheart.store(now_sec(), std::memory_order_relaxed);
    }
    // latency: settle for a quarter second (a drag on the lookahead time), then tell the host
    int want = latencyFromParams(srate_);
    double t = now_sec();
    if (want != latencyWant_) { latencyWant_ = want; latencyChangeT_ = t; }
    if (latencyWant_ != latencyReported_ && t - latencyChangeT_ > 0.25) {
        if (active_) {
            if (!restartAsked_) { restartAsked_ = true; host->request_restart(host); }
        } else {
            latencyReported_ = latencyWant_;
            if (hLatency_) hLatency_->changed(host);
        }
    }
    if (tick_ % 10 == 1) updatePlacement();
    driveTick();
    onMainTimer();
}

void Plugin::onMainThread()
{
    if (fallbackRun_) mainTimer();
}

// ======================================================================== params
uint32_t Plugin::paramsCount() { return (uint32_t)T.hostIds().size(); }

bool Plugin::paramsInfo(uint32_t index, clap_param_info *info)
{
    if (index >= T.hostIds().size()) return false;
    const ParamDef *d = T.get(T.hostIds()[index]);
    memset(info, 0, sizeof *info);
    info->id = (clap_id)d->id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (d->flags & PF_INT) info->flags |= CLAP_PARAM_IS_STEPPED;
    if (d->flags & PF_ENUM) info->flags |= CLAP_PARAM_IS_ENUM;
    if (d->flags & PF_HIDDEN) info->flags |= CLAP_PARAM_IS_HIDDEN;
    info->cookie = nullptr;
    snprintf(info->name, sizeof info->name, "%s", d->name);
    snprintf(info->module, sizeof info->module, "%s", d->module);
    info->min_value = d->min;
    info->max_value = d->max;
    info->default_value = d->def;
    return true;
}

bool Plugin::paramsValue(clap_id id, double *v)
{
    if (!T.get((int)id)) return false;
    *v = P.get((int)id);
    return true;
}

bool Plugin::paramsValueToText(clap_id id, double v, char *buf, uint32_t n)
{
    if (!T.get((int)id)) return false;
    T.format((int)id, v, buf, (int)n);
    return true;
}

bool Plugin::paramsTextToValue(clap_id id, const char *s, double *v)
{
    if (!T.get((int)id)) return false;
    return T.parse((int)id, s, v);
}

void Plugin::paramsFlush(const clap_input_events *in, const clap_output_events *out)
{
    handleIn(in);
    sendGui(out);
    sendAudio(out);
}

// ======================================================================== state
// Text: "TWOSTORMS <subkind> 1", then "p <id> <value>" per parameter, "ab <id> <value>" for the other
// A/B set, and "x <key> <value>" for anything else (the plug-in's and the drive link's).
bool Plugin::stateSave(const clap_ostream *os)
{
    std::string s;
    char line[256];
    snprintf(line, sizeof line, "TWOSTORMS %d 1\n", K.subkind);
    s += line;
    for (int id : T.ids()) {
        snprintf(line, sizeof line, "p %d %.17g\n", id, P.get(id));
        s += line;
    }
    for (int id : T.abIds()) {
        snprintf(line, sizeof line, "ab %d %.17g\n", id, AB_[id].load());
        s += line;
    }
    if (!dFx_.empty()) {
        s += "x drive_track " + dTrack_ + "\n";
        s += "x drive_fx " + dFx_ + "\n";
        snprintf(line, sizeof line, "x drive_param %d\n", dParam_);
        s += line;
        s += "x drive_name " + dName_ + "\n";
    }
    stateSaveExtra(s);
    const char *p = s.data();
    int64_t left = (int64_t)s.size();
    while (left > 0) {
        int64_t w = os->write(os, p, (uint64_t)left);
        if (w <= 0) return false;
        p += w;
        left -= w;
    }
    return true;
}

bool Plugin::stateLoad(const clap_istream *is)
{
    std::string s;
    char buf[4096];
    for (;;) {
        int64_t r = is->read(is, buf, sizeof buf);
        if (r < 0) return false;
        if (r == 0) break;
        s.append(buf, (size_t)r);
    }
    if (s.rfind("TWOSTORMS ", 0) != 0) return false;
    stateLoadBegin();
    size_t pos = 0;
    bool first = true;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        std::string l = s.substr(pos, e - pos);
        pos = e + 1;
        if (first) { first = false; continue; }
        if (l.size() > 2 && l[0] == 'p' && l[1] == ' ') {
            int id = 0;
            double v = 0;
            if (sscanf(l.c_str() + 2, "%d %lf", &id, &v) == 2 && T.get(id)) P.put(id, T.clamp(id, v));
        } else if (l.size() > 3 && l[0] == 'a' && l[1] == 'b' && l[2] == ' ') {
            int id = 0;
            double v = 0;
            if (sscanf(l.c_str() + 3, "%d %lf", &id, &v) == 2 && T.get(id)) AB_[id].store(T.clamp(id, v));
        } else if (l.size() > 2 && l[0] == 'x' && l[1] == ' ') {
            size_t sp = l.find(' ', 2);
            std::string k = l.substr(2, sp == std::string::npos ? std::string::npos : sp - 2);
            std::string v = sp == std::string::npos ? "" : l.substr(sp + 1);
            if (k == "drive_track") dTrack_ = v;
            else if (k == "drive_fx") dFx_ = v;
            else if (k == "drive_param") dParam_ = atoi(v.c_str());
            else if (k == "drive_name") dName_ = v;
            else stateLoadExtra(k, v);
        }
    }
    dLast_ = 1e9;
    if (hParams_) hParams_->rescan(host, CLAP_PARAM_RESCAN_VALUES);
    return true;
}

// ======================================================================== extensions
static const clap_plugin_audio_ports s_portsImpl = {
    [](const clap_plugin *p, bool isInput) -> uint32_t {
        Plugin *pl = (Plugin *)p->plugin_data;
        return isInput ? (pl->K.sidechain ? 2u : 1u) : 1u;
    },
    [](const clap_plugin *p, uint32_t index, bool isInput, clap_audio_port_info *info) -> bool {
        Plugin *pl = (Plugin *)p->plugin_data;
        uint32_t n = isInput ? (pl->K.sidechain ? 2u : 1u) : 1u;
        if (index >= n) return false;
        memset(info, 0, sizeof *info);
        info->id = index;
        info->channel_count = index == 0 ? (uint32_t)pl->K.nch() : 2u;
        info->port_type = info->channel_count == 2 ? CLAP_PORT_STEREO : nullptr;
        info->flags = CLAP_AUDIO_PORT_SUPPORTS_64BITS | CLAP_AUDIO_PORT_PREFERS_64BITS;
        if (index == 0) {
            info->flags |= CLAP_AUDIO_PORT_IS_MAIN;
            snprintf(info->name, sizeof info->name, isInput ? "Input" : "Output");
            info->in_place_pair = 0;
        } else {
            snprintf(info->name, sizeof info->name, "Sidechain");
            info->in_place_pair = CLAP_INVALID_ID;
        }
        return true;
    },
};
const clap_plugin_audio_ports Plugin::s_ports = s_portsImpl;

const clap_plugin_params Plugin::s_params = {
    [](const clap_plugin *p) { return self(p)->paramsCount(); },
    [](const clap_plugin *p, uint32_t i, clap_param_info *info) { return self(p)->paramsInfo(i, info); },
    [](const clap_plugin *p, clap_id id, double *v) { return self(p)->paramsValue(id, v); },
    [](const clap_plugin *p, clap_id id, double v, char *b, uint32_t n) { return self(p)->paramsValueToText(id, v, b, n); },
    [](const clap_plugin *p, clap_id id, const char *s, double *v) { return self(p)->paramsTextToValue(id, s, v); },
    [](const clap_plugin *p, const clap_input_events *in, const clap_output_events *out) { self(p)->paramsFlush(in, out); },
};

const clap_plugin_state Plugin::s_state = {
    [](const clap_plugin *p, const clap_ostream *s) { return self(p)->stateSave(s); },
    [](const clap_plugin *p, const clap_istream *s) { return self(p)->stateLoad(s); },
};

const clap_plugin_latency Plugin::s_latency = {
    [](const clap_plugin *p) -> uint32_t {
        Plugin *pl = self(p);
        pl->latencyReported_ = pl->latencyFromParams(pl->srate_);
        pl->latencyWant_ = pl->latencyReported_;
        pl->restartAsked_ = false;
        return (uint32_t)pl->latencyReported_;
    },
};

const clap_plugin_timer_support Plugin::s_timer = {
    [](const clap_plugin *p, clap_id id) {
        Plugin *pl = self(p);
        if (id == pl->timerId_) pl->mainTimer();
        else if (pl->editor_) pl->editor_->onTimer(id);
    },
};

const clap_plugin_posix_fd_support Plugin::s_fd = {
    [](const clap_plugin *p, int fd, clap_posix_fd_flags_t flags) {
        Plugin *pl = self(p);
        if (pl->editor_) pl->editor_->onFd(fd, flags);
    },
};

const clap_plugin_gui Plugin::s_gui = {
    [](const clap_plugin *, const char *api, bool floating) { return gui::Editor::apiSupported(api, floating); },
    [](const clap_plugin *, const char **api, bool *floating) {
        *api = gui::Editor::preferredApi();
        *floating = false;
        return true;
    },
    [](const clap_plugin *p, const char *api, bool floating) {
        Plugin *pl = self(p);
        if (pl->editor_) return false;
        pl->editor_.reset(new gui::Editor(*pl));
        if (!pl->editor_->create(api, floating)) { pl->editor_.reset(); return false; }
        return true;
    },
    [](const clap_plugin *p) {
        Plugin *pl = self(p);
        if (pl->editor_) { pl->editor_->destroy(); pl->editor_.reset(); }
    },
    [](const clap_plugin *p, double s) { Plugin *pl = self(p); return pl->editor_ ? pl->editor_->setScale(s) : false; },
    [](const clap_plugin *p, uint32_t *w, uint32_t *h) { Plugin *pl = self(p); return pl->editor_ ? pl->editor_->getSize(w, h) : false; },
    [](const clap_plugin *p) { Plugin *pl = self(p); return pl->editor_ ? pl->editor_->canResize() : false; },
    [](const clap_plugin *p, clap_gui_resize_hints *h) { Plugin *pl = self(p); return pl->editor_ ? pl->editor_->resizeHints(h) : false; },
    [](const clap_plugin *p, uint32_t *w, uint32_t *h) { Plugin *pl = self(p); return pl->editor_ ? pl->editor_->adjustSize(w, h) : false; },
    [](const clap_plugin *p, uint32_t w, uint32_t h) { Plugin *pl = self(p); return pl->editor_ ? pl->editor_->setSize(w, h) : false; },
    [](const clap_plugin *p, const clap_window *w) { Plugin *pl = self(p); return pl->editor_ ? pl->editor_->setParent(w) : false; },
    [](const clap_plugin *, const clap_window *) { return false; },
    [](const clap_plugin *, const char *) {},
    [](const clap_plugin *p) { Plugin *pl = self(p); return pl->editor_ ? pl->editor_->show() : false; },
    [](const clap_plugin *p) { Plugin *pl = self(p); return pl->editor_ ? pl->editor_->hide() : false; },
};

const clap_plugin_note_ports Plugin::s_notes = {
    [](const clap_plugin *p, bool) -> uint32_t { return self(p)->K.midi ? 1u : 0u; },
    [](const clap_plugin *p, uint32_t index, bool isInput, clap_note_port_info *info) -> bool {
        if (!self(p)->K.midi || index != 0) return false;
        memset(info, 0, sizeof *info);
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
        snprintf(info->name, sizeof info->name, isInput ? "MIDI in" : "MIDI out");
        return true;
    },
};

const void *Plugin::getExtension(const char *id)
{
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return K.midi ? &s_notes : nullptr;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &s_params;
    if (!strcmp(id, CLAP_EXT_STATE)) return &s_state;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &s_latency;
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &s_timer;
#ifndef _WIN32
    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &s_fd;
#endif
    if (!strcmp(id, CLAP_EXT_GUI)) return &s_gui;
    return nullptr;
}

} // namespace ts
