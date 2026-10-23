// Parameters. Every TwoStorms plug-in numbers its parameters like the JSFX sliders it came from
// (1..42 the built-in Act, 50.. the plug-in's own), and those numbers are the CLAP parameter IDs,
// the keys in saved state and presets, and the indices in shared memory. Old preset strings
// ("50=-24 51=4 ...") therefore load unchanged.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ts {

constexpr int MAXP = 256;

enum PFlag : uint32_t {
    PF_HOST = 1u << 0,   // a CLAP parameter: shown to the host, automatable
    PF_INT = 1u << 1,    // whole numbers only (enums, switches, counters)
    PF_AB = 1u << 2,     // part of the A/B set, and so of presets
    PF_ENUM = 1u << 3,   // labels[] name each value
    PF_BOOL = 1u << 4,   // an Off/On switch
    PF_HIDDEN = 1u << 5, // a CLAP parameter the host should not list (kept for automation of old projects)
    PF_PRESET = 1u << 6, // in presets, though not in the A/B set (the Saturator's oversampling: a held Swap never changes the latency)
};

typedef void (*FmtFn)(double v, char *buf, int n);
typedef bool (*ParseFn)(const char *s, double *out);

struct ParamDef {
    int id = 0;
    const char *name = "";
    const char *module = "";
    double min = 0, max = 1, def = 0;
    uint32_t flags = 0;
    std::vector<const char *> labels;
    FmtFn fmt = nullptr;   // value -> text, for the host and the window
    ParseFn tparse = nullptr;   // text -> value, where the text is not a number with a unit (packed settings)
    const char *unit = ""; // "ms": typed seconds ("1.5 s") are converted; "Hz": "2k" is 2000
    const char *minText = nullptr, *maxText = nullptr;   // words shown at the ends ("off", "no limit")
};

// One plug-in kind's parameter list (shared by all its instances).
class ParamTable {
public:
    ParamTable();
    ParamDef &add(int id, const char *name, double min, double max, double def, uint32_t flags,
                  FmtFn fmt = nullptr, const char *module = "");
    ParamDef &addEnum(int id, const char *name, std::vector<const char *> labels, double def, uint32_t flags,
                      const char *module = "");
    const ParamDef *get(int id) const { return (id > 0 && id < MAXP) ? defs_[id] : nullptr; }
    double clamp(int id, double v) const;
    double def(int id) const { const ParamDef *d = get(id); return d ? d->def : 0; }
    void format(int id, double v, char *buf, int n) const;
    bool parse(int id, const char *s, double *out) const;

    // in the order they were added
    const std::vector<int> &ids() const { return order_; }
    // the ones the host sees, in order
    const std::vector<int> &hostIds() const { return host_; }
    // the A/B set
    const std::vector<int> &abIds() const { return ab_; }
    // what a preset holds: the A/B set and the PF_PRESET ones
    const std::vector<int> &presetIds() const { return pre_; }
    int hostIndex(int id) const;

private:
    ParamDef *defs_[MAXP];
    std::vector<ParamDef *> owned_;
    std::vector<int> order_, host_, ab_, pre_;
};

// One instance's values. Read from any thread; written by the audio thread or the main thread
// through the plug-in (which tells the host).
class ParamValues {
public:
    explicit ParamValues(const ParamTable &t);
    double get(int id) const { return (id > 0 && id < MAXP) ? v_[id].load(std::memory_order_relaxed) : 0.0; }
    void put(int id, double v) { if (id > 0 && id < MAXP) v_[id].store(v, std::memory_order_relaxed); }
    void resetToDefaults();
    const ParamTable &table() const { return t_; }

private:
    const ParamTable &t_;
    std::atomic<double> v_[MAXP];
};

// common value formats
void fmt_db1(double v, char *b, int n);      // "-18.0 dB"
void fmt_db1s(double v, char *b, int n);     // "+1.5 dB"
void fmt_db2s(double v, char *b, int n);     // "+1.50 dB"
void fmt_gain(double v, char *b, int n);     // "+0.00 dB" / "-inf dB" at the floor (-100)
void fmt_pct0(double v, char *b, int n);     // "50%"
void fmt_ms(double v, char *b, int n);       // adaptive ms / s
void fmt_hz(double v, char *b, int n);       // "250 Hz" / "2.00k" / "10.0k"
void fmt_dbfs1(double v, char *b, int n);    // "-12.0 dBFS"
void fmt_int(double v, char *b, int n);

// a typed value: numbers with optional units, "k" for thousands, "inf" (returns -1000000 like the JSFX)
bool parse_number(const char *s, double *out);

} // namespace ts
