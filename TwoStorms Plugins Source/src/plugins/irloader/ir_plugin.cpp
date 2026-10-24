// TwoStorms IR Loader: the plug-in (parameters, presets, the IR files, the window). The sound is in ir_engine.cpp.
// Port of twostorms_ir_loader.jsfx. The IRs are read here on the main thread (never on the audio thread) and handed to
// the engine; they are kept in the project's state, so a project doesn't need the files.
#include "ir_plugin.h"
#include "ir_engine.h"
#include "../../gui/analyzer.h"
#include "../../gui/effect_view.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace ts {
namespace irl {

using namespace gui;

// ---------------------------------------------------------------- parameters
static void f_pct(double v, char *b, int n) { snprintf(b, n, "%.0f%%", v); }
static void f_dbs(double v, char *b, int n) { snprintf(b, n, "%+.1f dB", v); }
static void f_db0(double v, char *b, int n) { snprintf(b, n, "%.0f dB", v); }
static void f_hz(double v, char *b, int n)
{
    if (v >= 1000) snprintf(b, n, "%.2f kHz", v / 1000);
    else snprintf(b, n, "%.0f Hz", v);
}
static void f_pan(double v, char *b, int n)
{
    if (std::fabs(v) < 0.5) snprintf(b, n, "C");
    else snprintf(b, n, v < 0 ? "%.0f L" : "%.0f R", std::fabs(v));
}
static void f_ms2(double v, char *b, int n) { snprintf(b, n, "%.2f ms", v); }

static const char *const L_LAT[] = {"Zero latency", "64 samples", "128 samples", "256 samples"};
static const char *const L_INM[] = {"Stereo", "Mono (summed)"};
static const char *const L_SLOPE[] = {"12 dB/oct", "24 dB/oct", "48 dB/oct"};
static const char *const L_SM[] = {"Off", "1/48 oct", "1/24 oct", "1/12 oct", "1/6 oct", "1/3 oct"};

template <size_t N> static std::vector<const char *> labels(const char *const (&a)[N]) { return std::vector<const char *>(a, a + N); }

static const ParamTable &table()
{
    static std::deque<std::string> names;
    static ParamTable t = [] {
        ParamTable t;
        auto nm = [](const std::string &s) { names.push_back(s); return names.back().c_str(); };
        addActParams(t, true);
        const char *M = "IR Loader";
        const uint32_t H = PF_HOST | PF_PRESET;
        for (int s = 0; s < 2; ++s) {
            std::string L = "IR " + std::to_string(s + 1) + " ";
            const char *mod = nm("IR " + std::to_string(s + 1));
            auto n = [&](const char *x) { return nm(L + x); };
            t.addEnum(irp(s, K_ON), n("on"), {"Off", "On"}, s == 0 ? 1 : 0, H | PF_BOOL, mod);
            t.add(irp(s, K_LVL), n("level"), -24, 12, 0, H, f_dbs, mod).unit = "dB";
            t.add(irp(s, K_PAN), n("pan"), -100, 100, 0, H, f_pan, mod);
            t.add(irp(s, K_DLY), n("delay"), 0, 10, 0, H, f_ms2, mod).unit = "ms";
            t.addEnum(irp(s, K_PHS), n("phase"), {"Normal", "Inverted"}, 0, H, mod);
            t.addEnum(irp(s, K_HPON), n("high pass"), {"Off", "On"}, 0, H | PF_BOOL, mod);
            t.add(irp(s, K_HP), n("high pass frequency"), 20, 500, 80, H, f_hz, mod).unit = "Hz";
            t.addEnum(irp(s, K_LPON), n("low pass"), {"Off", "On"}, 0, H | PF_BOOL, mod);
            t.add(irp(s, K_LP), n("low pass frequency"), 1000, 20000, 8000, H, f_hz, mod).unit = "Hz";
            t.addEnum(irp(s, K_HPS), n("high pass slope"), labels(L_SLOPE), 0, H, mod);
            t.addEnum(irp(s, K_LPS), n("low pass slope"), labels(L_SLOPE), 0, H, mod);
            t.add(irp(s, K_WID), n("stereo width"), 0, 200, 100, H, f_pct, mod).unit = "%";
            t.addEnum(irp(s, K_SOLO), n("solo"), {"Off", "On"}, 0, PF_HOST | PF_BOOL, mod);
        }
        t.addEnum(P_LAT, "Latency", labels(L_LAT), 0, H, M);
        t.addEnum(P_INM, "Input", labels(L_INM), 0, H, M);
        t.addEnum(P_STIR, "Stereo IRs", {"Off (summed to mono)", "On"}, 1, H | PF_BOOL, M);
        t.addEnum(P_ALVL, "Two IRs: auto level", {"Off", "On"}, 1, H | PF_BOOL, M);
        t.add(P_MIX, "Dry/wet", 0, 100, 100, H, f_pct, M).unit = "%";
        t.addEnum(P_ALIGN, "Two IRs: align starts", {"Off", "On"}, 1, H | PF_BOOL, M);
        t.addEnum(P_MATCH, "Two IRs: match phase", {"Off", "On"}, 1, H | PF_BOOL, M);
        t.addEnum(P_TTON, "Tail trim", {"Off", "On"}, 1, H | PF_BOOL, M);
        t.add(P_TTDB, "Tail trim level", -120, -40, -90, H, f_db0, M).unit = "dB";
        t.addEnum(P_SMOOTH, "Response smoothing", labels(L_SM), 2, 0, M);
        t.addEnum(P_NORM, "Normalize", {"Off", "On"}, 1, H | PF_BOOL, M);
        return t;
    }();
    return t;
}

// factory presets: Instruments, Buses, Creative, Weird, Tone (the IRs themselves are what you load: these set
// the filters, levels, blend and the rest around them)
static std::string S(std::initializer_list<std::pair<int, double>> kv) { return presetStr(kv); }
static int I1(int k) { return irp(0, k); }
static int I2(int k) { return irp(1, k); }
static const std::vector<FactoryPreset> &factoryPresets()
{
    static std::vector<FactoryPreset> v = {
        // ---- guitar
        {"Instruments/Guitar/Clean: open cab (high pass 80, low pass 10k)", "55=1 56=80 57=1 58=10000 59=1"},
        {"Instruments/Guitar/Funk: tight and bright (high pass 120)", "55=1 56=120 57=1 58=9000 59=1"},
        {"Instruments/Guitar/Rock: classic cab (high pass 90, low pass 8k)", "55=1 56=90 57=1 58=8000 59=1 60=0"},
        {"Instruments/Guitar/Metal: tight cab (high pass 100, low pass 9k)", "55=1 56=100 57=1 58=9000 59=1 60=1"},
        {"Instruments/Guitar/Two cabs: even blend", S({{I1(K_HPON), 1}, {I1(K_HP), 90}, {I2(K_ON), 1}, {I2(K_HPON), 1}, {I2(K_HP), 90}})},
        {"Instruments/Guitar/Two mics: dynamic + darker ribbon", S({{I1(K_HPON), 1}, {I1(K_HP), 90}, {I2(K_ON), 1}, {I2(K_LVL), -4}, {I2(K_LPON), 1}, {I2(K_LP), 6000}})},
        {"Instruments/Guitar/Acoustic: body IR for a piezo (50%)", S({{I1(K_HPON), 1}, {I1(K_HP), 60}, {P_MIX, 50}})},
        // ---- bass
        {"Instruments/Bass/Clean: cab + DI blend", "55=1 56=35 57=1 58=6000 59=1 94=55"},
        {"Instruments/Bass/Distorted: full cab", "55=1 56=50 57=1 58=5000 59=1 60=1 94=100"},
        {"Instruments/Bass/Slap: open top", "55=1 56=35 57=1 58=10000 94=45"},
        {"Instruments/Bass/Punk", "55=1 56=50 57=1 58=6000 59=1 94=100"},
        {"Instruments/Bass/Pop punk", "55=1 56=40 57=1 58=7000 59=1 94=80"},
        {"Instruments/Bass/Metal", "55=1 56=60 57=1 58=7500 59=2 60=1 94=100"},
        {"Instruments/Bass/Rock", "55=1 56=45 57=1 58=6000 59=1 94=90"},
        {"Instruments/Bass/Jazz", "55=1 56=30 57=1 58=3500 60=1 94=60"},
        {"Instruments/Bass/Pop", "55=1 56=35 57=1 58=9000 59=1 94=70"},
        // ---- vocals
        {"Instruments/Vocals/Small room IR (15%)", S({{I1(K_HPON), 1}, {I1(K_HP), 200}, {I1(K_LPON), 1}, {I1(K_LP), 9000}, {P_MIX, 15}})},
        {"Instruments/Vocals/Through a speaker (radio, megaphone IR)", S({{I1(K_HPON), 1}, {I1(K_HP), 400}, {I1(K_HPS), 1}, {I1(K_LPON), 1}, {I1(K_LP), 3500}, {I1(K_LPS), 1}})},
        // ---- drums
        {"Instruments/Drums/Snare: room IR blend (20%)", S({{I1(K_HPON), 1}, {I1(K_HP), 150}, {P_MIX, 20}})},
        {"Instruments/Drums/Kick: sub IR under it (low pass 1k)", S({{I1(K_LPON), 1}, {I1(K_LP), 1000}, {I1(K_LPS), 2}, {P_MIX, 35}})},
        // ---- keys
        {"Instruments/Keys/Keys through an amp or rotary IR", S({{I1(K_HPON), 1}, {I1(K_HP), 60}, {I1(K_LPON), 1}, {I1(K_LP), 9000}, {P_MIX, 80}})},
        // ---- buses
        {"Buses/Instrument bus/Guitar bus: second cab, wide", S({{I1(K_PAN), -60}, {I2(K_ON), 1}, {I2(K_PAN), 60}, {I1(K_HPON), 1}, {I1(K_HP), 90}, {I2(K_HPON), 1}, {I2(K_HP), 90}})},
        {"Buses/Instrument bus/Drum bus: room IR (20%)", S({{I1(K_HPON), 1}, {I1(K_HP), 150}, {I1(K_LPON), 1}, {I1(K_LP), 8000}, {P_MIX, 20}})},
        {"Buses/Mixbus/Subtle space (10%, short tail)", S({{I1(K_HPON), 1}, {I1(K_HP), 250}, {P_MIX, 10}, {P_TTDB, -60}})},
        {"Buses/Master/Character IR (30%, no low end)", S({{I1(K_HPON), 1}, {I1(K_HP), 120}, {P_MIX, 30}, {P_TTDB, -60}})},
        // ---- creative
        {"Creative/Wide: two IRs hard left and right", S({{I1(K_PAN), -100}, {I2(K_ON), 1}, {I2(K_PAN), 100}})},
        {"Creative/Slap: IR 2 10 ms later", S({{I2(K_ON), 1}, {I2(K_DLY), 10}, {I2(K_LVL), -6}, {P_ALIGN, 0}, {P_MATCH, 0}})},
        {"Creative/Dark and far (low pass 2k, 40%)", S({{I1(K_LPON), 1}, {I1(K_LP), 2000}, {I1(K_LPS), 2}, {P_MIX, 40}})},
        {"Creative/Mono IR wide (stereo width 200%)", S({{I1(K_WID), 200}})},
        // ---- weird
        {"Weird/Cancelled cabs (IR 2 inverted, nothing matched)", S({{I2(K_ON), 1}, {I2(K_PHS), 1}, {P_ALIGN, 0}, {P_MATCH, 0}})},
        {"Weird/Comb: IR 2 half a millisecond late", S({{I2(K_ON), 1}, {I2(K_DLY), 0.5}, {P_ALIGN, 0}, {P_MATCH, 0}})},
        {"Weird/Narrow band (1-2 kHz only)", S({{I1(K_HPON), 1}, {I1(K_HP), 500}, {I1(K_HPS), 2}, {I1(K_LPON), 1}, {I1(K_LP), 2000}, {I1(K_LPS), 2}})},
        {"Weird/A drum hit as the IR (50%)", S({{P_MIX, 50}, {P_TTON, 0}})},
        // ---- the IR's own tone
        {"Tone/No filters (the IR as it is)", ""},
        {"Tone/Tight lows (high pass 80 Hz)", "55=1 56=80 59=1"},
        {"Tone/Dark (low pass 4 kHz)", "57=1 58=4000 60=1"},
        {"Tone/Short tail (trim at -60 dB)", "98=-60"},
        {"Tone/Raw level (no normalizing)", S({{P_NORM, 0}})},
    };
    return v;
}

// ---------------------------------------------------------------- base64 (an IR's samples in the project's state)
static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64enc(const unsigned char *p, size_t n)
{
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o += B64[(v >> 18) & 63];
        o += B64[(v >> 12) & 63];
        o += i + 1 < n ? B64[(v >> 6) & 63] : '=';
        o += i + 2 < n ? B64[v & 63] : '=';
    }
    return o;
}
static std::vector<unsigned char> b64dec(const std::string &s)
{
    static int T[256];
    static bool init = false;
    if (!init) {
        for (int &x : T) x = -1;
        for (int i = 0; i < 64; ++i) T[(unsigned char)B64[i]] = i;
        init = true;
    }
    std::vector<unsigned char> o;
    o.reserve(s.size() / 4 * 3);
    uint32_t v = 0;
    int bits = 0;
    for (unsigned char c : s) {
        if (T[c] < 0) continue;
        v = (v << 6) | (uint32_t)T[c];
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back((unsigned char)((v >> bits) & 255)); }
    }
    return o;
}

// ---------------------------------------------------------------- files
static std::string u8(const fs::path &p)
{
#if defined(__cpp_char8_t)
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
#else
    return p.u8string();
#endif
}
static fs::path fromU8(const std::string &s)
{
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}
static std::string lower(std::string s)
{
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static bool isWav(const fs::path &p) { return lower(u8(p.extension())) == ".wav"; }
static std::string baseName(const std::string &path)
{
    fs::path p = fromU8(path);
    return u8(p.stem());
}
// the .wav files in a folder, sorted (case-insensitive)
static std::vector<std::string> wavsIn(const std::string &dir)
{
    std::vector<std::string> v;
    std::error_code ec;
    for (auto it = fs::directory_iterator(fromU8(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && isWav(it->path())) v.push_back(u8(it->path().filename()));
    }
    std::sort(v.begin(), v.end(), [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
    return v;
}
static std::string homeDir()
{
#ifdef _WIN32
    const char *h = getenv("USERPROFILE");
#else
    const char *h = getenv("HOME");
#endif
    return h ? std::string(h) : std::string();
}

// ---------------------------------------------------------------- the plug-in
class IrPlugin : public Plugin, public EngineHost {
public:
    explicit IrPlugin(const clap_host *h) : Plugin(h, kind()), eng(*this, act, uiLink)
    {
        if (const char *b = getenv("TS_TEST_IRBUDGET")) eng.budgetMul = atof(b);
        // tests: an IR in each slot from the start (as the JSFX reads its file sliders before its first block)
        for (int s = 0; s < 2; ++s) {
            const char *e = getenv(s ? "TS_TEST_IR2" : "TS_TEST_IR1");
            if (e && *e) load(s, e);
        }
    }
    UiLink uiLink;
    Engine eng;
    // the IRs as the main thread keeps them: name, path (empty: only in the project), the samples (for the state)
    struct Slot {
        std::string name, path, err;
        std::shared_ptr<IrData> data;
        double errT = -100;
    } slot[2];
    double pv(int id) const override { return P.get(id); }
    gui::View *createView() override;

    // REAPER's Data/TwoStorms IRs folder (the JSFX's), or empty
    std::string irFolder()
    {
        std::string r = reaper.resourcePath();
        if (r.empty()) return std::string();
        fs::path p = fromU8(r) / "Data" / "TwoStorms IRs";
        std::error_code ec;
        return fs::is_directory(p, ec) ? u8(p) : std::string();
    }
    void hand(int s, const IrData &d) { eng.give(s, new IrData(d)); }
    bool load(int s, const std::string &path)
    {
        std::string err;
        IrData *d = readWav(path, err);
        if (!d) {
            slot[s].err = err;
            slot[s].errT = now_sec();
            return false;
        }
        slot[s].data.reset(d);
        slot[s].name = baseName(path);
        slot[s].path = path;
        slot[s].err.clear();
        hand(s, *d);
        markDirty();
        return true;
    }
    void clear(int s)
    {
        slot[s] = Slot();
        eng.give(s, nullptr);
        markDirty();
    }
    // step through the IR's folder (no IR, or one only in the project: the TwoStorms IRs folder)
    void step(int s, int d)
    {
        std::string dir;
        std::string cur;
        if (!slot[s].path.empty()) {
            fs::path p = fromU8(slot[s].path);
            dir = u8(p.parent_path());
            cur = u8(p.filename());
        } else {
            dir = irFolder();
        }
        if (dir.empty()) return;
        std::vector<std::string> v = wavsIn(dir);
        if (v.empty()) return;
        int i = -1;
        for (int k = 0; k < (int)v.size(); ++k)
            if (v[k] == cur) i = k;
        int n = (int)v.size();
        i = i < 0 ? (d > 0 ? 0 : n - 1) : (i + d + n) % n;
        load(s, u8(fromU8(dir) / fromU8(v[i])));
    }

protected:
    int latencyFromParams(double) const override { return latencyOf(P.get(P_LAT)); }
    void dspActivate(double sr, uint32_t) override { eng.init(sr); }
    void dspReset() override { eng.reset(); }
    void dspProcess(const double *const *in, const double *const *, double *const *out, int n) override
    {
        eng.block(n);
        act.block(n);
        eng.afterAct();
        eng.samples(in, out, n);
        act.process(out, 2, n);
        eng.post(out, n);
        act.audioKey(out, 2, n);
    }
    void onMainTimer() override { eng.collect(); }
    // the IRs themselves (as loaded: their own sample rate and channels, float), so the project doesn't need the files
    void stateSaveExtra(std::string &s) override
    {
        for (int k = 0; k < 2; ++k) {
            const Slot &sl = slot[k];
            if (!sl.data || sl.data->frames <= 0) continue;
            char b[160];
            s += "x ir" + std::to_string(k) + "name " + sl.name + "\n";
            if (!sl.path.empty()) s += "x ir" + std::to_string(k) + "path " + sl.path + "\n";
            const IrData &d = *sl.data;
            std::vector<float> f((size_t)d.frames * d.chans);
            for (int i = 0; i < d.frames; ++i)
                for (int c = 0; c < d.chans; ++c) f[(size_t)i * d.chans + c] = (float)d.d[c][i];
            snprintf(b, sizeof b, "x ir%ddata %d %d %.17g ", k, d.frames, d.chans, d.sr);
            s += b;
            s += b64enc((const unsigned char *)f.data(), f.size() * sizeof(float));
            s += "\n";
        }
    }
    void stateLoadBegin() override
    {
        for (int s = 0; s < 2; ++s) {
            slot[s] = Slot();
            eng.give(s, nullptr);
        }
    }
    void stateLoadExtra(const std::string &k, const std::string &v) override
    {
        if (k.size() < 4 || k.compare(0, 2, "ir") != 0) return;
        int s = k[2] == '1' ? 1 : 0;
        std::string what = k.substr(3);
        if (what == "name") slot[s].name = v;
        else if (what == "path") slot[s].path = v;
        else if (what == "data") {
            int fr = 0, ch = 0, used = 0;
            double sr = 0;
            if (sscanf(v.c_str(), "%d %d %lf %n", &fr, &ch, &sr, &used) < 3 || fr <= 0 || ch < 1 || ch > 2) return;
            std::vector<unsigned char> raw = b64dec(v.substr(used));
            if (raw.size() < (size_t)fr * ch * sizeof(float)) return;
            auto *d = new IrData();
            d->frames = std::min(fr, MAXR);
            d->chans = ch;
            d->sr = std::max(8000.0, sr);
            const float *f = (const float *)raw.data();
            for (int c = 0; c < ch; ++c) {
                d->d[c].resize(d->frames);
                for (int i = 0; i < d->frames; ++i) d->d[c][i] = f[(size_t)i * ch + c];
            }
            slot[s].data.reset(d);
            hand(s, *d);
        }
    }
    // a user preset keeps the IR files (by path): per slot 0 = none, 2 = a path (its length, then the characters)
    void presetExtraSave(std::vector<double> &x) override
    {
        for (int s = 0; s < 2; ++s) {
            if (!slot[s].data || slot[s].path.empty()) { x.push_back(0); continue; }
            const std::string &p = slot[s].path;
            int n = (int)std::min<size_t>(p.size(), 2000);
            x.push_back(2);
            x.push_back(n);
            for (int i = 0; i < n; ++i) x.push_back((unsigned char)p[i]);
        }
    }
    void presetExtraLoad(const std::vector<double> &x) override
    {
        size_t b = 0;
        for (int s = 0; s < 2 && b < x.size(); ++s) {
            int kd = (int)std::floor(x[b] + 0.5);
            if (kd == 2 && b + 1 < x.size()) {
                int n = (int)std::max(0.0, std::min(2000.0, std::floor(x[b + 1])));
                std::string p;
                for (int i = 0; i < n && b + 2 + i < x.size(); ++i) p += (char)(int)std::floor(x[b + 2 + i] + 0.5);
                load(s, p);
                b += 2 + n;
            } else {
                if (kd == 0) clear(s);
                b += kd == 1 ? 2 : 1;
            }
        }
    }
};

static Plugin *create(const clap_host *h) { return new IrPlugin(h); }

// ---------------------------------------------------------------- the window's numbers
// a complex FFT (radix 2, in place, interleaved) for the window's response curves (up to 2^18 points)
static void cfft(std::vector<double> &d, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(d[2 * i], d[2 * j]); std::swap(d[2 * i + 1], d[2 * j + 1]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2 * PI / len;
        double wlr = std::cos(ang), wli = std::sin(ang);
        int half = len >> 1;
        for (int i = 0; i < n; i += len) {
            double wr = 1, wi = 0;
            for (int k = 0; k < half; ++k) {
                double *a = &d[2 * (i + k)], *b = &d[2 * (i + k + half)];
                double xr = b[0] * wr - b[1] * wi, xi = b[0] * wi + b[1] * wr;
                b[0] = a[0] - xr; b[1] = a[1] - xi;
                a[0] += xr; a[1] += xi;
                double t = wr * wlr - wi * wli;
                wi = wr * wli + wi * wlr;
                wr = t;
            }
        }
    }
}

// a response: the power per FFT bin (left and right averaged), summed up (for smoothing over any frequency span)
struct Response {
    bool ok = false;
    int n = 0;                // FFT size
    double sr = 48000;
    std::vector<double> cum;  // cum[k]: the power of bins 0 .. k - 1
    void build(const std::vector<double> &l, const std::vector<double> &r, double g, double srate)
    {
        ok = false;
        size_t len = std::max(l.size(), r.size());
        if (len == 0) return;
        n = 65536;
        while ((size_t)n < 2 * len && n < 262144) n <<= 1;
        sr = srate;
        std::vector<double> d(2 * (size_t)n, 0.0);
        for (size_t i = 0; i < std::min(len, (size_t)n); ++i) {
            d[2 * i] = i < l.size() ? l[i] * g : 0;
            d[2 * i + 1] = (r.empty() ? (i < l.size() ? l[i] : 0) : (i < r.size() ? r[i] : 0)) * g;
        }
        cfft(d, n);
        cum.assign(n / 2 + 2, 0.0);
        for (int k = 0; k <= n / 2; ++k) {
            int j = (n - k) & (n - 1);
            double a = d[2 * k], b = d[2 * k + 1], c = d[2 * j], e = d[2 * j + 1];
            double p = (a * a + b * b + c * c + e * e) * 0.25;   // (|L|^2 + |R|^2) / 2
            cum[k + 1] = cum[k] + p;
        }
        ok = true;
    }
    // the average power from f0 to f1 (Hz), each bin covering half a bin either side of its centre
    double avg(double f0, double f1) const
    {
        double b0 = f0 * n / sr + 0.5, b1 = f1 * n / sr + 0.5;
        double top = n / 2 + 1;
        b0 = std::max(0.0, std::min(top, b0));
        b1 = std::max(b0 + 1e-9, std::min(top, b1));
        auto I = [&](double x) {
            int k = (int)std::floor(x);
            if (k >= (int)cum.size() - 1) return cum.back();
            return cum[k] + (x - k) * (cum[k + 1] - cum[k]);
        };
        return (I(b1) - I(b0)) / (b1 - b0);
    }
};

// ---------------------------------------------------------------- the window
static Col irCol(int s, float a = 1) { return (s == 0 ? th::purple() : th::green()).alpha(a); }

struct Entry {
    std::string name;
    bool dir;
};

class IrView : public EffectView {
public:
    explicit IrView(IrPlugin &p) : EffectView(p, p.uiLink, 1000, 700, "IR LOADER"), D(p)
    {
        SH = 196 + 30;
        help = {"Click an IR's name to browse for a .wav (or pick one from its folder); < and > step through its folder.",
#ifdef _WIN32
                "You can also drop .wav files on an IR panel. IRs are kept in the project; up to 2.7 s at 48 kHz.",
#else
                "IRs are kept in the project, so it doesn't need the files; up to 2.7 s at 48 kHz.",
#endif
                "IRs at another sample rate are resampled to the project's (windowed sinc). Zero latency, or 64..256 for less CPU.",
                "Normalize: one IR or two at 0 dB play as loud as the input (K-weighted, as LUFS); the levels act on top. Output sets the rest.",
                "Bypass (Lvl Match) plays the input at the same loudness. Shift + click Solo or Bypass: only while held."};
    }
    void frame(Ui &ui) override;
    void opened() override { view = IrShow(); }
    void filesDropped(const std::vector<std::string> &files, float x, float y) override;

protected:
    void hashMore(std::function<void(uint64_t)> mix) override
    {
        mix((uint64_t)view.seq | (uint64_t)D.eng.building() << 40);
        for (int s = 0; s < 2; ++s) {
            mix(std::hash<std::string>()(D.slot[s].name) ^ (uint64_t)(now_sec() - D.slot[s].errT < 4));
            mix((uint64_t)(D.slot[s].data ? D.slot[s].data->frames : 0));
        }
    }
    bool extraFull() override { return br.open; }

private:
        IrPlugin &D;
    IrShow view;
    struct WaveImg { uint64_t key = 0, pkKey = 0; double pk = 1e-12; std::vector<uint32_t> px; } wimg[2];   // the waveforms, drawn once
    // the waveforms' zoom, both IRs together: the wheel zooms in on time around the mouse (from t0 ms, zx times), Ctrl +
    // wheel on the height (zy times); a double-click shows it all again
    double zx_ = 1, zt0_ = 0, zy_ = 1, zclickT_ = -10;
    double slotPeak(int s);   // an IR's loudest sample (either channel), worked out once per IR
    double waveDiv(int s);    // what its samples are divided by on the picture (both IRs on one scale)
    Response rsp[3];
    uint32_t rspSeq = 0;
    // the browser
    struct Browser {
        bool open = false;
        int slot = 0;
        std::string dir, picked;
        std::vector<Entry> list;
        float scroll = 0;
        double clickT = -10;
        int clickI = -1;
    } br;

    void openBrowser(int s);
    void listDir(const std::string &dir);
    void browser(Ui &ui);
    void display(Ui &ui, const R &r);
    void meters(Ui &ui, const R &r);
    void panel(Ui &ui, int s, const R &r);
    void wave(Ui &ui, int s, const R &r);
    void nameMenu(Ui &ui, int s, const R &r);
    void globalRows(Ui &ui, float y);
};

gui::View *IrPlugin::createView() { return new IrView(*this); }

void IrView::filesDropped(const std::vector<std::string> &files, float x, float)
{
    // the first into the panel under the pointer, a second into the other one
    int s = x >= DW * 0.5f ? 1 : 0;
    for (size_t i = 0; i < files.size() && i < 2; ++i) {
        D.load(s, files[i]);
        s = 1 - s;
    }
}

// ---- the display: each IR's frequency response and both together (as they play), 20 Hz .. 20 kHz ----
void IrView::display(Ui &ui, const R &r)
{
    if (!ui.drawS()) return;
    FreqScale fsc(r);
    ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
    freqGrid(ui, r);
    // the curves, worked out again after each build or a smoothing change (one point per pixel)
    int sm = iclamp(iround(D.get(P_SMOOTH)), 0, 5);
    static const double OCT[6] = {0, 1.0 / 48, 1.0 / 24, 1.0 / 12, 1.0 / 6, 1.0 / 3};
    if (rspSeq != view.seq) {
        rspSeq = view.seq;
        for (int s = 0; s < 2; ++s) {
            if (view.used[s] && !view.spr[s][0].empty())
                rsp[s].build(view.spr[s][0], view.irpc[s] > 1 ? view.spr[s][1] : std::vector<double>(), std::fabs(view.irg[s]), view.srate);
            else rsp[s].ok = false;
        }
        if (!view.gl.empty() && (view.used[0] || view.used[1])) rsp[2].build(view.gl, view.gr, 1, view.srate);
        else rsp[2].ok = false;
    }
    int px0 = (int)std::floor(ui.X(r.x + 1)), px1 = (int)std::ceil(ui.X(r.r() - 1));
    int np = std::max(2, px1 - px0);
    std::vector<float> curve[3];
    double top = -1e9;
    for (int j = 0; j < 3; ++j) {
        if (!rsp[j].ok) continue;
        curve[j].resize(np);
        for (int i = 0; i < np; ++i) {
            double xd = (px0 + i + 0.5 - ui.ox) / ui.s;
            double f = fsc.f(xd);
            double fl = fsc.f((px0 + i - ui.ox) / ui.s), fh = fsc.f((px0 + i + 1 - ui.ox) / ui.s);
            double k = std::pow(2.0, OCT[sm] * 0.5);
            double f0 = std::min(fl, f / k), f1 = std::max(fh, f * k);
            double p = rsp[j].avg(f0, f1);
            double db = p > 1e-30 ? 10 * std::log10(p) : -300;
            curve[j][i] = (float)db;
            top = std::max(top, db);
        }
    }
    // the scale: 60 dB from +18 dB at the top, moved up in 6 dB steps when the curves are higher
    double rtop = 18;
    if (top > -1e8 && top + 3 > rtop) rtop = 6 * std::ceil((top + 3) / 6);
    auto Y = [&](double db) { return (float)(r.y + r.h * std::max(0.0, std::min(1.0, (rtop - db) / 60))); };
    for (int i = 0; i < 10; ++i) {
        double db = rtop - 6 - 6 * i;
        if (db < rtop - 60) break;
        ui.fill(R(r.x, Y(db), r.w, 1 / ui.s), Paint((i % 2) ? th::grid() : th::gridHi()));
        if (i % 2 == 0) {
            char s[16];
            if (std::fabs(db) < 0.5) snprintf(s, sizeof s, "0 dB");
            else snprintf(s, sizeof s, "%+.0f", db);
            ui.text(R(r.r() - 40, Y(db) - 12, 34, 11), s, th::faint(), F_REGULAR, 9.5f, 2);
        }
    }
    Canvas &c = ui.cv();
    c.pushClip(ui.X(r.x + 1), ui.Y(r.y + 1), ui.W(r.w - 2), ui.W(r.h - 2));
    std::vector<Pt> pts(np);
    std::vector<float> tops(np);
    if (!curve[2].empty()) {
        for (int i = 0; i < np; ++i) { float y = ui.Y(Y(curve[2][i])); tops[i] = y; pts[i] = {(float)(px0 + i) + 0.5f, y}; }
        c.columns(px0, tops.data(), np, ui.Y(r.b()), Paint::vertical(ui.Y(r.y), th::orange().alpha(0.22f), ui.Y(r.b()), th::orange().alpha(0.02f)));
    }
    for (int j = 0; j < 2; ++j) {
        if (curve[j].empty()) continue;
        for (int i = 0; i < np; ++i) pts[i] = {(float)(px0 + i) + 0.5f, ui.Y(Y(curve[j][i]))};
        c.strokeThin(pts.data(), np, ui.W(1.1f), irCol(j, 0.8f));
    }
    if (!curve[2].empty()) {
        for (int i = 0; i < np; ++i) pts[i] = {(float)(px0 + i) + 0.5f, ui.Y(Y(curve[2][i]))};
        c.strokeThin(pts.data(), np, ui.W(1.7f), th::orange());
    }
    freqLabels(ui, r);
    c.popClip();
    // the key, the smoothing
    float kx = r.x + 10;
    for (int j = 0; j < 3; ++j) {
        const char *t = j == 0 ? "IR 1" : (j == 1 ? "IR 2" : "Both (as they play)");
        Col cc = j < 2 ? irCol(j) : th::orange();
        ui.fill(R(kx, r.y + 12, 14, 2), Paint(cc));
        ui.text(R(kx + 18, r.y + 6, 140, 14), t, th::dim(), F_REGULAR, 11);
        kx += j < 2 ? 62 : 150;
    }
    if (D.eng.building()) ui.text(R(kx, r.y + 6, 100, 14), "building...", th::faint(), F_REGULAR, 11);
    enumDD(ui, R(r.r() - 170, r.y + 4, 164, 22), P_SMOOTH, L_SM, 6, "Smoothing");
    ui.stroke(r, Paint(th::border()), 6);
}

void IrView::meters(Ui &ui, const R &r)
{
    double range = av.meterRange();
    float top = r.y + 22, h = r.h - 22 - 10;
    float x = r.x + 6, ox = r.r() - 6 - 22;
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        ui.stroke(r, Paint(th::border()), 6);
        ui.text(R(x - 2, r.y + 6, 26, 12), "IN", th::faint(), F_SEMIBOLD, 10, 1);
        ui.text(R(ox - 2, r.y + 6, 26, 12), "OUT", th::faint(), F_SEMIBOLD, 10, 1);
    }
    if (!ui.drawD()) return;
    vMeter(ui, R(x, top, 10, h), D.eng.mi0.load(), live, range);
    vMeter(ui, R(x + 12, top, 10, h), D.eng.mi1.load(), live, range);
    double o0 = -200, o1 = -200, h0 = -200, h1 = -200;
    if (av.myslot >= 0) {
        shm::Slot &b = shm::slot(av.myslot);
        o0 = b.pkd[0].load(); o1 = b.pkd[1].load(); h0 = b.pkh[0].load(); h1 = b.pkh[1].load();
    }
    vMeter(ui, R(ox, top, 10, h), o0, live, range, h0);
    vMeter(ui, R(ox + 12, top, 10, h), o1, live, range, h1);
}

// ---- an IR's waveform, as a picture: drawn once when the IR, what's used of it or the window's size changes (8
// sub-columns per pixel, every sample counted, between samples when zoomed in that far; edges anti-aliased), then only
// copied to the screen. On a linear scale (its peak at full height): a stereo IR's left above the centre line, its right
// below (each its loudest); a mono one, its own shape. Bright where it's used (from its start, lined up, to where the
// tail trim ends it) ----
static void renderWave(std::vector<uint32_t> &out, int W, int H, float cy, float hh, const std::vector<double> &a,
                       const std::vector<double> &b, bool st2, double pk, int st, int en, bool used, const Col &cin,
                       const Col &cinB, const Col &cout, double v0, double v1)
{
    const int SS = 8;
    const int n = (int)a.size();
    std::vector<float> acc((size_t)W * H * 4, 0.f), col((size_t)H * 4, 0.f);
    auto at = [&](const std::vector<double> &v, double x) {
        int k = (int)std::floor(x);
        if (k < 0) return v[0];
        if (k >= n - 1) return v[n - 1];
        return v[k] + (v[k + 1] - v[k]) * (x - k);
    };
    auto span = [&](const std::vector<double> &v, double x0, double x1, double &mx, double &mn) {
        double e0 = at(v, x0), e1 = at(v, x1);
        mx = std::max(e0, e1);
        mn = std::min(e0, e1);
        int k0 = std::max(0, (int)std::ceil(x0)), k1 = std::min(n - 1, (int)std::floor(x1));
        for (int k = k0; k <= k1; ++k) { mx = std::max(mx, v[k]); mn = std::min(mn, v[k]); }
    };
    // a colour over part of this sub-column (rows y0..y1, fractional ends), by coverage times alpha, over what's there
    auto paint = [&](float y0, float y1, const Col &c, float al) {
        y0 = std::max(0.f, y0);
        y1 = std::min((float)H, y1);
        for (int y = (int)std::floor(y0); y < (int)std::ceil(y1); ++y) {
            float cov = std::min((float)y + 1, y1) - std::max((float)y, y0);
            if (cov <= 0) continue;
            float t = cov * al * c.a;
            float *p = &col[(size_t)y * 4];
            p[0] = c.r * t + p[0] * (1 - t); p[1] = c.g * t + p[1] * (1 - t); p[2] = c.b * t + p[2] * (1 - t); p[3] = t + p[3] * (1 - t);
        }
    };
    // the samples shown: v0 to v1 (zoomed: part of it)
    const double spp = (v1 - v0) / ((double)W * SS);
    const float ew = 1.0f;   // the edge's thickness, in pixels
    float pA = cy, pB = cy;
    for (int j = 0; j < W * SS; ++j) {
        double x0 = v0 + j * spp, x1 = std::min((double)(n - 1), v0 + (j + 1) * spp);
        if (x0 > n - 1) x0 = n - 1;
        double amx, amn, bmx, bmn;
        span(a, x0, x1, amx, amn);
        float top, bot;
        if (st2) {
            span(b, x0, x1, bmx, bmn);
            top = cy - hh * (float)(std::max(std::fabs(amx), std::fabs(amn)) / pk);
            bot = cy + hh * (float)(std::max(std::fabs(bmx), std::fabs(bmn)) / pk);
        } else {
            top = std::min(cy, cy - hh * (float)(amx / pk));
            bot = std::max(cy, cy - hh * (float)(amn / pk));
        }
        if (j == 0) { pA = top; pB = bot; }
        int k = (int)(0.5 * (x0 + x1));
        bool in = used && k >= st && k < en;
        const Col &ca = in ? cin : cout, &cb = (st2 && in) ? cinB : ca;
        float lo = std::max(0.f, std::min(top, pA) - ew), hi2 = std::min((float)H, std::max(bot, pB) + ew);
        int r0 = (int)std::floor(lo), r1 = (int)std::ceil(hi2);
        std::fill(col.begin() + (size_t)r0 * 4, col.begin() + (size_t)r1 * 4, 0.f);
        // the body (see-through), then the edges (joined to the last sub-column's)
        paint(top, cy, ca, 0.40f);
        paint(cy, bot, cb, 0.40f);
        paint(std::min(top, pA) - ew * 0.5f, std::max(top, pA) + ew * 0.5f, ca, 1.f);
        paint(std::min(bot, pB) - ew * 0.5f, std::max(bot, pB) + ew * 0.5f, cb, 1.f);
        pA = top;
        pB = bot;
        float *dst = &acc[(size_t)(j / SS) * 4];
        for (int y = r0; y < r1; ++y) {
            const float *c = &col[(size_t)y * 4];
            float *d = dst + (size_t)y * W * 4;
            d[0] += c[0]; d[1] += c[1]; d[2] += c[2]; d[3] += c[3];
        }
    }
    out.assign((size_t)W * H, 0);
    const float k = 255.f / SS;
    for (size_t i = 0; i < out.size(); ++i) {
        const float *c = &acc[i * 4];
        auto q = [&](float v) { return (uint32_t)std::max(0.f, std::min(255.f, v * k + 0.5f)); };
        out[i] = q(c[3]) << 24 | q(c[0]) << 16 | q(c[1]) << 8 | q(c[2]);
    }
}

double IrView::slotPeak(int s)
{
    const auto &a = view.irs[s][0], &b = view.irs[s][1].empty() ? view.irs[s][0] : view.irs[s][1];
    if (a.empty()) return 1e-12;
    int n = (int)a.size();
    uint64_t k = (uint64_t)(uintptr_t)D.slot[s].data.get() ^ ((uint64_t)n << 32) ^ (uint64_t)(a[n / 2] * 1e12) ^ (uint64_t)view.irl[s] * 0x9e3779b97f4a7c15ull;
    WaveImg &wi = wimg[s];
    if (wi.pkKey != k) {
        double pk = 1e-12;
        for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(a[i]), std::fabs(b[i])));
        wi.pk = pk;
        wi.pkKey = k;
    }
    return wi.pk;
}
// both IRs on one height scale, as they play (their levels, normalizing): the louder one's peak at the full height.
// One that doesn't play (off, or the other soloed) on its own scale, its peak at the full height. Ctrl + wheel zooms in
// on the height.
double IrView::waveDiv(int s)
{
    auto loud = [&](int t) { return slotPeak(t) * std::fabs(view.irg[t]); };
    if (!view.used[s] || std::fabs(view.irg[s]) <= 0) return slotPeak(s) / zy_;
    double mx = 0;
    for (int t = 0; t < 2; ++t)
        if (view.used[t] && !view.irs[t][0].empty()) mx = std::max(mx, loud(t));
    return std::max(1e-12, mx / (zy_ * std::fabs(view.irg[s])));
}

void IrView::wave(Ui &ui, int s, const R &r)
{
    ui.fill(r, Paint(th::well()), 5);
    const bool st2 = !view.irs[s][1].empty();
    const auto &a = view.irs[s][0], &b = st2 ? view.irs[s][1] : view.irs[s][0];
    bool has = D.slot[s].data && !a.empty();
    bool on = D.get(irp(s, K_ON)) >= 0.5;
    if (has) {
        int n = (int)a.size();
        int st = view.irst[s], en = view.irst[s] + view.irls[s];
        bool used = view.used[s];
        const float cy = r.y + r.h * 0.5f, hh = r.h * 0.5f - 2;
        const R wr0 = !view.irs[s][1].empty() ? R(r.x + 14, r.y, r.w - 14, r.h) : r;
        // the zoom: the wheel on time (the moment under the mouse stays put), Ctrl + wheel on the height
        if (ui.hit(r) && ui.wheel != 0 && !ui.used) {
            ui.used = true;
            if (ui.mods & M_CTRL) zy_ = std::max(1.0, std::min(32.0, zy_ * std::pow(1.25, ui.wheel)));
            else {
                double f = std::max(0.0, std::min(1.0, (double)((ui.mx() - wr0.x) / wr0.w)));
                double span = n / zx_, at = zt0_ * view.srate / 1000 + f * span;
                zx_ = std::max(1.0, std::min(512.0, zx_ * std::pow(1.25, ui.wheel)));
                double v0 = std::max(0.0, at - f * n / zx_);
                zt0_ = zx_ <= 1.0001 ? 0 : v0 / view.srate * 1000;
            }
        }
        if (ui.hit(r) && ui.lclick && !ui.used) {
            if (ui.now - zclickT_ < 0.35) { zx_ = 1; zt0_ = 0; zy_ = 1; zclickT_ = -10; ui.used = true; }
            else zclickT_ = ui.now;
        }
        double v0 = std::min((double)n - 2, zt0_ * view.srate / 1000), v1 = std::min((double)n - 1, v0 + (n - 1) / zx_);
        if (v1 - v0 < 4) v0 = std::max(0.0, v1 - 4);
        Canvas &c = ui.cv();
        ui.fill(R(r.x + 1, cy, r.w - 2, 1 / ui.s), Paint(th::grid()));
        // a stereo IR's L and R in a margin on the left
        const R wr = st2 ? R(r.x + 14, r.y, r.w - 14, r.h) : r;
        int px0 = (int)std::floor(ui.X(wr.x + 1)), px1 = (int)std::ceil(ui.X(wr.r() - 1));
        int py0 = (int)std::floor(ui.Y(r.y + 1)), py1 = (int)std::ceil(ui.Y(r.b() - 1));
        int W = std::max(1, px1 - px0), Hh = std::max(1, py1 - py0);
        // the picture: again only when what it shows changed
        uint64_t key = (uint64_t)(uintptr_t)D.slot[s].data.get();
        auto mix = [&](uint64_t v) { key ^= v + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2); };
        const double div = waveDiv(s);
        mix((uint64_t)n); mix((uint64_t)st); mix((uint64_t)en); mix(used); mix(on); mix(st2); mix((uint64_t)W); mix((uint64_t)Hh);
        mix((uint64_t)(ui.Y(cy) * 64)); mix(a.empty() ? 0 : (uint64_t)(a[n / 2] * 1e12)); mix((uint64_t)view.irl[s]);
        mix((uint64_t)(std::log(div) * 1000 + 1e6));
        mix((uint64_t)(v0 * 16)); mix((uint64_t)(v1 * 16));
        WaveImg &wi = wimg[s];
        if (wi.key != key || (int)wi.px.size() != W * Hh) {
            Col cin = irCol(s, on ? 1.f : 0.45f);
            renderWave(wi.px, W, Hh, ui.Y(cy) - py0, ui.W(hh), a, b, st2, div, st, en, used, cin, cin.lighter(0.45f), Col(0.30f, 0.29f, 0.29f), v0, v1);
            wi.key = key;
        }
        c.blit(px0, py0, W, Hh, wi.px.data());
        if (st2) {
            ui.text(R(r.x + 2, cy - hh * 0.5f - 6, 12, 12), "L", irCol(s, 0.9f), F_SEMIBOLD, 9.5f, 1);
            ui.text(R(r.x + 2, cy + hh * 0.5f - 6, 12, 12), "R", irCol(s, 0.9f).lighter(0.45f), F_SEMIBOLD, 9.5f, 1);
        }
        // where it starts and ends
        if (used) {
            float x0 = wr.x + 1, w = wr.w - 2;
            float xs = x0 + w * (float)((st - v0) / (v1 - v0)), xe = x0 + w * (float)((std::min(n, en) - v0) / (v1 - v0));
            if (st > 0 && xs >= x0 && xs <= x0 + w) ui.fill(R(xs, r.y, 1 / ui.s, r.h), Paint(th::orange()));
            if (en < n && xe >= x0 && xe <= x0 + w) ui.fill(R(xe, r.y, 1 / ui.s, r.h), Paint(th::orange()));
        }
        // what it is: its length (and what's used), its own rate (and the project's, when it's resampled), its channels
        const IrData &d = *D.slot[s].data;
        char inf[160], u2[48] = "";
        if (used && view.irls[s] > 0 && view.irls[s] < n - 1) snprintf(u2, sizeof u2, " (%.0f used)", view.irls[s] / view.srate * 1000);
        if (std::fabs(d.sr - view.srate) > 0.5)
            snprintf(inf, sizeof inf, "%.0f ms%s   \xC2\xB7   %.1f kHz, resampled to %.1f kHz   \xC2\xB7   %s", d.frames / d.sr * 1000, u2, d.sr / 1000,
                     view.srate / 1000, d.chans > 1 ? "stereo" : "mono");
        else
            snprintf(inf, sizeof inf, "%.0f ms%s   \xC2\xB7   %.1f kHz   \xC2\xB7   %s", d.frames / d.sr * 1000, u2, d.sr / 1000, d.chans > 1 ? "stereo" : "mono");
        if (zx_ > 1.0001 || zy_ > 1.0001) {
            char zb[96];
            snprintf(zb, sizeof zb, "   \xC2\xB7   %.1f to %.1f ms (double-click: all)", v0 / view.srate * 1000, v1 / view.srate * 1000);
            strncat(inf, zb, sizeof inf - strlen(inf) - 1);
        }
        float tw = ui.textW(F_REGULAR, 10, inf);
        R ib(r.r() - tw - 12, r.y + 2, tw + 10, 14);
        ui.fill(ib, Paint(Col(0.03f, 0.03f, 0.035f, 0.85f)), 3);
        ui.text(R(ib.x + 5, ib.y, ib.w - 6, ib.h), inf, th::dim(), F_REGULAR, 10);
        // normalized: by how much
        if (used && view.norm) {
            snprintf(inf, sizeof inf, "normalized %+.1f dB", 20 * std::log10(view.nrm[s]));
            tw = ui.textW(F_REGULAR, 10, inf);
            R nb2(r.r() - tw - 12, r.b() - 16, tw + 10, 14);
            ui.fill(nb2, Paint(Col(0.03f, 0.03f, 0.035f, 0.85f)), 3);
            ui.text(R(nb2.x + 5, nb2.y, nb2.w - 6, nb2.h), inf, th::dim(), F_REGULAR, 10);
        }
    } else {
        ui.text(r, D.slot[s].data ? "building..." : "no IR loaded", th::faint(), F_REGULAR, 11, 1);
    }
    ui.stroke(r, Paint(th::border()), 5);
}

// ---- the name box's menu: Browse..., the IRs in its folder, Clear ----
void IrView::nameMenu(Ui &ui, int s, const R &r)
{
    std::string dir, cur;
    if (!D.slot[s].path.empty()) {
        fs::path p = fromU8(D.slot[s].path);
        dir = u8(p.parent_path());
        cur = u8(p.filename());
    } else {
        dir = D.irFolder();
    }
    std::vector<std::string> files = dir.empty() ? std::vector<std::string>() : wavsIn(dir);
    std::vector<MenuItem> m;
    m.push_back(MenuItem::item("Browse...", 1));
    if (!files.empty()) {
        m.push_back(MenuItem::sep());
        int nf = std::min<int>((int)files.size(), 40);
        for (int i = 0; i < nf; ++i) m.push_back(MenuItem::item(u8(fromU8(files[i]).stem()), 100 + i, files[i] == cur));
        if ((int)files.size() > nf) m.push_back(MenuItem::item("More in the browser...", 1));
    }
    m.push_back(MenuItem::sep());
    m.push_back(MenuItem::item("Clear this IR", 2, false, (bool)D.slot[s].data));
    ui.menu(m, r.x, r.b() + 4, [this, s, dir, files](int k) {
        if (k == 1) openBrowser(s);
        else if (k == 2) D.clear(s);
        else if (k >= 100 && k - 100 < (int)files.size()) D.load(s, u8(fromU8(dir) / fromU8(files[k - 100])));
    }, std::max(r.w, 220.0f));
}

// ---- one IR's panel: on, name (click: browse, its folder's IRs, clear), previous / next, solo; its waveform; level,
// pan, delay, phase, width; high and low pass ----
void IrView::panel(Ui &ui, int s, const R &r)
{
    if (!ui.drawS()) return;
    const float h = 26;
    bool on = D.get(irp(s, K_ON)) >= 0.5;
    card(ui, r);
    ui.fill(R(r.x, r.y, r.w, 2), Paint(irCol(s)), 1);
    float x = r.x + 10, y = r.y + 10;
    if (ui.checkbox(R(x, y + 5, 16, 16), on)) D.setOnce(irp(s, K_ON), on ? 0 : 1);
    ui.text(R(x + 22, y, 40, h), s ? "IR 2" : "IR 1", irCol(s), F_BOLD, 13);
    if (ui.button(R(x + 64, y, 26, h), "<")) D.step(s, -1);
    R nb(x + 94, y, r.w - 20 - 94 - 30 - 82, h);
    {
        bool fail = now_sec() - D.slot[s].errT < 4;
        bool hv = ui.hit(nb);
        ui.fill(nb, Paint(hv ? th::hover() : th::well()), 5);
        ui.stroke(nb, Paint(hv ? th::borderHi() : th::border()), 5);
        std::string t;
        Col tc = th::text();
        if (fail) { t = "Couldn't read that file (" + D.slot[s].err + ")"; tc = th::red(); }
        else if (D.slot[s].data) t = D.slot[s].name.empty() ? "(an IR kept in the project)" : D.slot[s].name;
        else {
#ifdef _WIN32
            t = "Drop an IR (.wav) here, or click";
#else
            t = "Click to load an IR (.wav)";
#endif
            tc = th::faint();
        }
        ui.textFit(R(nb.x + 10, nb.y, nb.w - 30, nb.h), t.c_str(), tc, F_REGULAR, 12);
        ui.icon(IC_CHEVRON, nb.r() - 12, nb.y + nb.h * 0.5f, 9, th::dim());
        if (hv && ui.lclick && !ui.used && !ui.busy()) { ui.used = true; nameMenu(ui, s, nb); }
    }
    if (ui.button(R(nb.r() + 4, y, 26, h), ">")) D.step(s, 1);
    toggle(ui, R(r.r() - 10 - 78, y, 78, h), "Solo", irp(s, K_SOLO), BS_PURPLE, true);
    y += h + 8;
    wave(ui, s, R(r.x + 10, y, r.w - 20, 70));
    y += 70 + 10;
    const float cw = (r.w - 20 - 12) / 2, x2 = r.x + 10 + cw + 12;
    ui.paramRow(R(x, y, cw, h), "Level", irp(s, K_LVL), SC_LIN, 54, 72, on);
    ui.paramRow(R(x2, y, cw, h), "Pan", irp(s, K_PAN), SC_LIN, 40, 64, on);
    y += h + 6;
    ui.paramRow(R(x, y, cw, h), "Delay", irp(s, K_DLY), SC_LIN, 54, 72, on);
    toggle(ui, R(x2, y, 108, h), "Invert phase", irp(s, K_PHS));
    // a stereo IR's width (hidden for a mono IR, or with Stereo IRs off)
    bool wd = D.slot[s].data && D.slot[s].data->chans > 1 && D.get(P_STIR) >= 0.5;
    if (wd) ui.valueBox(R(x2 + 114, y, cw - 114, h), irp(s, K_WID), on, KD_OTHER, 1, 0.1, 2, nullptr, "Width");
    y += h + 6;
    // high and low pass, 12, 24 or 48 dB/oct (frequency and slope only while on)
    toggle(ui, R(x, y, 82, h), "High pass", irp(s, K_HPON));
    if (D.get(irp(s, K_HPON)) >= 0.5) {
        ui.valueBox(R(x + 86, y, 76, h), irp(s, K_HP), on, KD_LOG, std::log(25.0) / 120, std::log(25.0) / 1200);
        enumDD(ui, R(x + 166, y, cw - 166, h), irp(s, K_HPS), L_SLOPE, 3);
    }
    toggle(ui, R(x2, y, 82, h), "Low pass", irp(s, K_LPON));
    if (D.get(irp(s, K_LPON)) >= 0.5) {
        ui.valueBox(R(x2 + 86, y, 76, h), irp(s, K_LP), on, KD_LOG, std::log(20.0) / 120, std::log(20.0) / 1200);
        enumDD(ui, R(x2 + 166, y, cw - 166, h), irp(s, K_LPS), L_SLOPE, 3);
    }
}

// ---- both: latency, input, stereo IRs, the tail trim, dry/wet; then with two IRs, lining them up ----
void IrView::globalRows(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26;
    enumDD(ui, R(M, y, 150, h), P_LAT, L_LAT, 4);
    enumDD(ui, R(M + 156, y, 176, h), P_INM, L_INM, 2, "Input");
    toggle(ui, R(M + 338, y, 100, h), "Stereo IRs", P_STIR);
    toggle(ui, R(M + 444, y, 86, h), "Tail trim", P_TTON);
    if (D.get(P_TTON) >= 0.5) ui.valueBox(R(M + 534, y, 74, h), P_TTDB, true, KD_OTHER, 0.5, 0.1);
    toggle(ui, R(M + 618, y, 92, h), "Normalize", P_NORM);
    ui.paramRow(R(M + 722, y, DW - 2 * M - 722, h), "Dry/wet", P_MIX, SC_LIN, 62, 64);
    y += h + 8;
    if (D.slot[0].data && D.slot[1].data) {
        label(ui, R(M, y, 60, h), "Two IRs");
        toggle(ui, R(M + 62, y, 106, h), "Align starts", P_ALIGN);
        toggle(ui, R(M + 174, y, 106, h), "Match phase", P_MATCH);
        bool nrm = D.get(P_NORM) >= 0.5;
        if (!nrm) toggle(ui, R(M + 286, y, 96, h), "Auto level", P_ALVL);   // normalized, both are as loud already
        std::string t;
        char b[120];
        if (!D.eng.building() && view.used[0] && view.used[1]) {
            // what Align did: the silence it took off the front of each
            if (D.get(P_ALIGN) >= 0.5) {
                double a = view.irst[0] / view.srate * 1000, c = view.irst[1] / view.srate * 1000;
                if (std::fabs(a - c) < 0.05) snprintf(b, sizeof b, "starts already lined up");
                else snprintf(b, sizeof b, "starts lined up: IR 1 moved %.2f ms, IR 2 %.2f ms", a, c);
                t = b;
            }
            if (view.match) {
                if (!t.empty()) t += ";  ";
                snprintf(b, sizeof b, "IR 2 %s %.2f ms", view.alag >= 0 ? "later by" : "earlier by", std::fabs(view.alag) / view.srate * 1000);
                t += b;
                if (view.apol < 0) t += ", polarity flipped";
            }
            if (view.alvl) {
                snprintf(b, sizeof b, "%sIR 2 %+.1f dB to match IR 1's loudness", t.empty() ? "" : ";  ", view.alvg);
                t += b;
            }
            if (view.norm) {
                snprintf(b, sizeof b, "%sboth together %+.1f dB", t.empty() ? "" : ";  ", 20 * std::log10(view.nrk));
                t += b;
            }
        } else if (!(view.used[0] && view.used[1])) {
            t = "(both IRs on to line them up and match them)";
        }
        ui.textFit(R(M + 392, y, DW - M - (M + 392), h), t.c_str(), th::dim(), F_REGULAR, 11.5f);
    } else {
        ui.text(R(M, y, DW - 2 * M, h),
                D.get(P_NORM) >= 0.5 ? "Load a second IR to blend the two (lined up: start, timing, polarity; normalized together)."
                                     : "Load a second IR to line the two up (start, timing, polarity) and match their loudness.",
                th::faint(), F_REGULAR, 11.5f);
    }
}

// ---------------------------------------------------------------- the browser: anywhere on this computer
void IrView::listDir(const std::string &dir)
{
    br.dir = dir;
    br.list.clear();
    br.scroll = 0;
    std::error_code ec;
    std::vector<Entry> ds, fs2;
    for (auto it = fs::directory_iterator(fromU8(dir), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string nm = u8(it->path().filename());
        if (nm.empty() || nm[0] == '.') continue;
        std::error_code e2;
        if (it->is_directory(e2)) ds.push_back({nm, true});
        else if (it->is_regular_file(e2) && isWav(it->path())) fs2.push_back({nm, false});
    }
    auto cmp = [](const Entry &a, const Entry &b) { return lower(a.name) < lower(b.name); };
    std::sort(ds.begin(), ds.end(), cmp);
    std::sort(fs2.begin(), fs2.end(), cmp);
    br.list = ds;
    br.list.insert(br.list.end(), fs2.begin(), fs2.end());
}

void IrView::openBrowser(int s)
{
    br.open = true;
    br.slot = s;
    br.picked.clear();
    std::string d;
    if (!D.slot[s].path.empty()) d = u8(fromU8(D.slot[s].path).parent_path());
    if (d.empty() && !D.slot[1 - s].path.empty()) d = u8(fromU8(D.slot[1 - s].path).parent_path());
    if (d.empty()) d = D.irFolder();
    if (d.empty()) d = homeDir();
    std::error_code ec;
    if (!fs::is_directory(fromU8(d), ec)) d = homeDir();
    listDir(d);
    if (!D.slot[s].path.empty()) br.picked = u8(fromU8(D.slot[s].path).filename());
}

void IrView::browser(Ui &ui)
{
    if (!br.open || !ui.drawS()) return;
    ui.blockOn = false;
    R w(70, 72, DW - 140, DH - 120);
    ui.fill(R(0, 0, DW, DH), Paint(Col(0, 0, 0, 0.45f)));
    ui.shadow(w, 10, 24, Col(0, 0, 0, 0.75f));
    ui.fill(w, Paint::vertical(w.y, Col::hex(0x1a191d), w.b(), Col::hex(0x131215)), 10);
    ui.stroke(w, Paint(th::borderHi()), 10);
    const float h = 26;
    char t[64];
    snprintf(t, sizeof t, "LOAD IR %d", br.slot + 1);
    ui.text(R(w.x + 16, w.y + 12, 200, 16), t, irCol(br.slot), F_SEMIBOLD, 11);
    if (ui.button(R(w.r() - 16 - 80, w.y + 10, 80, h), "Done")) { br.open = false; return; }
    // places
    struct Place { std::string name, path; };
    std::vector<Place> pl;
    std::string irf = D.irFolder();
    if (!irf.empty()) pl.push_back({"TwoStorms IRs", irf});
    std::string home = homeDir();
    if (!home.empty()) {
        pl.push_back({"Home", home});
        for (const char *sub : {"Desktop", "Documents", "Downloads", "Music"}) {
            std::error_code ec;
            fs::path p = fromU8(home) / sub;
            if (fs::is_directory(p, ec)) pl.push_back({sub, u8(p)});
        }
    }
#ifdef _WIN32
    for (char d = 'C'; d <= 'Z'; ++d) {
        std::string root = std::string(1, d) + ":\\";
        std::error_code ec;
        if (fs::exists(fromU8(root), ec)) pl.push_back({root, root});
    }
#else
    pl.push_back({"Computer", "/"});
#endif
    for (int s = 0; s < 2; ++s)
        if (!D.slot[s].path.empty()) {
            std::string d = u8(fromU8(D.slot[s].path).parent_path());
            bool dup = false;
            for (auto &p : pl) if (p.path == d) dup = true;
            if (!dup) pl.push_back({std::string(s ? "IR 2's folder" : "IR 1's folder"), d});
        }
    float py = w.y + 44;
    ui.text(R(w.x + 16, py, 160, 14), "PLACES", th::faint(), F_SEMIBOLD, 10);
    py += 20;
    for (auto &p : pl) {
        R pr(w.x + 10, py, 170, 24);
        bool hv = ui.hit(pr), cur = br.dir == p.path;
        if (hv || cur) ui.fill(pr, Paint(cur ? th::raised2() : th::hover()), 5);
        ui.textFit(R(pr.x + 8, pr.y, pr.w - 12, pr.h), p.name.c_str(), cur ? th::text() : th::dim(), F_REGULAR, 12);
        if (hv && ui.lclick && !ui.used) { ui.used = true; listDir(p.path); }
        py += 26;
    }
    // the folder: its path, up, its folders and .wav files
    R lr(w.x + 192, w.y + 44, w.r() - 16 - (w.x + 192), w.b() - 16 - (w.y + 44));
    {
        R up(lr.x, lr.y, 60, h);
        fs::path cp = fromU8(br.dir);
        bool canUp = cp.has_parent_path() && cp.parent_path() != cp;
        if (ui.button(up, "Up", false, canUp)) listDir(u8(cp.parent_path()));
        R pb(lr.x + 66, lr.y, lr.w - 66, h);
        ui.fill(pb, Paint(th::well()), 5);
        ui.stroke(pb, Paint(th::border()), 5);
        ui.textFit(R(pb.x + 10, pb.y, pb.w - 16, pb.h), br.dir.c_str(), th::dim(), F_REGULAR, 11.5f);
    }
    R area(lr.x, lr.y + h + 8, lr.w, lr.h - h - 8);
    ui.fill(area, Paint(th::well()), 6);
    ui.stroke(area, Paint(th::border()), 6);
    const float rh = 24;
    float total = rh * (float)br.list.size();
    float maxs = std::max(0.0f, total - area.h + 8);
    if (ui.hit(area) && ui.wheel != 0 && !ui.used) { ui.used = true; br.scroll -= ui.wheel * rh * 3; }
    br.scroll = std::max(0.0f, std::min(maxs, br.scroll));
    ui.beginArea(R(area.x + 1, area.y + 1, area.w - 2, area.h - 2));
    if (br.list.empty()) ui.text(area, "No folders or .wav files here", th::faint(), F_REGULAR, 12, 1);
    for (int i = 0; i < (int)br.list.size(); ++i) {
        float yy = area.y + 4 + i * rh - br.scroll;
        if (yy + rh < area.y || yy > area.b()) continue;
        R er(area.x + 4, yy, area.w - 8, rh - 2);
        const Entry &e = br.list[i];
        bool hv = ui.hit(er), sel = !e.dir && e.name == br.picked && fromU8(br.dir) == fromU8(D.slot[br.slot].path).parent_path();
        if (hv || sel) ui.fill(er, Paint(sel ? irCol(br.slot, 0.22f) : th::hover()), 4);
        if (e.dir) {
            // a small folder
            ui.fill(R(er.x + 8, er.y + 7, 14, 10), Paint(th::faint()), 2);
            ui.fill(R(er.x + 8, er.y + 5, 6, 3), Paint(th::faint()), 1);
        } else {
            for (int k = 0; k < 5; ++k) {
                float bh = (float)(2 + 7 * std::fabs(std::sin(k * 1.7 + 0.6)));
                ui.fill(R(er.x + 8 + k * 3, er.y + er.h * 0.5f - bh * 0.5f, 2, bh), Paint(irCol(br.slot, 0.8f)), 1);
            }
        }
        ui.textFit(R(er.x + 30, er.y, er.w - 36, er.h), e.name.c_str(), e.dir ? th::dim() : th::text(), F_REGULAR, 12);
        if (hv && ui.lclick && !ui.used) {
            ui.used = true;
            std::string full = u8(fromU8(br.dir) / fromU8(e.name));
            if (e.dir) {
                listDir(full);
                break;
            }
            // a click loads it (hear it right away); a double-click loads it and closes
            bool dbl = ui.now - br.clickT < 0.35 && br.clickI == i;
            br.clickT = ui.now;
            br.clickI = i;
            if (D.load(br.slot, full)) br.picked = e.name;
            if (dbl) { br.open = false; break; }
        }
    }
    ui.endArea();
    // a click outside closes it
    if (ui.lclick && !ui.used && !ui.hitRaw(w) && !ui.busy()) { ui.used = true; br.open = false; }
}

void IrView::frame(Ui &ui)
{
    D.eng.snapshot(view);
    R gear = begin(ui, D.eng.latency() / std::max(1.0, D.sampleRate()) * 1000, false);
    if (br.open) ui.block(R(0, 0, DW, DH));
    float y = 66;
    const float dh = 230, metW = 62;
    display(ui, R(M, y, DW - 2 * M - metW - 6, dh));
    meters(ui, R(DW - M - metW, y, metW, dh));
    y += dh + 10;
    const float pw = (DW - 2 * M - 12) / 2, ph = 226;
    panel(ui, 0, R(M, y, pw, ph));
    panel(ui, 1, R(M + pw + 12, y, pw, ph));
    y += ph + 12;
    globalRows(ui, y);
    y += 26 + 8 + 26 + 12;
    actCardAt(ui, y);
    finish(ui, gear);
    browser(ui);
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_REVERB, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.irloader", "TwoStorms IR Loader", "TwoStorms", "", "", "", TS_VERSION,
         "An impulse-response loader with an Act built in: two IRs (.wav at any sample rate, resampled), blended, each with level, "
         "pan, delay, phase, width, high and low pass; with two, starts lined up and IR 2's timing, polarity and loudness matched. "
         "Tail trim, true zero latency (or 64..256 samples for less CPU).",
         s_features},
        &table,
        &factoryPresets,
        "IR Loader",
        "TWOSTORMS IR LOADER",
        shm::KIND_POINT,
        shm::SK_IRLOADER,
        false,
        false,
        0,
        &create,
        2,
        false,
    };
    return k;
}

} // namespace irl
} // namespace ts
