#include "comp_params.h"
#include "comp_engine.h"
#include "../../core/act.h"
#include <cstdio>

namespace ts {
namespace comp {

// the values as the JSFX window shows them (cfmt / cfmt0)
void fmt(int p, double v, char *b, int n)
{
    switch (p) {
    case P_FB: case P_BLEND: case P_SAT: case P_MIX: case P_SLINK:
        snprintf(b, n, "%.0f%%", v); return;
    case P_SCTL: case P_SCT: case P_IN:
        snprintf(b, n, "%+.1f dB", v); return;
    case P_SCHP:
        if (v <= 20.5) snprintf(b, n, "off");
        else snprintf(b, n, "%.0f Hz", v);
        return;
    case P_THR: case P_KNEE: case P_TGR:
        snprintf(b, n, "%.1f dB", v); return;
    case P_RNG:
        if (v >= 59.95) snprintf(b, n, "no limit");
        else snprintf(b, n, "%.1f dB", v);
        return;
    case P_RAT:
        if (v >= 99.5) snprintf(b, n, "inf : 1");
        else snprintf(b, n, v < 10 ? "%.2f : 1" : "%.1f : 1", v);
        return;
    case P_ATT:
        snprintf(b, n, v < 1 ? "%.2f ms" : (v < 10 ? "%.1f ms" : "%.0f ms"), v); return;
    case P_HLD:
        snprintf(b, n, "%.0f ms", v); return;
    case P_RMST:
        if (v < 1) snprintf(b, n, "%.2f ms", v);
        else if (v < 10) snprintf(b, n, "%.1f ms", v);
        else if (v < 1000) snprintf(b, n, "%.0f ms", v);
        else snprintf(b, n, "%.2f s", v / 1000);
        return;
    case P_REL:
        if (v < 1000) snprintf(b, n, "%.0f ms", v);
        else snprintf(b, n, "%.2f s", v / 1000);
        return;
    case P_LAMS:
        snprintf(b, n, "%.1f ms", v); return;
    default:
        if (v >= 10000) snprintf(b, n, "%.1fk", v / 1000);
        else if (v >= 1000) snprintf(b, n, "%.2fk", v / 1000);
        else snprintf(b, n, "%.0f Hz", v);
        return;
    }
}

#define F(P) [](double v, char *b, int n) { fmt(P, v, b, n); }

static ParamTable *build()
{
    ParamTable *t = new ParamTable();
    addActParams(*t, true);
    const uint32_t H = PF_HOST, AB = PF_AB;
    const char *DYN = "Dynamics", *DET = "Detector", *SC = "Sidechain", *OUT = "Output", *VO = "Voicing", *TR = "Trigger ranges";
    t->add(P_THR, "Threshold", -60, 0, -18, H | AB, F(P_THR), DYN);
    t->add(P_RAT, "Ratio", 1, 100, 4, H | AB, F(P_RAT), DYN);
    t->add(P_KNEE, "Knee", 0, 24, 6, H | AB, F(P_KNEE), DYN);
    t->add(P_ATT, "Attack", 0.01, 500, 10, H | AB, F(P_ATT), DYN).unit = "ms";
    t->add(P_HLD, "Hold", 0, 500, 0, H | AB, F(P_HLD), DYN).unit = "ms";
    t->add(P_REL, "Release", 1, 5000, 120, H | AB, F(P_REL), DYN).unit = "ms";
    t->add(P_RNG, "Range", 0, 60, 60, H | AB, F(P_RNG), DYN).maxText = "no limit";
    t->addEnum(P_DET, "Detection", {"Peak", "RMS", "Hybrid"}, 0, H | AB, DET);
    t->addEnum(P_CMODE, "Channel mode", {"Mono", "Stereo", "Mid/Side", "Left/Right"}, 1, H | AB, DET);
    t->add(P_SLINK, "Stereo link", 0, 100, 100, H | AB, F(P_SLINK), DET);
    t->add(P_IN, "Input", -24, 24, 0, H | AB, F(P_IN), OUT);
    t->add(P_MIX, "Dry/wet", 0, 100, 100, H | AB, F(P_MIX), OUT);
    t->add(P_AMK, "Auto makeup", 0, 1, 0, H | AB | PF_INT | PF_BOOL, nullptr, OUT);
    t->add(P_DELTA, "Delta", 0, 1, 0, H | PF_INT | PF_BOOL, nullptr, OUT);
    t->addEnum(P_SC, "Detector source", {"Main input", "Sidechain (3-4)"}, 0, H | AB, SC);
    t->add(P_LISTEN, "Detector listen", 0, 1, 0, H | PF_INT | PF_BOOL, nullptr, SC);
    t->add(P_LA, "Lookahead", 0, 1, 0, H | AB | PF_INT | PF_BOOL, nullptr, DYN);
    t->add(P_LAMS, "Lookahead time", 0.1, 20, 5, H | AB, F(P_LAMS), DYN).unit = "ms";
    t->add(P_TGR, "Target gain reduction", 0.5, 30, 4, AB, F(P_TGR), DYN);
    t->addEnum(P_TTYPE, "Target type", {"Peak GR", "Average GR"}, 0, AB, DYN);
    t->addEnum(P_TTIME, "Set threshold: listening time", {"1 s", "2 s", "3 s", "5 s", "10 s", "20 s", "30 s"}, 3, 0, DYN);
    t->addEnum(P_AB, "A/B set", {"A", "B"}, 0, 0, OUT);
    static const char *rn[4][3] = {
        {"Range 1 on", "Range 1 low", "Range 1 high"}, {"Range 2 on", "Range 2 low", "Range 2 high"},
        {"Range 3 on", "Range 3 low", "Range 3 high"}, {"Range 4 on", "Range 4 low", "Range 4 high"}};
    static const double rdef[4][2] = {{60, 250}, {2000, 5000}, {5000, 10000}, {250, 800}};
    for (int r = 0; r < 4; ++r) {
        t->add(P_R1 + 3 * r, rn[r][0], 0, 1, 0, H | AB | PF_INT | PF_BOOL, nullptr, TR);
        t->add(P_R1 + 3 * r + 1, rn[r][1], 20, 20000, rdef[r][0], H | AB, F(P_R1 + 1), TR);
        t->add(P_R1 + 3 * r + 2, rn[r][2], 20, 20000, rdef[r][1], H | AB, F(P_R1 + 2), TR);
    }
    t->add(P_CUIST, "UI state", 0, 1000000000, 0, PF_INT, nullptr, OUT);
    t->add(P_RMST, "RMS time", 0.01, 20000, 20, H | AB, F(P_RMST), DET).unit = "ms";
    t->addEnum(P_ENV, "Detector envelope", {"Raw", "Smooth"}, 1, H | AB, VO);
    t->addEnum(P_RELM, "Release mode", {"Classic", "Program", "2-stage"}, 1, H | AB, VO);
    t->add(P_FB, "Feedback", 0, 100, 0, H | AB, F(P_FB), VO);
    t->add(P_BLEND, "Hybrid RMS share", 0, 100, 50, H | AB, F(P_BLEND), DET);
    t->addEnum(P_LKL, "Stereo link law", {"Max", "Average (power)"}, 0, H | AB, DET);
    t->addEnum(P_RCMB, "Ranges combine", {"Loudest", "Combined"}, 0, H | AB, TR);
    t->add(P_SCHP, "Sidechain high-pass", 20, 1000, 20, H | AB, F(P_SCHP), SC).minText = "off";
    t->addEnum(P_SCHS, "Sidechain high-pass slope", {"6 dB/oct", "12 dB/oct", "24 dB/oct"}, 1, H | AB, SC);
    t->add(P_SCTL, "Sidechain tilt", -12, 12, 0, H | AB, F(P_SCTL), SC);
    t->add(P_SCT, "Sidechain trim", -24, 24, 0, H | AB, F(P_SCT), SC);
    t->addEnum(P_AMKM, "Auto makeup mode", {"Average GR", "Static"}, 0, H | AB, OUT);
    t->addEnum(P_CURVE, "Gain curve", {"Standard", "Soft", "Opto-like"}, 0, H | AB, VO);
    t->addEnum(P_CHAR, "Character", {"Custom", "Clean VCA", "Opto", "FET", "Vari-mu"}, 1, H | AB, VO);
    t->add(P_SAT, "Saturation", 0, 100, 0, H | AB, F(P_SAT), VO);
    t->addEnum(P_SATT, "Saturation type", {"Soft", "Warm"}, 0, H | AB, VO);
    t->addEnum(P_BASE, "Custom base", {"Clean VCA", "Opto", "FET", "Vari-mu"}, 0, AB, VO);
    return t;
}

const ParamTable &table()
{
    static ParamTable *t = build();
    return *t;
}

// factory presets: Instruments, Buses, Creative, Weird (every one starts from the defaults, then these)
static std::string S(std::initializer_list<std::pair<int, double>> kv) { return presetStr(kv); }
enum { VCA = 1, OPTO = 2, FET = 3, VARIMU = 4 };
const std::vector<FactoryPreset> &factoryPresets()
{
    static std::vector<FactoryPreset> v = {
        // ---- guitar
        {"Instruments/Guitar/Clean: opto 3-4 dB", S({{P_THR, -24}, {P_RAT, 3}, {P_KNEE, 10}, {P_ATT, 15}, {P_REL, 200}, {P_AMK, 1}, {P_TGR, 3.5}, {P_RELM, 2}, {P_FB, 50}, {P_SCHP, 100}, {P_CURVE, 2}, {P_CHAR, OPTO}, {P_SAT, 10}})},
        {"Instruments/Guitar/Clean: country squash (sustain)", S({{P_THR, -30}, {P_RAT, 6}, {P_KNEE, 6}, {P_ATT, 8}, {P_REL, 180}, {P_AMK, 1}, {P_TGR, 6}, {P_RELM, 1}, {P_SCHP, 150}, {P_CHAR, VCA}})},
        {"Instruments/Guitar/Funk: snappy squash", S({{P_THR, -26}, {P_RAT, 6}, {P_KNEE, 4}, {P_ATT, 2}, {P_REL, 60}, {P_AMK, 1}, {P_TGR, 6}, {P_RELM, 1}, {P_SCHP, 150}, {P_CHAR, FET}, {P_SAT, 10}})},
        {"Instruments/Guitar/Funk: let the pick through", S({{P_THR, -24}, {P_RAT, 4}, {P_KNEE, 6}, {P_ATT, 20}, {P_REL, 80}, {P_AMK, 1}, {P_TGR, 4}, {P_RELM, 1}, {P_SCHP, 150}, {P_CHAR, VCA}})},
        {"Instruments/Guitar/Rock: lead sustain", S({{P_THR, -26}, {P_RAT, 5}, {P_KNEE, 8}, {P_ATT, 10}, {P_REL, 250}, {P_AMK, 1}, {P_TGR, 6}, {P_RELM, 2}, {P_SCHP, 100}, {P_CHAR, OPTO}})},
        {"Instruments/Guitar/Metal: rhythm tightener", S({{P_THR, -16}, {P_RAT, 3}, {P_KNEE, 4}, {P_ATT, 15}, {P_REL, 60}, {P_AMK, 1}, {P_TGR, 2}, {P_RELM, 1}, {P_SCHP, 150}, {P_CHAR, FET}})},
        {"Instruments/Guitar/Metal: smooth lead (opto)", S({{P_THR, -22}, {P_RAT, 4}, {P_KNEE, 10}, {P_ATT, 8}, {P_REL, 180}, {P_AMK, 1}, {P_TGR, 4}, {P_RELM, 2}, {P_SCHP, 120}, {P_CHAR, OPTO}})},
        {"Instruments/Guitar/Acoustic: strummed (opto)", S({{P_THR, -24}, {P_RAT, 3}, {P_KNEE, 10}, {P_ATT, 15}, {P_REL, 200}, {P_AMK, 1}, {P_TGR, 4}, {P_RELM, 2}, {P_FB, 50}, {P_SCHP, 80}, {P_CURVE, 2}, {P_CHAR, OPTO}, {P_SAT, 15}})},
        {"Instruments/Guitar/Acoustic: fingerpicked (tame the plucks)", S({{P_THR, -20}, {P_RAT, 4}, {P_KNEE, 6}, {P_ATT, 1}, {P_REL, 90}, {P_AMK, 1}, {P_TGR, 3}, {P_RELM, 1}, {P_SCHP, 100}, {P_CHAR, VCA}})},
        // ---- bass
        {"Instruments/Bass/Clean: opto", S({{P_THR, -24}, {P_RAT, 4}, {P_KNEE, 8}, {P_ATT, 15}, {P_REL, 200}, {P_AMK, 1}, {P_TGR, 4}, {P_RELM, 2}, {P_FB, 50}, {P_SCHP, 20}, {P_CURVE, 2}, {P_CHAR, OPTO}, {P_SAT, 15}})},
        {"Instruments/Bass/Distorted: FET, steady", S({{P_THR, -20}, {P_RAT, 6}, {P_KNEE, 4}, {P_ATT, 5}, {P_REL, 80}, {P_AMK, 1}, {P_TGR, 5}, {P_RELM, 1}, {P_FB, 60}, {P_CHAR, FET}, {P_SAT, 35}})},
        {"Instruments/Bass/Slap: fast and tight", S({{P_THR, -22}, {P_RAT, 8}, {P_KNEE, 4}, {P_ATT, 1}, {P_HLD, 5}, {P_REL, 60}, {P_AMK, 1}, {P_TGR, 7}, {P_RELM, 1}, {P_FB, 60}, {P_CHAR, FET}, {P_SAT, 35}})},
        {"Instruments/Bass/Punk", S({{P_THR, -20}, {P_RAT, 6}, {P_KNEE, 4}, {P_ATT, 8}, {P_REL, 80}, {P_AMK, 1}, {P_TGR, 5}, {P_RELM, 1}, {P_FB, 60}, {P_CHAR, FET}, {P_SAT, 20}})},
        {"Instruments/Bass/Pop punk", S({{P_THR, -22}, {P_RAT, 5}, {P_KNEE, 6}, {P_ATT, 10}, {P_REL, 100}, {P_AMK, 1}, {P_TGR, 5}, {P_RELM, 1}, {P_CHAR, VCA}})},
        {"Instruments/Bass/Metal: locked in", S({{P_THR, -18}, {P_RAT, 10}, {P_KNEE, 2}, {P_ATT, 2}, {P_REL, 50}, {P_AMK, 1}, {P_TGR, 6}, {P_RELM, 1}, {P_FB, 60}, {P_CHAR, FET}, {P_SAT, 15}})},
        {"Instruments/Bass/Rock", S({{P_THR, -21}, {P_RAT, 4}, {P_KNEE, 6}, {P_ATT, 20}, {P_REL, 120}, {P_AMK, 1}, {P_TGR, 4}, {P_RELM, 1}, {P_FB, 60}, {P_CHAR, FET}, {P_SAT, 35}})},
        {"Instruments/Bass/Jazz: gentle", S({{P_THR, -26}, {P_RAT, 2.5}, {P_KNEE, 12}, {P_ATT, 30}, {P_REL, 300}, {P_AMK, 1}, {P_TGR, 3}, {P_RELM, 2}, {P_FB, 50}, {P_CURVE, 2}, {P_CHAR, OPTO}, {P_SAT, 15}})},
        {"Instruments/Bass/Pop: even", S({{P_THR, -24}, {P_RAT, 4}, {P_KNEE, 8}, {P_ATT, 12}, {P_REL, 100}, {P_AMK, 1}, {P_TGR, 5}, {P_RELM, 1}, {P_CHAR, VCA}})},
        {"Instruments/Bass/Synth bass: tight", S({{P_THR, -20}, {P_RAT, 6}, {P_KNEE, 4}, {P_ATT, 3}, {P_REL, 70}, {P_AMK, 1}, {P_TGR, 5}, {P_RELM, 1}, {P_CHAR, VCA}})},
        // ---- vocals
        {"Instruments/Vocals/Lead: singing (2-stage)", S({{P_THR, -24}, {P_RAT, 3}, {P_KNEE, 10}, {P_ATT, 10}, {P_REL, 150}, {P_AMK, 1}, {P_TGR, 5}, {P_TTYPE, 1}, {P_RELM, 2}, {P_FB, 50}, {P_SCHP, 100}, {P_CURVE, 2}, {P_CHAR, OPTO}, {P_SAT, 15}})},
        {"Instruments/Vocals/Lead: leveler (opto)", S({{P_THR, -26}, {P_RAT, 3}, {P_KNEE, 12}, {P_ATT, 15}, {P_REL, 250}, {P_AMK, 1}, {P_TGR, 4}, {P_TTYPE, 1}, {P_RELM, 2}, {P_FB, 50}, {P_SCHP, 100}, {P_CURVE, 2}, {P_CHAR, OPTO}, {P_SAT, 15}})},
        {"Instruments/Vocals/Lead: upfront rap (FET)", S({{P_THR, -22}, {P_RAT, 4}, {P_KNEE, 4}, {P_ATT, 3}, {P_REL, 80}, {P_AMK, 1}, {P_TGR, 6}, {P_RELM, 1}, {P_SCHP, 120}, {P_CHAR, FET}, {P_SAT, 15}})},
        {"Instruments/Vocals/Lead: rock scream (FET 8:1)", S({{P_THR, -20}, {P_RAT, 8}, {P_KNEE, 2}, {P_ATT, 1}, {P_REL, 60}, {P_AMK, 1}, {P_TGR, 8}, {P_RELM, 1}, {P_SCHP, 150}, {P_CHAR, FET}, {P_SAT, 25}})},
        {"Instruments/Vocals/Backing: tucked in", S({{P_THR, -26}, {P_RAT, 5}, {P_KNEE, 8}, {P_ATT, 5}, {P_REL, 120}, {P_AMK, 1}, {P_TGR, 6}, {P_RELM, 1}, {P_SCHP, 150}, {P_CHAR, OPTO}})},
        {"Instruments/Vocals/Spoken word / podcast", S({{P_THR, -26}, {P_RAT, 4}, {P_KNEE, 8}, {P_ATT, 8}, {P_REL, 120}, {P_AMK, 1}, {P_TGR, 5}, {P_RELM, 1}, {P_SCHP, 120}, {P_CHAR, VCA}})},
        // ---- drums
        {"Instruments/Drums/Kick: punch (slow attack)", S({{P_THR, -20}, {P_RAT, 4}, {P_KNEE, 4}, {P_ATT, 25}, {P_REL, 80}, {P_AMK, 1}, {P_TGR, 5}, {P_RELM, 1}, {P_CHAR, VCA}})},
        {"Instruments/Drums/Kick: tight and short", S({{P_THR, -22}, {P_RAT, 6}, {P_KNEE, 2}, {P_ATT, 10}, {P_HLD, 20}, {P_REL, 40}, {P_AMK, 1}, {P_TGR, 6}, {P_CHAR, VCA}})},
        {"Instruments/Drums/Snare: crack", S({{P_THR, -22}, {P_RAT, 5}, {P_KNEE, 4}, {P_ATT, 6}, {P_REL, 70}, {P_AMK, 1}, {P_TGR, 6}, {P_RELM, 1}, {P_FB, 60}, {P_CHAR, FET}, {P_SAT, 35}})},
        {"Instruments/Drums/Snare: fat body (fast attack)", S({{P_THR, -24}, {P_RAT, 4}, {P_KNEE, 6}, {P_ATT, 0.5}, {P_REL, 120}, {P_AMK, 1}, {P_TGR, 6}, {P_RELM, 1}, {P_CHAR, FET}, {P_SAT, 20}})},
        {"Instruments/Drums/Toms: sustain", S({{P_THR, -24}, {P_RAT, 4}, {P_KNEE, 6}, {P_ATT, 15}, {P_REL, 200}, {P_AMK, 1}, {P_TGR, 5}, {P_RELM, 1}, {P_CHAR, VCA}})},
        {"Instruments/Drums/Overheads: gentle", S({{P_THR, -18}, {P_RAT, 2}, {P_KNEE, 10}, {P_ATT, 30}, {P_REL, 150}, {P_AMK, 1}, {P_TGR, 2}, {P_RELM, 1}, {P_SCHP, 150}, {P_CHAR, VCA}})},
        {"Instruments/Drums/Room mics: smash", S({{P_THR, -30}, {P_RAT, 20}, {P_KNEE, 2}, {P_ATT, 0.3}, {P_REL, 100}, {P_AMK, 1}, {P_TGR, 12}, {P_RELM, 0}, {P_CHAR, FET}, {P_SAT, 30}})},
        {"Instruments/Drums/Hi-hat: even it out", S({{P_THR, -20}, {P_RAT, 3}, {P_KNEE, 6}, {P_ATT, 2}, {P_REL, 60}, {P_AMK, 1}, {P_TGR, 3}, {P_RELM, 1}, {P_SCHP, 300}, {P_CHAR, VCA}})},
        // ---- keys and others
        {"Instruments/Keys/Piano: gentle 2:1", S({{P_THR, -22}, {P_RAT, 2}, {P_KNEE, 12}, {P_ATT, 20}, {P_REL, 200}, {P_AMK, 1}, {P_TGR, 3}, {P_RELM, 2}, {P_CHAR, OPTO}})},
        {"Instruments/Keys/Electric piano: squeeze", S({{P_THR, -24}, {P_RAT, 4}, {P_KNEE, 6}, {P_ATT, 8}, {P_REL, 120}, {P_AMK, 1}, {P_TGR, 5}, {P_RELM, 1}, {P_CHAR, FET}, {P_SAT, 15}})},
        {"Instruments/Keys/Synth pad: smooth", S({{P_THR, -20}, {P_RAT, 2}, {P_KNEE, 12}, {P_ATT, 40}, {P_REL, 400}, {P_AMK, 1}, {P_TGR, 3}, {P_RELM, 2}, {P_CHAR, VARIMU}})},
        {"Instruments/Strings & brass/Strings: transparent", S({{P_THR, -20}, {P_RAT, 2}, {P_KNEE, 12}, {P_ATT, 30}, {P_REL, 300}, {P_AMK, 1}, {P_TGR, 2}, {P_RELM, 2}, {P_CHAR, OPTO}})},
        {"Instruments/Strings & brass/Brass: control the blasts", S({{P_THR, -18}, {P_RAT, 4}, {P_KNEE, 6}, {P_ATT, 5}, {P_REL, 120}, {P_AMK, 1}, {P_TGR, 4}, {P_RELM, 1}, {P_CHAR, VCA}})},
        // ---- buses
        {"Buses/Instrument bus/Drum bus: glue 2:1", S({{P_THR, -18}, {P_RAT, 2}, {P_KNEE, 6}, {P_ATT, 30}, {P_REL, 150}, {P_AMK, 1}, {P_TGR, 3}, {P_RELM, 1}, {P_SCHP, 80}, {P_CHAR, VCA}})},
        {"Buses/Instrument bus/Drum bus: parallel smash (40%)", S({{P_THR, -30}, {P_RAT, 20}, {P_KNEE, 2}, {P_ATT, 1}, {P_REL, 60}, {P_MIX, 40}, {P_AMK, 1}, {P_TGR, 12}, {P_RELM, 1}, {P_FB, 60}, {P_CHAR, FET}, {P_SAT, 30}})},
        {"Buses/Instrument bus/Guitar bus: glue 2:1", S({{P_THR, -18}, {P_RAT, 2}, {P_KNEE, 6}, {P_ATT, 30}, {P_REL, 150}, {P_AMK, 1}, {P_TGR, 2.5}, {P_RELM, 1}, {P_SCHP, 100}, {P_CHAR, VCA}})},
        {"Buses/Instrument bus/Vocal bus: blend the stack", S({{P_THR, -20}, {P_RAT, 3}, {P_KNEE, 10}, {P_ATT, 10}, {P_REL, 180}, {P_AMK, 1}, {P_TGR, 3}, {P_RELM, 2}, {P_SCHP, 120}, {P_CHAR, OPTO}})},
        {"Buses/Instrument bus/Keys and synths bus", S({{P_THR, -18}, {P_RAT, 2}, {P_KNEE, 10}, {P_ATT, 25}, {P_REL, 250}, {P_AMK, 1}, {P_TGR, 2}, {P_RELM, 2}, {P_CHAR, VARIMU}})},
        {"Buses/Mixbus/Glue: 2:1, 30 ms, auto release", S({{P_THR, -16}, {P_RAT, 2}, {P_KNEE, 6}, {P_ATT, 30}, {P_REL, 300}, {P_AMK, 1}, {P_TGR, 2}, {P_RELM, 1}, {P_SCHP, 60}, {P_CHAR, VCA}})},
        {"Buses/Mixbus/Vari-mu warmth", S({{P_THR, -14}, {P_RAT, 2}, {P_KNEE, 10}, {P_ATT, 30}, {P_REL, 300}, {P_AMK, 1}, {P_TGR, 2}, {P_TTYPE, 1}, {P_RELM, 1}, {P_FB, 40}, {P_SCHP, 60}, {P_CURVE, 1}, {P_CHAR, VARIMU}, {P_SAT, 30}, {P_SATT, 1}})},
        {"Buses/Mixbus/Punchy 4:1 (pop, EDM)", S({{P_THR, -18}, {P_RAT, 4}, {P_KNEE, 4}, {P_ATT, 10}, {P_REL, 100}, {P_AMK, 1}, {P_TGR, 4}, {P_RELM, 1}, {P_SCHP, 80}, {P_CHAR, VCA}})},
        {"Buses/Master/Gentle 1.5:1 (1 dB)", S({{P_THR, -12}, {P_RAT, 1.5}, {P_KNEE, 12}, {P_ATT, 30}, {P_REL, 400}, {P_AMK, 1}, {P_TGR, 1}, {P_TTYPE, 1}, {P_RELM, 1}, {P_SCHP, 60}, {P_CHAR, VCA}})},
        {"Buses/Master/Peak catcher", S({{P_THR, -8}, {P_RAT, 20}, {P_KNEE, 2}, {P_ATT, 0.5}, {P_REL, 50}, {P_AMK, 0}, {P_TGR, 3}, {P_CHAR, VCA}})},
        {"Buses/Master/Limiter (lookahead, inf:1)", S({{P_THR, -6}, {P_RAT, 100}, {P_KNEE, 0}, {P_ATT, 0.05}, {P_REL, 80}, {P_AMK, 0}, {P_LA, 1}, {P_LAMS, 3}, {P_TGR, 3}, {P_RELM, 1}, {P_CHAR, VCA}})},
        // ---- creative
        {"Creative/Sidechain pump (key on 3-4)", S({{P_THR, -30}, {P_RAT, 10}, {P_KNEE, 2}, {P_ATT, 0.5}, {P_REL, 180}, {P_SC, 1}, {P_TGR, 10}, {P_RELM, 0}, {P_CHAR, VCA}})},
        {"Creative/Breathing drums (pumps on every hit)", S({{P_THR, -30}, {P_RAT, 10}, {P_KNEE, 0}, {P_ATT, 0.3}, {P_REL, 300}, {P_AMK, 1}, {P_TGR, 14}, {P_RELM, 0}, {P_CHAR, FET}, {P_SAT, 25}})},
        {"Creative/Endless sustain (guitar, pads)", S({{P_THR, -40}, {P_RAT, 20}, {P_KNEE, 6}, {P_ATT, 5}, {P_REL, 800}, {P_AMK, 1}, {P_TGR, 18}, {P_RELM, 2}, {P_CHAR, OPTO}})},
        {"Creative/Swell (no attack: bowed feel)", S({{P_THR, -36}, {P_RAT, 30}, {P_KNEE, 0}, {P_ATT, 0.05}, {P_HLD, 60}, {P_REL, 600}, {P_AMK, 1}, {P_TGR, 16}, {P_RELM, 0}, {P_CHAR, VCA}})},
        {"Creative/Lo-fi squash (warm, crushed)", S({{P_THR, -34}, {P_RAT, 12}, {P_KNEE, 2}, {P_ATT, 1}, {P_REL, 120}, {P_AMK, 1}, {P_TGR, 12}, {P_RELM, 0}, {P_FB, 80}, {P_CHAR, FET}, {P_SAT, 70}, {P_SATT, 1}})},
        {"Creative/Duck only on the kick (low trigger)", S({{P_THR, -30}, {P_RAT, 8}, {P_KNEE, 2}, {P_ATT, 1}, {P_REL, 150}, {P_TGR, 8}, {P_R1, 1}, {P_R1 + 1, 40}, {P_R1 + 2, 120}, {P_CHAR, VCA}})},
        // ---- weird
        {"Weird/Waveshaper (release so fast it distorts)", S({{P_THR, -30}, {P_RAT, 100}, {P_KNEE, 0}, {P_ATT, 0.01}, {P_REL, 1}, {P_AMK, 1}, {P_TGR, 12}, {P_RELM, 0}, {P_ENV, 0}, {P_CHAR, VCA}})},
        {"Weird/Inside out (slow attack, instant release)", S({{P_THR, -36}, {P_RAT, 20}, {P_KNEE, 0}, {P_ATT, 300}, {P_REL, 5}, {P_AMK, 1}, {P_TGR, 14}, {P_RELM, 0}, {P_CHAR, VCA}})},
        {"Weird/Cymbals duck the whole mix (high trigger)", S({{P_THR, -34}, {P_RAT, 20}, {P_KNEE, 0}, {P_ATT, 0.5}, {P_REL, 250}, {P_TGR, 12}, {P_R1 + 6, 1}, {P_R1 + 7, 6000}, {P_R1 + 8, 14000}, {P_CHAR, VCA}})},
        {"Weird/Seasick (slow, heavy, feedback)", S({{P_THR, -36}, {P_RAT, 8}, {P_KNEE, 2}, {P_ATT, 200}, {P_REL, 2000}, {P_AMK, 1}, {P_TGR, 14}, {P_RELM, 0}, {P_FB, 100}, {P_CHAR, VARIMU}})},
        {"Weird/Gasping room (huge GR, instant recovery)", S({{P_THR, -50}, {P_RAT, 100}, {P_KNEE, 0}, {P_ATT, 0.01}, {P_REL, 20}, {P_AMK, 1}, {P_TGR, 24}, {P_RELM, 0}, {P_CHAR, FET}, {P_SAT, 40}})},
    };
    return v;
}

} // namespace comp
} // namespace ts
