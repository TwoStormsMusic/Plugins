// TwoStorms Gain Stage: put it first in a chain. ON/OFF (glided; made for ReaLearn scenes) and input
// calibration gain with its own meter and Auto. Tabs: Main lists every TwoStorms plug-in in this chain;
// Stages lists the input calibration of every Gain Stage in the project; Finales lists each chain's finale.
// Each card is a remote for that instance: meter, gain, Auto (Target, Match, Reference), Time, Hide, and a
// compressor's, EQ's, multiband's or wah's Set row. Port of twostorms_gain_stage.jsfx (1.0.1).
#include "gs_plugin.h"
#include "../../core/act_view.h"
#include "../../core/clock.h"
#include "../../core/eel.h"
#include "../../gui/common.h"
#include "../../gui/editor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <sstream>

namespace ts {
namespace gs {

using namespace gui;
using shm::GS_ROWS;
using shm::GS_W;

// the Set rows' parameters on other plug-ins (as they publish them)
enum { P_MBT = 56, P_TGR = 69, P_TTYPE = 70, P_TTIME = 71 };

static void fmt_ms0(double v, char *b, int n) { snprintf(b, n, "%.0f ms", v); }
static void fmt_db1(double v, char *b, int n) { snprintf(b, n, "%.1f dB", v); }
static void fmt_b1(double v, char *b, int n) { snprintf(b, n, "%.1f", v); }

static const ParamTable &table()
{
    static ParamTable t = [] {
        ParamTable t;
        addActParams(t, false);
        const char *M = "Gain Stage";
        t.add(P_GS_ON, "ON/OFF", 0, 1, 1, PF_HOST | PF_INT | PF_BOOL, nullptr, M);
        t.add(P_GS_GLIDE, "ON/OFF glide", 0, 3000, 20, PF_INT, fmt_ms0, M).unit = "ms";
        t.add(P_GS_SETS, "Set all value, Stages", GFLOOR, 60, 0, 0, fmt_gain, M);
        t.add(P_GS_SETF, "Set all value, Finales", GFLOOR, 60, 0, 0, fmt_gain, M);
        // not this plug-in's: the ranges of what the Set rows edit on other plug-ins
        const char *R = "Remote";
        static const char *bn[4] = {"Band 1 target", "Band 2 target", "Band 3 target", "Band 4 target"};
        for (int b = 0; b < 4; ++b) t.add(P_MBT + b, bn[b], 0.5, 30, 3, 0, fmt_b1, R);
        t.add(P_TGR, "Target gain reduction", 0.5, 30, 4, 0, fmt_db1, R);
        t.addEnum(P_TTYPE, "Target type", {"Peak GR", "Average GR"}, 0, 0, R);
        t.addEnum(P_TTIME, "Listening time", {"1 s", "2 s", "3 s", "5 s", "10 s", "20 s", "30 s"}, 3, 0, R);
        return t;
    }();
    return t;
}

static const std::vector<FactoryPreset> &noPresets()
{
    static std::vector<FactoryPreset> v;
    return v;
}

// ======================================================================== the tables
// Kept with the project: MST[0] Main table size, [1] Main reference, [4] tab, [5] settings open, [6] show hidden
// cards, [7..9] Auto all run per tab, [10] shared table size, [11] the Finales reference last seen.
// Main table rows (this Gain Stage only): id, hidden on Main. Shared table rows (the same in every Gain Stage, see
// shm::GsTable): id, hidden on Stages, hidden on Finales, reference on Stages, reference on Finales, time of the
// last change, left out of Auto all (Main), Set row shown.
struct Tables {
    double MST[16] = {};
    std::vector<std::array<double, 2>> TBL;
    std::vector<std::array<double, GS_W>> STB;
    double shver = -1, shmax = 0;
    Plugin *P = nullptr;

    Tables() { MST[6] = 1; }   // hidden cards are shown (dimmed) until you switch that off

    void changed() { if (P) P->markDirty(); }

    int tblFind(double id) const
    {
        int f = -1;
        for (size_t i = 0; i < TBL.size(); ++i) if (TBL[i][0] == id) f = (int)i;
        return f;
    }
    double tblGet(double id) const { int i = tblFind(id); return i >= 0 ? TBL[i][1] : 0; }
    void tblSet(double id, double v)
    {
        int i = tblFind(id);
        if (i < 0 && ne(v, 0) && TBL.size() < 1024) { TBL.push_back({id, 0}); i = (int)TBL.size() - 1; }
        if (i >= 0) {
            TBL[i][1] = v;
            if (eq(v, 0)) {
                TBL[i] = TBL.back();
                TBL.pop_back();
            }
        }
        MST[0] = (double)TBL.size();
        changed();
    }

    // ---- shared table: a copy is saved in every Gain Stage and kept in step through shared memory ----
    static shm::GsTable &G() { return shm::get()->gs; }
    struct Lock {
        Lock() { auto &l = G().lock; int32_t z = 0; while (!l.compare_exchange_weak(z, 1, std::memory_order_acquire)) z = 0; }
        ~Lock() { G().lock.store(0, std::memory_order_release); }
    };
    static double grow(int j, int k) { return G().row[j * GS_W + k].load(std::memory_order_relaxed); }
    int shFind(double id) const
    {
        int f = -1;
        for (size_t i = 0; i < STB.size(); ++i) if (STB[i][0] == id) f = (int)i;
        return f;
    }
    double shGet(double id, int fld) const { int i = shFind(id); return i >= 0 ? STB[i][fld] : 0; }
    static int shGfind(double id)
    {
        int f = -1, n = (int)std::min<int64_t>(GS_ROWS, G().n.load());
        for (int j = 0; j < n; ++j) if (grow(j, 0) == id) f = j;
        return f;
    }
    bool shEmpty(int i) const
    {
        const auto &r = STB[i];
        return eq(r[1], 0) && eq(r[2], 0) && eq(r[3], 0) && eq(r[4], 0) && eq(r[6], 0) && eq(r[7], 0);
    }
    // change times: wall clock in ms, always later than any change already seen
    double shNow()
    {
        shmax = std::max((double)std::time(nullptr) * 1000, shmax + 1);
        return shmax;
    }
    void shPush(int i)
    {
        shm::GsTable &g = G();
        int j = shGfind(STB[i][0]);
        if (j < 0 && g.n.load() < GS_ROWS) { j = (int)g.n.load(); g.n.store(j + 1); }
        if (j >= 0) {
            for (int k = 0; k < GS_W; ++k) g.row[j * GS_W + k].store(STB[i][k], std::memory_order_relaxed);
            g.ver.fetch_add(1);
        }
    }
    // a row for a new instance: append, or reuse the oldest row with nothing set
    int shNewrow()
    {
        if ((int)STB.size() < GS_ROWS) {
            STB.push_back({});
            MST[10] = (double)STB.size();
            return (int)STB.size() - 1;
        }
        int best = -1;
        double bt = 1e300;
        for (size_t i = 0; i < STB.size(); ++i)
            if (shEmpty((int)i) && STB[i][5] < bt) { bt = STB[i][5]; best = (int)i; }
        return best;
    }
    void shSet(double id, int fld, double v)
    {
        {
            Lock l;
            int i = shFind(id);
            if (i < 0 && ne(v, 0)) {
                i = shNewrow();
                if (i >= 0) { STB[i][0] = id; for (int k = 1; k < GS_W; ++k) STB[i][k] = 0; }
            }
            if (i >= 0) {
                STB[i][fld] = v;
                STB[i][5] = shNow();
                shPush(i);
            }
            shver = (double)G().ver.load();
        }
        changed();
    }
    // two-way merge: for each instance, the most recent change wins
    void shSync()
    {
        Lock l;
        shm::GsTable &g = G();
        int n = (int)std::min<int64_t>(GS_ROWS, g.n.load());
        for (int j = 0; j < n; ++j) {
            double gs5 = grow(j, 5);
            shmax = std::max(shmax, gs5);
            int i = shFind(grow(j, 0));
            if (i < 0) { i = shNewrow(); if (i >= 0) STB[i][5] = -1; }
            if (i >= 0 && gs5 > STB[i][5]) for (int k = 0; k < GS_W; ++k) STB[i][k] = grow(j, k);
        }
        for (size_t i = 0; i < STB.size(); ++i) {
            shmax = std::max(shmax, STB[i][5]);
            int j = shGfind(STB[i][0]);
            if (j < 0 || STB[i][5] > grow(j, 5)) shPush((int)i);
        }
        shver = (double)g.ver.load();
        MST[10] = (double)STB.size();
    }

    // ---- the project state (the JSFX's @serialize, version 3) ----
    void save(std::string &s)
    {
        shSync();
        std::ostringstream o;
        o.precision(17);
        o << "x gs_mst";
        for (double v : MST) o << ' ' << v;
        o << "\nx gs_tbl";
        for (auto &r : TBL) o << ' ' << r[0] << ' ' << r[1];
        o << "\nx gs_stb";
        for (auto &r : STB) for (double v : r) o << ' ' << v;
        o << '\n';
        s += o.str();
    }
    void load(const std::string &k, const std::string &v)
    {
        std::istringstream in(v);
        std::vector<double> x;
        double d;
        while (in >> d) x.push_back(d);
        if (k == "gs_mst") {
            for (size_t i = 0; i < 16 && i < x.size(); ++i) MST[i] = x[i];
        } else if (k == "gs_tbl") {
            TBL.clear();
            for (size_t i = 0; i + 1 < x.size() && TBL.size() < 1024; i += 2) TBL.push_back({x[i], x[i + 1]});
            MST[0] = (double)TBL.size();
        } else if (k == "gs_stb") {
            STB.clear();
            for (size_t i = 0; i + GS_W - 1 < x.size() && (int)STB.size() < GS_ROWS; i += GS_W) {
                std::array<double, GS_W> r;
                for (int j = 0; j < GS_W; ++j) r[j] = x[i + j];
                STB.push_back(r);
            }
            MST[10] = (double)STB.size();
            // loading: merge with what other Gain Stages already share (the newest change wins)
            shSync();
        }
    }
};

// ======================================================================== the plug-in
class GainStagePlugin : public Plugin {
public:
    explicit GainStagePlugin(const clap_host *h) : Plugin(h, kind()) { T_.P = this; }
    Tables T_;
    gui::View *createView() override;

protected:
    void dspActivate(double, uint32_t) override {}
    void dspReset() override {}
    void dspProcess(const double *const *in, const double *const *, double *const *out, int n) override
    {
        for (int c = 0; c < 8; ++c)
            if (out[c] != in[c]) memcpy(out[c], in[c], sizeof(double) * n);
        act.block(n);
        act.process(out, 8, n);
        act.audioKey(out, 8, n);
    }
    void stateSaveExtra(std::string &s) override { T_.save(s); }
    void stateLoadExtra(const std::string &k, const std::string &v) override { T_.load(k, v); }
};

static Plugin *create(const clap_host *h) { return new GainStagePlugin(h); }

// ======================================================================== the window
static const char *const L_HOLD[] = {"Off", "1 s", "2 s", "5 s", "10 s", "Infinite"};
static const char *const L_RANGE[] = {"24 dB", "36 dB", "48 dB", "60 dB", "72 dB", "96 dB"};
static const char *const L_ZOOM[] = {"100%", "115%", "130%", "150%", "175%", "200%"};
static const char *const L_MEAS[] = {"Peak", "RMS", "LUFS"};
static const char *const L_TIME[] = {"Manual", "1 s", "2 s", "3 s", "5 s", "10 s", "20 s", "30 s"};
static const char *const L_MODE[] = {"Target", "Match", "Reference"};
static const char *const L_TTYPE[] = {"Peak GR", "Average GR"};
static const char *const L_TTIME[] = {"1 s", "2 s", "3 s", "5 s", "10 s", "20 s", "30 s"};

class GsView : public gui::View {
public:
    explicit GsView(GainStagePlugin &p) : P(p), T(p.T_), av(p) {}
    void designSize(float &w, float &h) override { w = DW; h = DH; }
    void frame(Ui &ui) override;
    bool wantsFull() override { return st.settings || sbDrag; }
    uint64_t statusHash() override;
    void opened() override { T.shSync(); shSyncT = now_sec(); }

private:
    static constexpr float DW = 1100, DH = 760, M = 14, SW = 640, SH = 214;
    static constexpr float LIST_Y = 106;
    GainStagePlugin &P;
    Tables &T;
    ActView av;
    CommonState st;
    double shSyncT = -100;
    // lists: Main = this chain, Stages = every Gain Stage, Finales = every chain's finale
    std::vector<int> LST, LST2, LST3;
    int nhid1 = 0, nhid2 = 0, nhid3 = 0;
    double ref0 = 0, ref1 = 0, ref2 = 0;
    float scroll[3] = {0, 0, 0}, contentH = 0;
    bool sbDrag = false;
    float sbY0 = 0, sbS0 = 0;
    std::mt19937 rng{std::random_device{}()};

    int tab() const { return iclamp(iround(T.MST[4]), 0, 2); }
    int mySlot() const { return av.myslot; }
    int64_t myId() const { return av.myid; }
    // parameter source: this instance's own parameters, or a slot's (as it published them)
    int mps(int s) const { return s == mySlot() ? -1 : s; }
    static int key(int ps, int p) { return ps < 0 ? p : (ps + 2) * 256 + p; }
    double kget(int ps, int p) const { return ps < 0 ? P.get(p) : shm::slot(ps).p[p].load(); }
    void kset(int ps, int p, double v)
    {
        if (ps < 0) { P.setOnce(p, v); return; }
        v = P.T.clamp(p, v);
        shm::send(ps, shm::C_SET, p, v, 0);
        shm::slot(ps).p[p].store(v);
    }
    double mid(int s) const { return s == mySlot() ? (double)myId() : (double)shm::slot(s).owner.load(); }
    bool misact(int s) const { return s != mySlot() && shm::slot(s).kind.load() == shm::KIND_POINT; }
    double mkey(int s, int tb) const
    {
        double fk = s >= 0 && s < (int)av.FKEY.size() ? av.FKEY[s] : 0;
        return (tb == 2 && fk >= 100000) ? fk : mid(s);
    }
    bool mhidden(int s, int tb) const
    {
        if (tb == 0) return s == mySlot() ? false : truth(T.tblGet(mid(s)));
        return truth(T.shGet(mkey(s, tb), tb));
    }
    bool mincl(int s, int tb) const { return tb != 0 ? true : !truth(T.shGet(mid(s), 6)); }
    const std::vector<int> &tablist(int tb) const { return tb == 0 ? LST : (tb == 1 ? LST3 : LST2); }
    bool inlist(int tb, double id) const
    {
        for (int s : tablist(tb)) if (mid(s) == id) return true;
        return false;
    }
    double findref(int tb) const
    {
        if (tb == 0) return inlist(0, T.MST[1]) ? T.MST[1] : 0;
        for (int s : tablist(tb)) if (truth(T.shGet(mkey(s, tb), tb + 2))) return mid(s);
        return 0;
    }
    double mref(int tb) const { return tb == 0 ? ref0 : (tb == 1 ? ref1 : ref2); }
    void refreshRefs() { ref0 = findref(0); ref1 = findref(1); ref2 = findref(2); }
    bool live(int s) const { return s == mySlot() || av.live(s); }
    int fin(int s) const { return s >= 0 && s < (int)av.FIN.size() ? av.FIN[s] : 0; }

    // a compressor, EQ, multiband or wah card can show its Set row
    bool tgKind(int s) const
    {
        int k = (s >= 0 && s != mySlot()) ? (int)shm::slot(s).subkind.load() : 0;
        return (k >= 1 && k <= 3) || k == 11;
    }
    bool tgOn(int s) const { return tgKind(s) && truth(T.shGet((double)shm::slot(s).owner.load(), 7)); }
    void cardAuto(int s, int mode, double run)
    {
        // one card: the Set now, then Auto. Auto all: Auto queues, and the Set starts when the earlier cards are done
        if (eq(run, 0) && tgOn(s)) shm::send(s, shm::C_TGSTART, 0, 0, 0);
        shm::send(s, shm::C_AUTO, mode, run, 0);
        if (ne(run, 0) && tgOn(s)) shm::send(s, shm::C_TGSTART, 1, 0, 0);
    }
    void autoClick(int s)
    {
        if (s < 0) return;
        if (shm::slot(s).astate.load() != shm::AS_IDLE) shm::send(s, shm::C_STOP, 0, 0, 0);
        else cardAuto(s, av.autoMode(mps(s)), 0);
    }

    void buildLists();
    std::string cardTitle(int s, int tb, int idx) const;
    void setReference(int tb, int s);
    void clearReference(int tb);
    void refMenu(Ui &ui, int tb, float x, float y);
    void setHidden(int s, int tb, bool hide);
    void autoAll(int tb);
    void setAll(int tb, double v);
    void runStatus(int tb, int &busy, int &done, int &tot) const;
    void stopAll(int tb);
    void cardMenu(Ui &ui, int s, float x, float y);
    void tgrRow(Ui &ui, int s, float xr, float y, bool en, const R &timeCol, const R &btnCol);
    float cardHeight(int s, int tb) const;
    void card(Ui &ui, int s, int tb, int idx, float y);
    void topBar(Ui &ui);
    void toolbar(Ui &ui);
    void settings(Ui &ui, const R &gear);
    void enumDD(Ui &ui, const R &r, int ps, int p, const char *const *labels, int n, bool en = true, const char *prefix = nullptr);
};

gui::View *GainStagePlugin::createView() { return new GsView(*this); }

// ---------------------------------------------------------------- lists
static void sortByTrack(std::vector<int> &ls)
{
    auto trk = [](int s) { return (double)shm::slot(s).track.load(); };
    auto ord = [](int s) { return shm::slot(s).order.load(); };
    for (size_t i = 1; i < ls.size(); ++i) {
        int t = ls[i];
        double kt = trk(t), kv = ord(t);
        int j = (int)i - 1;
        while (j >= 0 && (trk(ls[j]) > kt || (trk(ls[j]) == kt && ord(ls[j]) > kv))) { ls[j + 1] = ls[j]; --j; }
        ls[j + 1] = t;
    }
}

void GsView::buildLists()
{
    LST.clear(); LST2.clear(); LST3.clear();
    nhid1 = nhid2 = nhid3 = 0;
    int me = mySlot();
    if (me < 0) return;
    shm::Slot &mb = shm::slot(me);
    int64_t mytrk = mb.track.load(), mypf = mb.pflags.load();
    int mx = (int)std::min<int64_t>(shm::NSLOT, shm::get()->h.maxslot.load());
    for (int s = 0; s < mx; ++s) {
        shm::Slot &b = shm::slot(s);
        if (b.owner.load() < 100000) continue;
        // one that stopped processing (bypassed) stays listed while REAPER still has it in a project
        bool ex = s == me || av.live(s) || (av.named(s) && (b.nflags.load() & shm::NF_INPROJECT));
        int64_t pf = b.pflags.load();
        if (!(ex && !(pf & 1) && (pf & 4) == (mypf & 4))) continue;
        if (s == me || b.track.load() == mytrk) { LST.push_back(s); if (mhidden(s, 0)) ++nhid1; }
        if (s == me || b.kind.load() == shm::KIND_MASTER) { LST3.push_back(s); if (mhidden(s, 1)) ++nhid3; }
        if (fin(s) > 0) { LST2.push_back(s); if (mhidden(s, 2)) ++nhid2; }
    }
    // Main: chain order. Stages and Finales: track, then chain order.
    for (size_t i = 1; i < LST.size(); ++i) {
        int t = LST[i];
        double kv = shm::slot(t).order.load();
        int j = (int)i - 1;
        while (j >= 0 && shm::slot(LST[j]).order.load() > kv) { LST[j + 1] = LST[j]; --j; }
        LST[j + 1] = t;
    }
    sortByTrack(LST3);
    sortByTrack(LST2);
}

// card title: Main = the plug-in's own name or Act - OUT: <previous plug-in>;
// Stages and Finales = <track name>, plus 1, 2, 3 when a track has several
std::string GsView::cardTitle(int s, int tb, int idx) const
{
    if (tb == 0) return av.pointTitle(s);
    std::string t = av.trackName(s);
    const auto &ls = tablist(tb);
    int64_t trk = shm::slot(s).track.load();
    int cnt = 0, rank = 0;
    for (int i = 0; i < (int)ls.size(); ++i)
        if (shm::slot(ls[i]).track.load() == trk) { ++cnt; if (i <= idx) ++rank; }
    if (cnt > 1) t += " " + std::to_string(rank);
    return t;
}

// ---------------------------------------------------------------- references, hiding, Auto all, Set all
void GsView::setReference(int tb, int s)
{
    const auto ls = tablist(tb);
    double rid = mid(s);
    if (tb == 0) {
        T.MST[1] = rid;
        T.changed();
    } else {
        // one reference per tab
        for (int t : ls)
            if (mkey(t, tb) != mkey(s, tb) && truth(T.shGet(mkey(t, tb), tb + 2))) T.shSet(mkey(t, tb), tb + 2, 0);
        T.shSet(mkey(s, tb), tb + 2, 1);
        if (tb == 2) T.MST[11] = rid;
    }
    // the reference leads: it follows nothing itself
    kset(mps(s), P_REF, 0);
    for (int t : ls)
        if (t != s && !mhidden(t, tb)) kset(mps(t), P_REF, rid);
    refreshRefs();
}

void GsView::clearReference(int tb)
{
    const auto ls = tablist(tb);
    double rid = mref(tb), rk = 0;
    for (int t : ls) {
        if (kget(mps(t), P_REF) == rid) kset(mps(t), P_REF, 0);
        if (mid(t) == rid) rk = mkey(t, tb);
    }
    if (tb == 0) { T.MST[1] = 0; T.changed(); }
    else if (rk >= 100000) T.shSet(rk, tb + 2, 0);
    if (tb == 2) T.MST[11] = 0;
    refreshRefs();
}

// the toolbar's reference menu: None, or any visible card on this tab
void GsView::refMenu(Ui &ui, int tb, float x, float y)
{
    const auto &ls = tablist(tb);
    double rid = mref(tb);
    std::vector<MenuItem> m;
    m.push_back(MenuItem::item("None", 1, rid < 100000));
    std::vector<int> args;
    for (int i = 0; i < (int)ls.size() && args.size() < 250; ++i) {
        int s = ls[i];
        if (mhidden(s, tb)) continue;
        std::string t = cardTitle(s, tb, i);
        if (tb == 2) t += "  \xE2\x80\x93  " + av.pointTitle(s);
        m.push_back(MenuItem::item(t, 2 + (int)args.size(), mid(s) == rid));
        args.push_back(s);
    }
    ui.menu(m, x, y, [this, tb, rid, args](int r) {
        if (r == 1 && rid >= 100000) clearReference(tb);
        if (r >= 2 && r - 2 < (int)args.size()) setReference(tb, args[r - 2]);
    }, 300);
}

void GsView::setHidden(int s, int tb, bool hide)
{
    double rid = mref(tb);
    if (tb == 0) T.tblSet(mid(s), hide ? 1 : 0);
    else T.shSet(mkey(s, tb), tb, hide ? 1 : 0);
    if (hide) {
        // hidden cards leave the reference alone; hiding the reference itself clears it
        if (mid(s) == rid) clearReference(tb);
        else if (kget(mps(s), P_REF) == rid) kset(mps(s), P_REF, 0);
    } else if (rid >= 100000 && mid(s) != rid) kset(mps(s), P_REF, rid);
}

void GsView::autoAll(int tb)
{
    double run = 1 + std::floor(std::uniform_real_distribution<double>(0, 999999)(rng));
    T.MST[7 + tb] = run;
    T.changed();
    for (int t : tablist(tb))
        if (!mhidden(t, tb) && mincl(t, tb) && live(t)) cardAuto(t, av.autoMode(mps(t)), run);
}

// Set all: every visible card's gain (each fader) to v
void GsView::setAll(int tb, double v)
{
    for (int t : tablist(tb)) {
        if (mhidden(t, tb) || !live(t)) continue;
        int ps = mps(t), ga[8], gn[8];
        int ng = av.groups(ps, ga, gn);
        for (int g = 0; g < ng; ++g) kset(ps, P_GAIN + ga[g], v);
    }
}

void GsView::runStatus(int tb, int &busy, int &done, int &tot) const
{
    busy = done = tot = 0;
    double run = T.MST[7 + tb];
    if (run <= 0) return;
    for (int s : tablist(tb)) {
        shm::Slot &b = shm::slot(s);
        if ((double)b.runid.load() == run) {
            ++tot;
            if (b.astate.load() != shm::AS_IDLE) ++busy; else ++done;
        }
    }
}

void GsView::stopAll(int tb)
{
    double run = T.MST[7 + tb];
    for (int s : tablist(tb)) {
        shm::Slot &b = shm::slot(s);
        if ((double)b.runid.load() == run && b.astate.load() != shm::AS_IDLE) shm::send(s, shm::C_STOP, 0, 0, 0);
    }
}

// the ... menu on a card: its Set row, finale, channels, links, drive link, peak holds
void GsView::cardMenu(Ui &ui, int s, float x, float y)
{
    int ps = mps(s);
    bool isact = misact(s);
    int nc = av.nch(ps);
    bool fixc = shm::slot(s).subkind.load() > 0;   // an Act built into a plug-in has the plug-in's channel layout
    std::vector<MenuItem> m;
    if (tgKind(s)) {
        int64_t sk = shm::slot(s).subkind.load();
        m.push_back(MenuItem::item(sk == 1 ? "Set Gain Reduction" : (sk == 2 ? "Set Dynamic Thresholds" : "Set Thresholds"), 80, tgOn(s)));
        m.push_back(MenuItem::sep());
    }
    if (isact) {
        int fm = iround(kget(ps, P_FINAL));
        m.push_back(MenuItem::item("Finale: auto (the last Act in the chain)", 1, fm == FIN_AUTO));
        m.push_back(MenuItem::item("Finale: always", 2, fm == FIN_ALWAYS));
        m.push_back(MenuItem::item("Finale: never", 3, fm == FIN_NEVER));
        m.push_back(MenuItem::sep());
    }
    if (!fixc) {
        std::vector<MenuItem> chm, lkm, usm;
        char b[64];
        int cur = iround(kget(ps, P_NCH));
        for (int i = 0; i < 4; ++i) { snprintf(b, sizeof b, "%d channels", i * 2 + 2); chm.push_back(MenuItem::item(b, 40 + i, cur == i)); }
        for (int i = 0; i < nc / 2; ++i) {
            snprintf(b, sizeof b, "Link %d-%d (one fader)", i * 2 + 1, i * 2 + 2);
            lkm.push_back(MenuItem::item(b, 50 + i, kget(ps, P_LINK + i) >= 0.5));
        }
        for (int i = 0; i < nc; ++i) { snprintf(b, sizeof b, "Use channel %d", i + 1); usm.push_back(MenuItem::item(b, 60 + i, kget(ps, P_USED + i) >= 0.5)); }
        snprintf(b, sizeof b, "Channels: %d", nc);
        m.push_back(MenuItem::submenu(b, chm));
        m.push_back(MenuItem::submenu("Links", lkm));
        m.push_back(MenuItem::submenu("Channels used", usm));
        m.push_back(MenuItem::sep());
    }
    std::vector<int64_t> ids;
    if (isact) {
        double cv = kget(ps, P_DRIVE);
        m.push_back(MenuItem::item("Drive link: none", 10, cv < DRIVE_PARAM));
        for (int d : av.chainPoints(s)) {
            int64_t id = shm::slot(d).owner.load();
            ids.push_back(id);
            m.push_back(MenuItem::item("Drive link: " + av.pointTitle(d), 100 + (int)ids.size() - 1, (double)id == cv));
        }
        m.push_back(MenuItem::item("Drive link: plug-in parameter (click, then move it)", 11, iround(cv) == DRIVE_PARAM, P.inReaper()));
        if (cv >= DRIVE_PARAM) m.push_back(MenuItem::item("Drive link engaged", 12, kget(ps, P_DRIVEON) >= 0.5));
        m.push_back(MenuItem::sep());
    }
    m.push_back(MenuItem::item("Clear peak holds", 13));
    ui.menu(m, x, y, [this, s, ps, ids](int r) {
        if (r >= 1 && r <= 3) kset(ps, P_FINAL, r - 1);
        else if (r >= 40 && r < 44) kset(ps, P_NCH, r - 40);
        else if (r >= 50 && r < 54) kset(ps, P_LINK + r - 50, kget(ps, P_LINK + r - 50) < 0.5 ? 1 : 0);
        else if (r >= 60 && r < 68) kset(ps, P_USED + r - 60, kget(ps, P_USED + r - 60) < 0.5 ? 1 : 0);
        else if (r == 10) av.setDrive(ps, 0);
        else if (r == 11) av.setDrive(ps, DRIVE_PARAM);
        else if (r == 12) kset(ps, P_DRIVEON, kget(ps, P_DRIVEON) < 0.5 ? 1 : 0);
        else if (r == 13) shm::send(s, shm::C_RESETHOLD, 0, 0, 0);
        else if (r == 80) T.shSet((double)shm::slot(s).owner.load(), 7, tgOn(s) ? 0 : 1);
        else if (r >= 100 && r - 100 < (int)ids.size()) av.setDrive(ps, ids[r - 100]);
    }, 300);
}

void GsView::enumDD(Ui &ui, const R &r, int ps, int p, const char *const *labels, int n, bool en, const char *prefix)
{
    int cv = iclamp(iround(kget(ps, p)), 0, n - 1);
    if (ui.dropdown(r, labels[cv], en, prefix)) {
        std::vector<MenuItem> m;
        for (int i = 0; i < n; ++i) m.push_back(MenuItem::item(labels[i], i + 1, i == cv));
        ui.menu(m, r.x, r.b() + 4, [this, ps, p](int r2) { kset(ps, p, r2 - 1); }, r.w);
    }
}

// the Set row on top of a card, lined up with the Time and Auto columns of the row below:
//   compressor: Set Gain Reduction - its target, Peak / Average, listening time, Set threshold
//   EQ: Set Dynamic Thresholds - listening time, Set thresholds
//   multiband: Set Thresholds - each band's target (the bands in use), Peak / Average, listening time, Set thresholds
//   wah: Set Thresholds - listening time, Set thresholds
void GsView::tgrRow(Ui &ui, int s, float xr, float y, bool en, const R &timeCol, const R &btnCol)
{
    int ps = mps(s);
    shm::Slot &b = shm::slot(s);
    int st = (int)b.tgs.load();
    int k = (int)b.subkind.load();
    // nothing to set: an EQ without dynamic bands, a multiband with every band off, a wah with no envelope or transient band
    bool avl = k == 1 || b.tgav.load() >= 0.5 || st > 0;
    const float h = 24;
    R typ(timeCol.x - 8 - 132, y, 132, h);
    float lx;
    if (k == 1) {
        R v(typ.x - 6 - 100, y, 100, h);
        char t[32];
        snprintf(t, sizeof t, "%.1f dB", kget(ps, P_TGR));
        ui.valueBox(v, key(ps, P_TGR), en, KD_OTHER, 0.1, 0.01, 2, t);
        enumDD(ui, typ, ps, P_TTYPE, L_TTYPE, 2, en);
        lx = v.x;
        ui.text(R(lx - 210, y, 202, h), "Set Gain Reduction", th::dim(), F_MEDIUM, 12, 2);
    } else if (k == 3) {
        int n = 0;
        for (int i = 0; i < 4; ++i) if (b.p[60 + i].load() >= 0.5) ++n;
        float x = typ.x - 6 - n * 76;
        lx = x;
        for (int i = 0; i < 4; ++i) {
            if (b.p[60 + i].load() < 0.5) continue;
            char t[32];
            snprintf(t, sizeof t, "B%d  %.1f", i + 1, kget(ps, P_MBT + i));
            ui.valueBox(R(x, y, 70, h), key(ps, P_MBT + i), en, KD_OTHER, 0.1, 0.01, 2, t);
            x += 76;
        }
        enumDD(ui, typ, ps, P_TTYPE, L_TTYPE, 2, en);
        ui.text(R(lx - 170, y, 162, h), "Set Thresholds", th::dim(), F_MEDIUM, 12, 2);
    } else {
        ui.text(R(timeCol.x - 8 - 240, y, 232, h), k == 2 ? "Set Dynamic Thresholds" : "Set Thresholds", th::dim(), F_MEDIUM, 12, 2);
    }
    if (st > 0) {
        char t[32];
        if (st == 1) snprintf(t, sizeof t, "waiting");
        else if (st == 2) snprintf(t, sizeof t, "%.1f s", b.tgel.load());
        else snprintf(t, sizeof t, "working");
        ui.button(R(timeCol.x, y, timeCol.w, h), t, true);
    } else {
        enumDD(ui, R(timeCol.x, y, timeCol.w, h), ps, P_TTIME, L_TTIME, 7, en);
    }
    const char *bl = st > 0 ? "Cancel"
                            : (!avl ? (k == 2 ? "no dynamic bands" : (k == 3 ? "every band off" : "no envelope band"))
                                    : (k == 1 ? "Set threshold" : "Set thresholds"));
    if (ui.button(R(btnCol.x, y, btnCol.w, h), bl, st > 0, en && avl, st > 0 ? BS_NORMAL : BS_ORANGE_OUTLINE))
        shm::send(s, st > 0 ? shm::C_TGSTOP : shm::C_TGSTART, 0, 0, 0);
    (void)xr;
}

float GsView::cardHeight(int s, int tb) const
{
    bool hid = mhidden(s, tb);
    if (hid) return 38;
    int ga[8], gn[8];
    int ng = av.groups(mps(s), ga, gn);
    bool tg = tgOn(s);
    return 10 + (tg ? 32 : 0) + 24 + 10 + std::max(ng, 0) * 28 + 6;
}

// one instance card
void GsView::card(Ui &ui, int s, int tb, int idx, float y)
{
    int ps = mps(s);
    bool own = s == mySlot(), isact = misact(s);
    shm::Slot &b = shm::slot(s);
    double id = mid(s);
    bool lv = live(s);
    bool hid = mhidden(s, tb);
    double rid = mref(tb);
    bool isref = rid >= 100000 && id == rid;
    bool en = lv && !hid;
    const float x0 = M, cw = DW - 2 * M - 12, xr = x0 + cw - 12;
    int ga[8], gn[8];
    int ng = hid ? 0 : av.groups(ps, ga, gn);
    bool rb = b.astate.load() != shm::AS_IDLE;
    bool tg = !hid && tgOn(s);
    float h = cardHeight(s, tb);
    R r(x0, y, cw, h);
    const float ch = 24;
    // the columns of line 1, from the right
    R hideB(xr - 56, 0, 56, ch), autoB(hideB.x - 6 - 66, 0, 66, ch), timB(autoB.x - 6 - 78, 0, 78, ch), measB(timB.x - 6 - 82, 0, 82, ch);
    int sm = hid ? 0 : av.autoMode(ps);
    bool srcLine = sm != 0 && ng == 1;
    R srcB(measB.x - 6 - 92, 0, 92, ch);
    float valR = (srcLine ? srcB.x : measB.x) - 6;
    R valB(valR - 210, 0, 210, ch), modeB(valB.x - 6 - 108, 0, 108, ch), dotsB(modeB.x - 6 - 30, 0, 30, ch);
    float ry = tg ? y + 10 + 32 : y + 10;
    auto at = [&](R q, float yy) { q.y = yy; return q; };

    if (ui.drawS()) {
        ui.shadow(r, 8, 10, Col(0, 0, 0, 0.4f));
        ui.fill(r, hid ? Paint(Col::hex(0x0f0f11)) : Paint::vertical(r.y, Col::hex(0x17161a), r.b(), Col::hex(0x111114)), 8);
        ui.stroke(r, Paint(isref ? th::purple() : th::border()), 8);
        if (isref) ui.stroke(R(r.x + 1, r.y + 1, r.w - 2, r.h - 2), Paint(th::purple().alpha(0.5f)), 7);
        // the Auto all tick (Main): it belongs to the card and is shared by every Gain Stage
        float tx = x0 + 14;
        if (tb == 0) {
            if (ui.checkbox(R(tx, (hid ? y + 12 : ry + 5), 14, 14), mincl(s, tb), !hid)) T.shSet(id, 6, mincl(s, tb) ? 1 : 0);
            tx += 24;
        }
        // title and badges
        std::string t = cardTitle(s, tb, idx);
        float ty = hid ? y + 7 : ry;
        Col tc = hid ? th::faint() : ((own && tb == 0) ? th::orange() : th::text());
        float tmax = (hid ? hideB.x : dotsB.x) - 10 - tx;
        float tw = std::min(ui.textW(F_SEMIBOLD, 13.5f, t.c_str()), std::min(tmax, 300.f));
        ui.textFit(R(tx, ty, tw + 2, ch), t.c_str(), tc, F_SEMIBOLD, 13.5f);
        float bx = tx + tw + 10, bmax = dotsB.x - 8;
        if (!hid) {
            if (tb != 0) {
                std::string ot = av.pointTitle(s);
                if (tb == 1 && own) ot += " (this one)";
                float ow = ui.textW(F_REGULAR, 11.5f, ot.c_str());
                if (bx + ow < bmax) { ui.text(R(bx, ty, ow + 4, ch), ot.c_str(), th::dim(), F_REGULAR, 11.5f); bx += ow + 10; }
            }
            auto badge = [&](const char *bt, const Col &fg) {
                float bw = ui.textW(F_SEMIBOLD, 9.5f, bt) + 14;
                if (bx + bw > bmax) return;
                ui.badge(bx, ty + 5, bt, fg, fg.alpha(0.15f));
                bx += bw + 6;
            };
            if (tb == 0 && fin(s) > 0) badge("FINALE", th::orange());
            if (tb != 1 && fin(s) > 0 && av.finTail(s)) badge("PLUG-INS AFTER IT", th::amber());
            if (isref) badge("REFERENCE", th::purple());
            if (isact && kget(ps, P_DRIVE) >= DRIVE_PARAM) badge(kget(ps, P_DRIVEON) >= 0.5 ? "DRIVE LINK" : "DRIVE LINK OFF", th::purpleHi());
            if (!lv) badge("NOT PROCESSING", th::red());
        }
        if (hid) {
            if (ui.button(R(hideB.x, y + 7, hideB.w, ch), "Show")) setHidden(s, tb, false);
        } else {
            // a Set row (switched on in the ... menu of a compressor, EQ, multiband or wah card) sits on top
            if (tg) tgrRow(ui, s, xr, y + 10, en, at(timB, 0), R(autoB.x, 0, xr - autoB.x, ch));
            if (ui.button(at(hideB, ry), "Hide", false, !(own && tb == 0))) setHidden(s, tb, true);
            if (ui.button(at(autoB, ry), av.autoLabel(ps), rb, en)) autoClick(s);
            // the Time box: its dropdown while idle; while Auto runs, queued / waiting / the seconds listened
            bool busy, nosig;
            std::string tt = av.autoTimeText(ps, &busy, &nosig);
            if (busy) ui.button(at(timB, ry), tt.c_str(), true);
            else if (nosig) ui.button(at(timB, ry), "no signal", false, false);
            else enumDD(ui, at(timB, ry), ps, P_TIME, L_TIME, 8, en);
            enumDD(ui, at(measB, ry), ps, P_MEAS, L_MEAS, 3, en);
            if (srcLine) {
                std::string st2 = av.srcText(ps, ga[0], true);
                if (ui.dropdown(at(srcB, ry), st2.c_str(), en)) {
                    // reuse the Act card's source menu logic through the parameter of group ga[0]
                    int a = ga[0];
                    int sel = iround(kget(ps, P_MSRC + a));
                    int srcSlot = sm == 1 ? av.prevSlot(av.ds(ps)) : av.findId((int64_t)kget(ps, P_REF));
                    int pnc = srcSlot >= 0 ? av.nch(srcSlot) : 2;
                    std::vector<MenuItem> mm;
                    mm.push_back(MenuItem::item("Same Ch", 100, sel == 0));
                    mm.push_back(MenuItem::item("All Ch (combined)", 109, sel == 9));
                    const char *who = sm == 1 ? "Previous Act" : "Reference";
                    char bb[64];
                    for (int i = 0; i < pnc; ++i) { snprintf(bb, sizeof bb, "%s ch %d", who, i + 1); mm.push_back(MenuItem::item(bb, 101 + i, sel == i + 1)); }
                    for (int i = 0; i < pnc / 2; ++i) {
                        snprintf(bb, sizeof bb, "%s ch %d-%d (average)", who, i * 2 + 1, i * 2 + 2);
                        mm.push_back(MenuItem::item(bb, 111 + i, sel == 11 + i));
                    }
                    R sb = at(srcB, ry);
                    ui.menu(mm, sb.x, sb.b() + 4, [this, ps, a](int r2) { kset(ps, P_MSRC + a, r2 - 100); });
                }
            }
            // mode | value
            R mb = at(modeB, ry);
            if (ui.dropdown(mb, L_MODE[iclamp(sm, 0, 2)], en)) {
                ui.menu({MenuItem::item("Target", 1, sm == 0), MenuItem::item("Reference", 3, sm == 2), MenuItem::item("Match", 2, sm == 1)}, mb.x,
                        mb.b() + 4, [this, ps](int r2) { kset(ps, P_AMODE, r2 == 1 ? 0 : (r2 == 3 ? 2 : 1)); });
            }
            R vb = at(valB, ry);
            if (sm == 0) {
                char buf[32];
                snprintf(buf, sizeof buf, kget(ps, P_MEAS) >= 1.5 ? "%.1f LUFS" : "%.1f dBFS", kget(ps, P_TGT));
                ui.valueBox(vb, key(ps, P_TGT), en, KD_TARGET, 0.1, 0.01, 2, buf);
            } else {
                bool ok;
                std::string vt = av.valueText(ps, &ok);
                ui.fill(vb, Paint(th::well()), 4);
                ui.stroke(vb, Paint(th::border()), 4);
                ui.textFit(R(vb.x + 8, vb.y, vb.w - 16, vb.h), vt.c_str(), ok ? th::purple() : th::faint(), F_MEDIUM, 11.5f);
            }
            R db = at(dotsB, ry);
            if (ui.button(db, "", false, en, BS_NORMAL, IC_DOTS)) cardMenu(ui, s, db.x, db.b() + 4);
        }
    }
    if (hid) return;
    // gain groups: meters (moving), source (several groups), readouts (moving), gain
    float gy = ry + ch + 10;
    bool srcRows = sm != 0 && ng > 1;
    R gbox(xr - 92, 0, 92, 22), gbar(gbox.x - 6 - 190, 0, 190, 22);
    R rr(gbar.x - 14 - 84, 0, 84, 22), rp(rr.x - 4 - 78, 0, 78, 22);
    R srcR(rp.x - 10 - 92, 0, 92, 22);
    float mx0 = x0 + 14 + 38, mx1 = (srcRows ? srcR.x : rp.x) - 12;
    double tgt = av.aimTick(ps), range = av.meterRange();
    for (int g = 0; g < ng; ++g, gy += 28) {
        int a = ga[g], n = gn[g];
        if (ui.drawS()) {
            char lb[32];
            if (n == 2) snprintf(lb, sizeof lb, "%d-%d", a + 1, a + 2); else snprintf(lb, sizeof lb, "%d", a + 1);
            ui.text(R(x0 + 14, gy, 36, 22), lb, th::dim(), F_MEDIUM, 11);
            if (srcRows) {
                std::string st2 = av.srcText(ps, a, true);
                R sb = at(srcR, gy);
                if (ui.dropdown(sb, st2.c_str(), en)) {
                    int sel = iround(kget(ps, P_MSRC + a));
                    std::vector<MenuItem> mm;
                    mm.push_back(MenuItem::item("Same Ch", 100, sel == 0));
                    mm.push_back(MenuItem::item("All Ch (combined)", 109, sel == 9));
                    int srcSlot = sm == 1 ? av.prevSlot(av.ds(ps)) : av.findId((int64_t)kget(ps, P_REF));
                    int pnc = srcSlot >= 0 ? av.nch(srcSlot) : 2;
                    const char *who = sm == 1 ? "Previous Act" : "Reference";
                    char bb[64];
                    for (int i = 0; i < pnc; ++i) { snprintf(bb, sizeof bb, "%s ch %d", who, i + 1); mm.push_back(MenuItem::item(bb, 101 + i, sel == i + 1)); }
                    for (int i = 0; i < pnc / 2; ++i) {
                        snprintf(bb, sizeof bb, "%s ch %d-%d (average)", who, i * 2 + 1, i * 2 + 2);
                        mm.push_back(MenuItem::item(bb, 111 + i, sel == 11 + i));
                    }
                    ui.menu(mm, sb.x, sb.b() + 4, [this, ps, a](int r2) { kset(ps, P_MSRC + a, r2 - 100); });
                }
            }
            // readouts: a click clears the holds
            R rd(rp.x, gy, rr.r() - rp.x, 22);
            if (lv && ui.hit(rd) && ui.lclick && !ui.used) { ui.used = true; shm::send(s, shm::C_RESETHOLD, 0, 0, 0); }
            ui.bar(at(gbar, gy), key(ps, P_GAIN + a), SC_GAIN, en, KD_GAIN, true);
            ui.valueBox(at(gbox, gy), key(ps, P_GAIN + a), en, KD_GAIN, 0.1, 0.01);
        }
        if (ui.drawD()) {
            if (n == 2) {
                hMeter(ui, R(mx0, gy + 1, mx1 - mx0, 9), s, a, tgt, lv, range);
                hMeter(ui, R(mx0, gy + 12, mx1 - mx0, 9), s, a + 1, tgt, lv, range);
            } else {
                hMeter(ui, R(mx0, gy + 1, mx1 - mx0, 20), s, a, tgt, lv, range);
            }
            double pk = -200, rm = -200;
            bool clip = false;
            if (lv) for (int k = a; k < a + n; ++k) {
                pk = std::max(pk, b.pkh[k].load());
                rm = std::max(rm, b.rmh[k].load());
                if (b.clip[k].load() > 0) clip = true;
            }
            R p1 = at(rp, gy), p2 = at(rr, gy);
            ui.fill(p1, Paint(clip ? Col::hex(0x3a0d12) : th::well()), 4);
            ui.stroke(p1, Paint(clip ? th::red() : th::border()), 4);
            ui.fill(p2, Paint(th::well()), 4);
            ui.stroke(p2, Paint(th::border()), 4);
            char b1[32], b2[32];
            if (pk <= -150) snprintf(b1, sizeof b1, "-inf"); else snprintf(b1, sizeof b1, "%.1f", pk);
            if (rm <= -150) snprintf(b2, sizeof b2, "-inf"); else snprintf(b2, sizeof b2, "%.1f", rm);
            ui.text(R(p1.x + 7, p1.y, 30, p1.h), "PK", th::faint(), F_SEMIBOLD, 10);
            ui.text(R(p1.x, p1.y, p1.w - 7, p1.h), b1, th::text(), F_MEDIUM, 11.5f, 2, true);
            ui.text(R(p2.x + 7, p2.y, 30, p2.h), "RMS", th::faint(), F_SEMIBOLD, 10);
            ui.text(R(p2.x, p2.y, p2.w - 7, p2.h), b2, th::purpleHi(), F_MEDIUM, 11.5f, 2, true);
        }
    }
    if (ng == 0 && !hid && ui.drawS())
        ui.text(R(x0 + 14, gy, cw - 28, 22), "Every channel is switched off (see \xE2\x80\xA6), so everything passes through untouched.", th::dim(),
                F_REGULAR, 12);
}

// ---------------------------------------------------------------- top bar, toolbar, settings
void GsView::topBar(Ui &ui)
{
    if (!ui.drawS()) return;
    const float H = 56;
    ui.fill(R(0, 0, DW, H), Paint::vertical(0, Col::hex(0x141317), H, Col::hex(0x0d0d10)));
    ui.fill(R(0, H - 1, DW, 1), Paint(th::border()));
    ui.fill(R(0, 0, DW, 2), Paint::linear(0, 0, th::orange().alpha(0.0f), DW * 0.5f, 0, th::orange().alpha(0.55f)));
    logo(ui, 14, 12, 30);
    float tx = 54;
    ui.textAt(tx, 27, "TWOSTORMS", th::orange(), F_BOLD, 15.5f);
    float w1 = ui.textW(F_BOLD, 15.5f, "TWOSTORMS");
    ui.textAt(tx + w1 + 6, 27, "GAIN STAGE", th::text(), F_BOLD, 15.5f);
    // a TwoStorms Tuner listening to this Gain Stage has its Mute on: that, instead of the subtitle
    if (tunerMuted(av)) ui.text(R(tx, 34, 260, 14), "MUTED BY TUNER", th::red(), F_SEMIBOLD, 11);
    else
        ui.textFit(R(tx, 34, 260, 14), st.subtitle.c_str(), th::faint(), F_REGULAR, 11);
    // tabs
    int tb = tab();
    char t1[32], t2[32];
    snprintf(t1, sizeof t1, "Stages (%d)", (int)LST3.size());
    snprintf(t2, sizeof t2, "Finales (%d)", (int)LST2.size());
    const char *names[3] = {"Main", t1, t2};
    float w[3] = {96, 116, 120}, x = 340;
    for (int i = 0; i < 3; ++i) {
        if (ui.button(R(x, 14, w[i], 28), names[i], tb == i)) { T.MST[4] = i; T.changed(); }
        x += w[i] + 4;
    }
    // right: ON/OFF (made for ReaLearn scenes), settings
    R gear(DW - 14 - 32, 14, 32, 28);
    bool on = P.get(P_GS_ON) >= 0.5;
    R onb(gear.x - 10 - 150, 10, 150, 36);
    if (ui.button(onb, on ? "ON" : "OFF", on)) P.setOnce(P_GS_ON, on ? 0 : 1);
    if (ui.button(gear, "", st.settings, true, BS_NORMAL, IC_GEAR)) st.settings = !st.settings;
}

void GsView::toolbar(Ui &ui)
{
    if (!ui.drawS()) return;
    int tb = tab();
    float y = 68, h = 28;
    int busy, done, tot;
    runStatus(tb, busy, done, tot);
    char as[96] = "";
    if (busy > 0) {
        if (ui.button(R(M, y, 104, h), "Stop all", true)) stopAll(tb);
        snprintf(as, sizeof as, "Auto all: %d of %d done", done, tot);
    } else {
        if (ui.button(R(M, y, 104, h), "Auto all")) autoAll(tb);
        if (tot > 0) snprintf(as, sizeof as, "Auto all: done (%d)", done);
        else if (tb == 0) snprintf(as, sizeof as, "Ticked, visible cards, in chain order, each in its own mode.");
    }
    float tx = M + 104 + 12;
    if (tb != 0) {
        // Set all: every visible card's gain to the value in the box
        int sk = tb == 1 ? P_GS_SETS : P_GS_SETF;
        if (ui.button(R(tx, y, 74, h), "Set all")) setAll(tb, P.get(sk));
        ui.valueBox(R(tx + 80, y, 96, h), sk, true, KD_GAIN, 0.1, 0.01);
        tx += 80 + 96 + 12;
    }
    // the tab's reference: cards in Reference mode aim for its output level
    double rid = mref(tb);
    std::string rs;
    if (rid >= 100000) {
        int rsl = av.findId((int64_t)rid);
        int rkf = -1;
        const auto &ls = tablist(tb);
        for (int i = 0; i < (int)ls.size(); ++i) if (ls[i] == rsl) rkf = i;
        rs = "Reference: " + (rkf >= 0 ? cardTitle(rsl, tb, rkf) : av.titleOfId((int64_t)rid));
    } else {
        rs = "Reference: none";
    }
    int nh = tb == 0 ? nhid1 : (tb == 1 ? nhid3 : nhid2);
    char sh[48];
    snprintf(sh, sizeof sh, truth(T.MST[6]) ? "Hidden (%d): shown" : "Hidden (%d): off", nh);
    R hb(DW - M - 156, y, 156, h);
    R rb(hb.x - 8 - 300, y, 300, h);
    ui.textFit(R(tx, y, rb.x - 12 - tx, h), as, busy > 0 ? th::orange() : th::dim(), F_REGULAR, 12);
    if (ui.dropdown(rb, rs.c_str())) refMenu(ui, tb, rb.x, rb.b() + 4);
    if (rid >= 100000) ui.stroke(rb, Paint(th::purple().alpha(0.8f)), 5);
    if (ui.button(hb, sh, truth(T.MST[6]) && nh > 0)) { T.MST[6] = truth(T.MST[6]) ? 0 : 1; T.changed(); }
}

void GsView::settings(Ui &ui, const R &gear)
{
    if (!ui.drawS()) return;
    R in;
    if (!settingsBegin(ui, st, gear, SW, SH, in)) return;
    const float h = 24;
    float y = in.y;
    ui.text(R(in.x, y, 200, 14), "SETTINGS", th::faint(), F_SEMIBOLD, 10.5f);
    y += 22;
    float x = in.x;
    ui.text(R(x, y, 90, h), "ON/OFF glide", th::dim(), F_REGULAR, 12);
    ui.valueBox(R(x + 90, y, 80, h), P_GS_GLIDE, true, KD_TIME, 2, 0.5);
    x += 90 + 80 + 18;
    ui.text(R(x, y, 64, h), "Peak hold", th::dim(), F_REGULAR, 12);
    enumDD(ui, R(x + 64, y, 86, h), -1, P_HOLD, L_HOLD, 6);
    x += 64 + 86 + 18;
    ui.text(R(x, y, 82, h), "Meter range", th::dim(), F_REGULAR, 12);
    enumDD(ui, R(x + 82, y, 80, h), -1, P_RANGE, L_RANGE, 6);
    y += h + 10;
    ui.text(R(in.x, y, 130, h), "Zoom (every window)", th::dim(), F_REGULAR, 12);
    enumDD(ui, R(in.x + 132, y, 80, h), -1, P_ZOOM, L_ZOOM, 6);
    y += h + 14;
    ui.fill(R(in.x, y, in.w, 1), Paint(th::border()));
    y += 8;
    static const char *help[] = {
        "Modes: Target = the output hits Target. Match = equals the previous plug-in. Reference = equals the tab's reference.",
        "Tick (Main) = in Auto all. Hide a card to leave it out; Hidden: shown brings hidden cards back, dimmed.",
        "Drag a bar to adjust (Shift/Ctrl = fine), double-click a bar to reset it, click a value to type (\"-inf\" works).",
        "A compressor's, EQ's, multiband's or wah's card shows its Set row when you tick it in the card's \xE2\x80\xA6 menu.",
    };
    for (const char *t : help) { ui.textFit(R(in.x, y, in.w, 14), t, th::faint(), F_REGULAR, 10.5f); y += 15; }
    char idb[64];
    snprintf(idb, sizeof idb, "Plugin ID %lld", (long long)myId());
    ui.text(R(in.x, y + 2, in.w, 14), idb, th::ghost().lighter(0.3f), F_REGULAR, 10.5f, 2);
    settingsEnd(ui);
}

uint64_t GsView::statusHash()
{
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    auto mixd = [&](double d) { uint64_t b; memcpy(&b, &d, 8); mix(b); };
    av.refresh();
    buildLists();
    mix((uint64_t)(av.myslot + 1));
    for (double v : T.MST) mixd(v);
    mix((uint64_t)T.TBL.size());
    for (auto &r : T.TBL) { mixd(r[0]); mixd(r[1]); }
    mix((uint64_t)shm::get()->gs.ver.load());
    mix((uint64_t)shm::get()->h.slotver.load());
    mixd(scroll[0]); mixd(scroll[1]); mixd(scroll[2]);
    mix((uint64_t)st.settings);
    mix((uint64_t)P.driveStatus());
    for (int tb = 0; tb < 3; ++tb) {
        const auto &ls = tablist(tb);
        mix(ls.size());
        for (int s : ls) {
            shm::Slot &b = shm::slot(s);
            mix((uint64_t)s);
            mix(av.live(s));
            mix((uint64_t)fin(s));
            mix((uint64_t)b.astate.load());
            mix((uint64_t)(b.atime.load() * 10));
            mix((uint64_t)b.resseq.load());
            mix(b.result.load() == shm::R_NOSIG && av.now - b.donet.load() < 4);
            mix((uint64_t)b.nseq.load());
            mix((uint64_t)b.tgs.load());
            mix((uint64_t)(b.tgel.load() * 10));
            mixd(b.tgav.load());
            mix((uint64_t)b.runid.load());
            if (s != av.myslot)
                for (int p = 1; p <= NPUB; ++p) mixd(b.p[p].load());
            for (int p = 56; p <= 71; ++p) mixd(b.p[p].load());
        }
    }
    return h;
}

void GsView::frame(Ui &ui)
{
    av.refresh();
    // Stages/Finales hide and reference settings: pick up changes made in other Gain Stages
    double nowt = now_sec();
    if ((double)shm::get()->gs.ver.load() != T.shver || nowt - shSyncT > 2) { T.shSync(); shSyncT = nowt; }
    buildLists();
    refreshRefs();
    int tb = tab();

    // The Finales reference is kept per chain. When another Act becomes that chain's finale (one was added
    // after it, say), the finales that followed the old one follow the new one.
    if (ref2 != T.MST[11]) {
        if (ref2 >= 100000 && T.MST[11] >= 100000)
            for (int s : LST2)
                if (kget(mps(s), P_REF) == T.MST[11]) kset(mps(s), P_REF, mid(s) == ref2 ? 0 : ref2);
        T.MST[11] = ref2;
    }
    // new instances on a tab with a reference start following it
    for (int t = 0; t < 3; ++t) {
        double rid = mref(t);
        if (rid < 100000) continue;
        for (int s : tablist(t))
            if (mid(s) != rid && !mhidden(s, t) && kget(mps(s), P_REF) < 100000 && live(s)) kset(mps(s), P_REF, rid);
    }

    // background and the subtitle
    if (ui.drawS()) ui.fill(R(0, 0, DW, DH), Paint::vertical(0, th::bg(), DH, th::bg2()));
    std::string sub = av.myslot >= 0 ? av.trackName(av.myslot) : std::string();
    if (!sub.empty()) sub += "   \xC2\xB7   ";
    sub += "no latency";
    st.subtitle = sub;

    R gear(DW - 14 - 32, 14, 32, 28);
    if (st.settings) ui.block(settingsRect(gear, SW, SH));
    topBar(ui);
    toolbar(ui);

    // the cards, scrolled
    R list(0, LIST_Y, DW, DH - LIST_Y);
    const auto ls = tablist(tb);
    bool showHid = truth(T.MST[6]);
    float total = 0;
    for (int s : ls) if (!mhidden(s, tb) || showHid) total += cardHeight(s, tb) + 8;
    int nh = tb == 0 ? nhid1 : (tb == 1 ? nhid3 : nhid2);
    bool emptyMain = tb == 0 && LST.size() <= 1, emptyFin = tb == 2 && LST2.empty();
    total += (emptyMain || emptyFin) ? 30 : 0;
    total += (nh > 0 && !showHid) ? 26 : 0;
    contentH = total + 12;
    float maxScroll = std::max(0.f, contentH - list.h);
    float &sc = scroll[tb];
    sc = std::max(0.f, std::min(sc, maxScroll));
    ui.beginArea(list);
    float y = LIST_Y + 6 - sc;
    for (int i = 0; i < (int)ls.size(); ++i) {
        int s = ls[i];
        if (mhidden(s, tb) && !showHid) continue;
        float h = cardHeight(s, tb);
        if (y + h >= LIST_Y - 10 && y <= DH + 10) card(ui, s, tb, i, y);
        y += h + 8;
    }
    if (ui.drawS()) {
        if (emptyMain) {
            ui.text(R(M + 4, y, DW - 2 * M, 22), "No Acts in this chain yet. Add TwoStorms Act after any plug-in you want to stage.", th::dim(), F_REGULAR, 12.5f);
            y += 30;
        }
        if (emptyFin) {
            ui.text(R(M + 4, y, DW - 2 * M, 22),
                    "No finales yet. The last Act in each track's chain is its finale (an Act's \xE2\x80\xA6 menu can change that).", th::dim(), F_REGULAR,
                    12.5f);
            y += 30;
        }
        if (nh > 0 && !showHid) {
            char hh[80];
            snprintf(hh, sizeof hh, "%d hidden. Click Hidden to show them.", nh);
            ui.text(R(M + 4, y, DW - 2 * M, 18), hh, th::faint(), F_REGULAR, 11.5f);
        }
    }
    ui.endArea();
    // the scroll bar (only when the cards don't fit) and the wheel
    if (maxScroll > 0) {
        R track(DW - 10, LIST_Y + 4, 5, list.h - 8);
        float th_ = std::max(30.f, track.h * list.h / contentH);
        float ty = track.y + (track.h - th_) * (sc / maxScroll);
        R thumb(track.x, ty, track.w, th_);
        if (ui.drawS()) {
            ui.fill(track, Paint(th::well()), 2.5f);
            ui.fill(thumb, Paint(sbDrag ? th::orange() : th::borderHi()), 2.5f);
        }
        if (ui.lclick && !ui.used && ui.hit(R(track.x - 4, track.y, track.w + 8, track.h))) {
            ui.used = true;
            sbDrag = true;
            sbY0 = ui.my();
            if (!ui.hitRaw(R(thumb.x - 4, thumb.y, thumb.w + 8, thumb.h))) sc = std::max(0.f, std::min(maxScroll, (ui.my() - track.y - th_ / 2) / (track.h - th_) * maxScroll));
            sbS0 = sc;
        }
        if (sbDrag) {
            if (!ui.ldown) sbDrag = false;
            else sc = std::max(0.f, std::min(maxScroll, sbS0 + (ui.my() - sbY0) * maxScroll / std::max(1.f, track.h - th_)));
        }
        if (ui.wheel != 0 && !ui.used && ui.hit(list)) {
            ui.used = true;
            sc = std::max(0.f, std::min(maxScroll, sc - ui.wheel * 60));
        }
    } else {
        sc = 0;
        sbDrag = false;
    }
    settings(ui, gear);
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_ANALYZER,
                                   CLAP_PLUGIN_FEATURE_SURROUND, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.gainstage", "TwoStorms Gain Stage", "TwoStorms", "", "", "", TS_VERSION,
         "Put it first in a chain: ON/OFF (glided) and input calibration with its own meter and Auto, and a remote for every TwoStorms "
         "plug-in: this chain (Main), every Gain Stage (Stages) and every chain's finale (Finales).",
         s_features},
        &table,
        &noPresets,
        "Gain Stage",
        "TWOSTORMS GAIN STAGE",
        shm::KIND_MASTER,
        shm::SK_ACT,
        false,
        false,
        0,
        &create,
        8,
    };
    return k;
}

} // namespace gs
} // namespace ts
