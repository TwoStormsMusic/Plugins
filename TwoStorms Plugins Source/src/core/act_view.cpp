#include "act_view.h"
#include "clock.h"
#include "eel.h"
#include "plugin.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ts {

using namespace shm;

static inline double ld(const AD &a) { return a.load(std::memory_order_relaxed); }
static inline int64_t li(const AI &a) { return a.load(std::memory_order_relaxed); }

ActView::ActView(Plugin &p) : P(p) {}

int ActView::mx() const { return (int)std::min<int64_t>(NSLOT, li(get()->h.maxslot)); }

void ActView::refresh()
{
    now = now_sec();
    myslot = P.act.slotPub.load(std::memory_order_relaxed);
    myid = P.act.idPub.load(std::memory_order_relaxed);
    if (myslot >= 0 && li(slot(myslot).owner) != myid) myslot = -1;
    finCompute();
}

double ActView::pv(int s, int p) const { return s < 0 ? P.get(p) : ld(slot(s).p[p]); }
int ActView::nch(int s) const { return 2 + 2 * iclamp(iround(pv(s, P_NCH)), 0, 3); }
bool ActView::used(int s, int c) const { return c < nch(s) && pv(s, P_USED + c) >= 0.5; }
bool ActView::linked(int s, int k) const { return pv(s, P_LINK + k) >= 0.5 && used(s, k * 2) && used(s, k * 2 + 1); }

int ActView::groups(int s, int *ga, int *gn) const
{
    int n = nch(s), cnt = 0;
    for (int k = 0; k < n / 2; ++k) {
        int a = k * 2;
        if (linked(s, k)) { ga[cnt] = a; gn[cnt] = 2; ++cnt; }
        else {
            if (used(s, a)) { ga[cnt] = a; gn[cnt] = 1; ++cnt; }
            if (used(s, a + 1)) { ga[cnt] = a + 1; gn[cnt] = 1; ++cnt; }
        }
    }
    return cnt;
}

int ActView::findId(int64_t id, int hint) const
{
    if (id < 100000) return -1;
    if (hint >= 0 && li(slot(hint).owner) == id) return hint;
    int m = mx();
    for (int s = 0; s < m; ++s)
        if (li(slot(s).owner) == id) return s;
    return -1;
}

bool ActView::live(int s) const { return s >= 0 && now - ld(slot(s).heart) < 1; }

bool ActView::named(int d) const
{
    if (d < 0) return false;
    int64_t o = li(slot(d).owner);
    return o > 0 && li(slot(d).nid) == o;
}

std::string ActView::trackName(int d) const
{
    if (d < 0) return "";
    char t[NAMELEN];
    int64_t id;
    if (named(d) && readNames(d, &id, t, nullptr, nullptr) && t[0]) return t;
    int64_t tn = li(slot(d).trackno);
    if (tn < 0) return "MASTER";
    if (tn == 0) return "";
    char b[32];
    snprintf(b, sizeof b, "Track %d", (int)tn);
    return b;
}

std::string ActView::prevName(int d) const
{
    char p[NAMELEN];
    int64_t id;
    if (named(d) && readNames(d, &id, nullptr, p, nullptr)) return p;
    return "";
}

const char *ActView::subName(int sk)
{
    switch (sk) {
    case SK_COMPRESSOR: return "TwoStorms Compressor";
    case SK_EQ: return "TwoStorms EQ";
    case SK_MULTIBAND: return "TwoStorms Multiband";
    case SK_IRLOADER: return "TwoStorms IR Loader";
    case SK_SATURATOR: return "TwoStorms Saturator";
    case SK_DELAY: return "TwoStorms Delay";
    case SK_CHORUS: return "TwoStorms Chorus";
    case SK_TREMOLO: return "TwoStorms Tremolo";
    case SK_DEFITCHER: return "TwoStorms Defitcher";
    case SK_GATE: return "TwoStorms Gate";
    case SK_WAH: return "TwoStorms Wah";
    default: return "Act";
    }
}

// 1-based position in its FX chain (input FX are numbered in their own chain)
int ActView::fxPos(int d) const
{
    double o = ld(slot(d).order);
    if (o < -50000) o += 100000;
    return (int)std::floor(o) + 1;
}

bool ActView::sameChain(int a, int b) const
{
    const Slot &A = slot(a), &B = slot(b);
    return li(A.track) == li(B.track) && (li(A.pflags) & PL_CHAIN_MASK) == (li(B.pflags) & PL_CHAIN_MASK);
}

// is there another live TwoStorms plug-in of the same kind in the chain of slot d?
bool ActView::dupKind(int d) const
{
    const Slot &me = slot(d);
    int m = mx();
    for (int s = 0; s < m; ++s) {
        const Slot &b = slot(s);
        if (s != d && li(b.owner) >= 100000 && li(b.kind) == li(me.kind) && li(b.subkind) == li(me.subkind) && sameChain(s, d) &&
            (s == myslot || live(s)))
            return true;
    }
    return false;
}

// a TwoStorms plug-in by its own name; an Act as "Act - OUT: <the plug-in before it>"
std::string ActView::pointTitle(int d) const
{
    if (d < 0) return "";
    const Slot &b = slot(d);
    int sk = (int)li(b.subkind), kd = (int)li(b.kind);
    std::string t;
    if (kd == KIND_MASTER) t = "TwoStorms Gain Stage";
    else if (sk > 0) t = subName(sk);
    else {
        std::string pn = prevName(d);
        if (!pn.empty()) t = "Act - OUT: " + pn;
        else {
            char buf[48];
            snprintf(buf, sizeof buf, "Act (FX %d)", fxPos(d));
            t = buf;
        }
    }
    if ((kd == KIND_MASTER || sk > 0) && dupKind(d)) {
        char buf[32];
        snprintf(buf, sizeof buf, " (FX %d)", fxPos(d));
        t += buf;
    }
    return t;
}

std::string ActView::titleOfId(int64_t id) const
{
    int s = findId(id);
    return s >= 0 ? pointTitle(s) : "(missing)";
}

// ---------------------------------------------------------------- finales
static bool finListed(const ActView &v, int s)
{
    const Slot &b = slot(s);
    return li(b.owner) >= 100000 && !(li(b.pflags) & PL_TAKE) &&
           (s == v.myslot || v.live(s) || (v.named(s) && (li(b.nflags) & NF_INPROJECT)));
}
static bool finActive(const ActView &v, int s)
{
    const Slot &b = slot(s);
    return v.named(s) ? (li(b.nflags) & (NF_ENABLED | NF_OFFLINE)) == NF_ENABLED : (s == v.myslot || v.live(s));
}
static double finOrder(const ActView &v, int s) { return v.named(s) ? (double)li(slot(s).norder) : ld(slot(s).order); }

// FIN: 0 = not a finale, 1 = its chain's last Act, 2 = set to Always. FKEY: where the Finales tab
// keeps its settings (the chain's Gain Stage for the main finale, else the Act itself).
void ActView::finCompute()
{
    int m = mx();
    FIN.assign(m, 0);
    FKEY.assign(m, 0);
    for (int s = 0; s < m; ++s) FIN[s] = (li(slot(s).kind) == KIND_POINT && finListed(*this, s)) ? -1 : 0;
    for (int s = 0; s < m; ++s) {
        if (FIN[s] >= 0) continue;
        bool alw = false, ba = false;
        int best = -1, ab = -1;
        double bo = -1e9, abo = -1e9;
        int64_t gs = 0;
        for (int t = 0; t < m; ++t) {
            const Slot &b = slot(t);
            if (li(b.owner) < 100000 || !sameChain(s, t)) continue;
            if (FIN[t] < 0) {
                int fm = iround(ld(b.p[P_FINAL]));
                double o = finOrder(*this, t);
                if (fm == FIN_ALWAYS) { alw = true; if (o > abo) { abo = o; ab = t; } }
                if (fm == FIN_AUTO) {
                    bool a = finActive(*this, t);
                    if ((a && !ba) || (a == ba && o > bo)) { best = t; bo = o; ba = a; }
                }
            } else if (li(b.kind) == KIND_MASTER && gs == 0 && finListed(*this, t)) {
                gs = li(b.owner);
            }
        }
        int prim = alw ? ab : best;
        for (int t = 0; t < m; ++t) {
            if (FIN[t] < 0 && sameChain(s, t)) {
                const Slot &b = slot(t);
                FIN[t] = alw ? (iround(ld(b.p[P_FINAL])) == FIN_ALWAYS ? 2 : 0) : (t == best ? 1 : 0);
                FKEY[t] = (int)((t == prim && gs >= 100000) ? gs : li(b.owner));
            }
        }
    }
}

int ActView::finOfChain(int d) const
{
    int f = -1;
    double fo = -1e9;
    for (int t = 0; t < (int)FIN.size(); ++t)
        if (FIN[t] > 0 && sameChain(d, t) && finOrder(*this, t) > fo) { f = t; fo = finOrder(*this, t); }
    return f;
}

bool ActView::finTail(int d) const { return named(d) && (li(slot(d).nflags) & NF_TAIL); }

std::string ActView::finaleStatus(bool *warn) const
{
    *warn = false;
    int fm = iround(P.get(P_FINAL));
    int f = fin();
    if (fm == FIN_NEVER) return "Never on the Finales tab.";
    if (f) {
        if (finTail(myslot)) { *warn = true; return "Finale, but plug-ins after it change the output."; }
        return f == 2 ? "Always on the Finales tab." : "The finale: the last Act in its chain.";
    }
    int fc = myslot >= 0 ? finOfChain(myslot) : -1;
    if (fc >= 0) return "Not the finale: " + pointTitle(fc) + " is.";
    return "Not the finale.";
}

// ---------------------------------------------------------------- Auto
bool ActView::autoBusy(int s) const
{
    int d = ds(s);
    return d >= 0 && li(slot(d).astate) != AS_IDLE;
}

int ActView::autoMode(int s) const
{
    int d = ds(s);
    int m = iround(pv(s, P_AMODE));
    return d >= 0 ? iclamp(m, 0, 2) : 0;
}

int ActView::refState(int s) const
{
    int d = ds(s);
    int64_t rid = (int64_t)pv(s, P_REF);
    int rs = (d >= 0 && rid >= 100000 && rid != li(slot(d).owner)) ? findId(rid) : -1;
    if (rs < 0) return 1;
    const Slot &b = slot(d), &rb = slot(rs);
    if ((sameChain(rs, d) && ld(rb.order) > ld(b.order)) || (named(d) && (li(b.nflags) & NF_REFFED))) return 2;
    return 0;
}

double ActView::aimTick(int s) const { return autoMode(s) == 0 ? pv(s, P_TGT) : pv(s, P_AIM); }

void ActView::autoClick(int s)
{
    int d = ds(s);
    if (d < 0) return;
    if (li(slot(d).astate) != AS_IDLE) send(d, C_STOP, 0, 0, 0);
    else send(d, C_AUTO, autoMode(s), 0, 0);
}

const char *ActView::autoLabel(int s) const
{
    int d = ds(s);
    int st = d >= 0 ? (int)li(slot(d).astate) : 0;
    return st == AS_MEAS ? "Stop" : (st != AS_IDLE ? "Cancel" : "Auto");
}

std::string ActView::autoTimeText(int s, bool *busy, bool *nosig) const
{
    int d = ds(s);
    *busy = false;
    *nosig = false;
    if (d < 0) return "";
    const Slot &b = slot(d);
    int st = (int)li(b.astate);
    if (st == AS_IDLE) {
        if (li(b.result) == R_NOSIG && now - ld(b.donet) < 4) *nosig = true;
        return "";
    }
    *busy = true;
    if (st == AS_MEAS) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.1f s", ld(b.atime));
        return buf;
    }
    return st == AS_QUEUED ? "queued" : "waiting";
}

int ActView::prevSlot(int d) const
{
    int best = -1;
    if (d < 0) return -1;
    double bo = -1e9, myo = ld(slot(d).order);
    int m = mx();
    for (int s = 0; s < m; ++s) {
        const Slot &b = slot(s);
        if (s != d && li(b.owner) >= 100000 && live(s) && sameChain(s, d)) {
            double o = ld(b.order);
            if (o < myo && o > bo) { bo = o; best = s; }
        }
    }
    return best;
}

std::vector<int> ActView::chainPoints(int d) const
{
    std::vector<int> out;
    if (d < 0) return out;
    int m = mx();
    for (int s = 0; s < m; ++s) {
        const Slot &b = slot(s);
        if (s != d && li(b.owner) >= 100000 && li(b.kind) == KIND_POINT && live(s) && sameChain(s, d)) out.push_back(s);
    }
    // in chain order
    std::sort(out.begin(), out.end(), [](int a, int b) { return ld(slot(a).order) < ld(slot(b).order); });
    return out;
}

std::string ActView::valueText(int s, bool *ok) const
{
    int m = autoMode(s);
    *ok = false;
    std::string name;
    if (m == 2) {
        int rs = refState(s);
        if (rs == 1) return "set reference in Gain Stage";
        if (rs == 2) return "reference is after this";
        name = titleOfId((int64_t)pv(s, P_REF));
    } else {
        int ps = prevSlot(ds(s));
        if (ps < 0) return "no earlier Act";
        name = pointTitle(ps);
    }
    *ok = true;
    double aim = pv(s, P_AIM);
    if (aim > -150) {
        char buf[64];
        snprintf(buf, sizeof buf, pv(s, P_MEAS) >= 1.5 ? "%.1f LUFS - " : "%.1f dBFS - ", aim);
        return buf + name;
    }
    return name;
}

std::string ActView::srcText(int s, int a, bool compact) const
{
    int m = autoMode(s);
    int sel = iround(pv(s, P_MSRC + a));
    char sm[32];
    if (sel == 0) snprintf(sm, sizeof sm, "Same Ch");
    else if (sel == 9) snprintf(sm, sizeof sm, "All Ch");
    else if (sel <= 8) snprintf(sm, sizeof sm, "ch %d", sel);
    else snprintf(sm, sizeof sm, "ch %d-%d", (sel - 11) * 2 + 1, (sel - 11) * 2 + 2);
    if (compact) return sm;
    return std::string(m == 1 ? "Match: " : "Ref: ") + sm;
}

// ---------------------------------------------------------------- drive link
void ActView::setDrive(int s, int64_t idb)
{
    int d = ds(s);
    // ask for the next plug-in parameter that gets moved
    if (idb == DRIVE_PARAM && d >= 0) slot(d).dlearn.fetch_add(1);
    if (s < 0) {
        P.setOnce(P_DRIVE, (double)idb);
        if (idb > 0) P.setOnce(P_DRIVEON, 1);
        if (idb == DRIVE_PARAM) P.driveLearn();
    } else {
        send(s, C_SET, P_DRIVE, (double)idb, 0);
        slot(s).p[P_DRIVE].store((double)idb);
        if (idb > 0) { send(s, C_SET, P_DRIVEON, 1, 0); slot(s).p[P_DRIVEON].store(1); }
    }
}

std::string ActView::driveLabel(int s) const
{
    double cv = pv(s, P_DRIVE);
    std::string t;
    if (iround(cv) == DRIVE_PARAM) {
        int st;
        std::string nm;
        if (s < 0) { st = P.driveStatus(); nm = P.driveName(); }
        else {
            int d = ds(s);
            st = d >= 0 ? (int)li(slot(d).dstat) : 0;
            char dn[NAMELEN];
            int64_t id;
            if (d >= 0 && named(d) && readNames(d, &id, nullptr, nullptr, dn)) nm = dn;
        }
        if (!P.inReaper()) t = "Drive link: plug-in parameters need REAPER";
        else if (st == DS_LINKED) t = "Drive link: " + nm;
        else if (st == DS_LEARNING) t = "Drive link: now move the plug-in parameter to link";
        else if (st == DS_GONE) t = "Drive link: the linked parameter is gone";
        else if (st == DS_NOTDB) t = "Drive link: that parameter doesn't show dB";
        else t = "Drive link: a plug-in parameter";
    } else if (cv >= 100000) {
        int ps = findId((int64_t)cv);
        int d = ds(s);
        if (ps >= 0 && d >= 0 && sameChain(ps, d)) t = "Drive link: " + titleOfId((int64_t)cv);
        else t = "Drive link: partner not in this chain";
    } else {
        t = "Drive link: none";
    }
    if (cv >= DRIVE_PARAM && pv(s, P_DRIVEON) < 0.5) t += " (bypassed)";
    return t;
}

} // namespace ts
