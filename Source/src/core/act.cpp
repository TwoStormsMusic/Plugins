#include "act.h"
#include "clock.h"
#include "eel.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace ts {

using namespace shm;

static inline double ld(const AD &a) { return a.load(std::memory_order_relaxed); }
static inline int64_t li(const AI &a) { return a.load(std::memory_order_relaxed); }
static inline void st(AD &a, double v) { a.store(v, std::memory_order_relaxed); }
static inline void si(AI &a, int64_t v) { a.store(v, std::memory_order_relaxed); }

// ---------------------------------------------------------------- parameters
static const char *const L_NCH[] = {"2", "4", "6", "8"};
static void fmt_tgt(double v, char *b, int n) { snprintf(b, n, "%.1f", v); }

void addActParams(ParamTable &t, bool fixedPair)
{
    const char *M = "Output";
    t.addEnum(P_NCH, "Channels", {"2", "4", "6", "8"}, 0, 0, M);
    t.addEnum(P_FINAL, "Finale", {"Auto", "Always", "Never"}, 0, 0, M);
    t.addEnum(P_MEAS, "Auto measure", {"Peak", "RMS", "LUFS"}, 0, 0, M);
    t.add(P_TGT, "Auto target", -60, 0, -12, 0, fmt_tgt, M);
    t.addEnum(P_TIME, "Auto time", {"Manual", "1 s", "2 s", "3 s", "5 s", "10 s", "20 s", "30 s"}, 4, 0, M);
    static const char *linkNames[4] = {"Link 1-2", "Link 3-4", "Link 5-6", "Link 7-8"};
    for (int k = 0; k < 4; ++k) t.add(P_LINK + k, linkNames[k], 0, 1, 1, PF_INT | PF_BOOL, nullptr, M);
    static const char *gainNames[8] = {"Ch1 gain", "Ch2 gain", "Ch3 gain", "Ch4 gain", "Ch5 gain", "Ch6 gain", "Ch7 gain", "Ch8 gain"};
    static const char *usedNames[8] = {"Ch1 used", "Ch2 used", "Ch3 used", "Ch4 used", "Ch5 used", "Ch6 used", "Ch7 used", "Ch8 used"};
    static const char *srcNames[8] = {"Ch1 match source", "Ch2 match source", "Ch3 match source", "Ch4 match source",
                                      "Ch5 match source", "Ch6 match source", "Ch7 match source", "Ch8 match source"};
    for (int c = 0; c < 8; ++c) {
        uint32_t f = 0;
        const char *nm = gainNames[c];
        if (fixedPair) { if (c == 0) { f = PF_HOST; nm = "Output"; } }
        else f = PF_HOST;
        t.add(P_GAIN + c, nm, GFLOOR, 60, 0, f, fmt_gain, M);
    }
    for (int c = 0; c < 8; ++c) t.add(P_USED + c, usedNames[c], 0, 1, 1, PF_INT | PF_BOOL, nullptr, M);
    for (int c = 0; c < 8; ++c) t.add(P_MSRC + c, srcNames[c], 0, 14, 0, PF_INT, fmt_int, M);
    t.add(P_DRIVE, "Drive link target ID", 0, 999999, 0, PF_INT, fmt_int, M);
    t.add(P_REF, "Reference ID", 0, 999999, 0, PF_INT, fmt_int, M);
    t.add(P_ID, "Instance ID", 0, 999999, 0, PF_INT, fmt_int, M);
    t.addEnum(P_HOLD, "Peak hold", {"Off", "1 s", "2 s", "5 s", "10 s", "Infinite"}, 3, 0, M);
    t.addEnum(P_RANGE, "Meter range", {"24 dB", "36 dB", "48 dB", "60 dB", "72 dB", "96 dB"}, 3, 0, M);
    t.add(P_DRIVEON, "Drive link engaged", 0, 1, 1, PF_INT | PF_BOOL, nullptr, M);
    t.addEnum(P_AMODE, "Auto mode", {"Target", "Match", "Reference"}, 0, 0, M);
    t.add(P_AIM, "Level the last Match/Reference aimed for", -200, 60, -200, 0, nullptr, M);
    t.addEnum(P_ZOOM, "Window zoom", {"100%", "115%", "130%", "150%", "175%", "200%"}, 1, 0, M);
    (void)L_NCH;
}

double act_time_sec(double v)
{
    int t = iround(v);
    return t == 0 ? 0 : (t == 1 ? 1 : (t == 2 ? 2 : (t == 3 ? 3 : (t == 4 ? 5 : (t == 5 ? 10 : (t == 6 ? 20 : 30))))));
}
double act_hold_sec(double v)
{
    int t = iround(v);
    return t == 0 ? 0 : (t == 1 ? 1 : (t == 2 ? 2 : (t == 3 ? 5 : (t == 4 ? 10 : -1))));
}
double act_range_db(double v)
{
    int t = iround(v);
    return t == 0 ? 24 : (t == 1 ? 36 : (t == 2 ? 48 : (t == 3 ? 60 : (t == 4 ? 72 : 96))));
}
double act_zoom(double v)
{
    int t = iround(v);
    return t <= 0 ? 1 : (t == 1 ? 1.15 : (t == 2 ? 1.3 : (t == 3 ? 1.5 : (t == 4 ? 1.75 : 2))));
}

// ---------------------------------------------------------------- setup
Act::Act(ActHost &h, int kind_, int subkind_) : kind(kind_), subkind(subkind_), H(h)
{
    std::random_device rd;
    std::mt19937_64 rng(((uint64_t)rd() << 32) ^ rd() ^ (uint64_t)(now_sec() * 1e9));
    mynonce = 1 + (int64_t)(rng() % 1000000000000ULL);
    MB.assign(8 * MAXB, 0.0);
    ZW.assign(MAXB / 10 + 8, 0.0);
    DFID.assign(NSLOT, 0);
    DFLAST.assign(NSLOT, 0.0);
    for (int c = 0; c < 8; ++c) { GLIN[c] = 1; PKD[c] = -200; PKH[c] = -200; RMH[c] = -200; }
    get();   // attach shared memory now (main thread), not on the first audio block
}

void Act::init(double sr)
{
    srate = sr;
    // K-weighting (ITU-R BS.1770) for the Loudness measure
    // (EEL2: a / binds tighter than a *, so a * b / c is a * (b / c); written out here)
    double a_kk = std::tan(PI * (1681.974450955533 / srate));
    double a_vh = epow(10, 3.999843853973347 / 20), a_vb = epow(a_vh, 0.4996667741545416), a_q = 0.7071752369554196;
    double a_a0 = 1 + a_kk / a_q + a_kk * a_kk;
    KB0 = (a_vh + a_vb * (a_kk / a_q) + a_kk * a_kk) / a_a0;
    KB1 = 2 * ((a_kk * a_kk - a_vh) / a_a0);
    KB2 = (a_vh - a_vb * (a_kk / a_q) + a_kk * a_kk) / a_a0;
    KA1 = 2 * ((a_kk * a_kk - 1) / a_a0);
    KA2 = (1 - a_kk / a_q + a_kk * a_kk) / a_a0;
    a_kk = std::tan(PI * (38.13547087602444 / srate));
    a_q = 0.5003270373238773;
    a_a0 = 1 + a_kk / a_q + a_kk * a_kk;
    KC1 = 2 * ((a_kk * a_kk - 1) / a_a0);
    KC2 = (1 - a_kk / a_q + a_kk * a_kk) / a_a0;
    a_lbinlen = (int)emax(1, std::floor(srate * 0.01 + 0.5));
    a_glide = (int)emax(1, std::floor(srate * 0.02));
    a_inited = false;
    a_prevblk = 0;
    AST = AS_IDLE;
    AU_KIN = false;
    a_inm = false; a_binon = false; a_kout = false; a_kout_was = false;
    AU_LMODE = -1; AU_LMEAS = -1; AU_LREF = -1;
    memset(KIN, 0, sizeof KIN);
    memset(KOUT, 0, sizeof KOUT);
}

// ---------------------------------------------------------------- shared helpers (audio copy)
double Act::a_pv(int s, int p) const { return s < 0 ? H.pv(p) : ld(slot(s).p[p]); }
int Act::a_nch(int s) const { return 2 + 2 * iclamp((int)std::floor(a_pv(s, P_NCH) + 0.5), 0, 3); }
bool Act::a_used(int s, int c) const { return c < a_nch(s) && a_pv(s, P_USED + c) >= 0.5; }
bool Act::a_linked(int s, int k) const { return a_pv(s, P_LINK + k) >= 0.5 && a_used(s, k * 2) && a_used(s, k * 2 + 1); }

int Act::a_groups(int s, int *ga, int *gn) const
{
    int n = a_nch(s), cnt = 0;
    for (int k = 0; k < n / 2; ++k) {
        int a = k * 2;
        if (a_linked(s, k)) { ga[cnt] = a; gn[cnt] = 2; cnt++; }
        else {
            if (a_used(s, a)) { ga[cnt] = a; gn[cnt] = 1; cnt++; }
            if (a_used(s, a + 1)) { ga[cnt] = a + 1; gn[cnt] = 1; cnt++; }
        }
    }
    return cnt;
}

int Act::a_find_id(int64_t id, int hint) const
{
    int f = -1;
    if (id >= 100000) {
        if (hint >= 0 && li(slot(hint).owner) == id) f = hint;
        else {
            int mx = (int)std::min<int64_t>(NSLOT, li(get()->h.maxslot));
            for (int s = 0; s < mx && f < 0; ++s)
                if (li(slot(s).owner) == id) f = s;
        }
    }
    return f;
}

bool Act::a_live(int s, double now) const { return s >= 0 && now - ld(slot(s).heart) < 1; }

bool Act::sameChain(int s) const
{
    const Slot &b = slot(s);
    return li(b.track) == a_trk && (li(b.pflags) & PL_CHAIN_MASK) == (a_pf & PL_CHAIN_MASK);
}

void Act::a_setsl(int p, double v) { H.setFromAudio(p, v); }

// set a parameter of this instance, keeping linked pairs on one fader
void Act::a_apply(int p, double v)
{
    v = H.clampParam(p, v);
    a_setsl(p, v);
    if (p >= P_GAIN && p < P_GAIN + 8) {
        int c = p - P_GAIN;
        LASTG[c] = v;
        int k = c / 2, pc = c - (c % 2) + (1 - c % 2);
        if (a_linked(-1, k)) { a_setsl(P_GAIN + pc, v); LASTG[pc] = v; }
    }
    if (p >= P_LINK && p < P_LINK + 4) {
        int k = p - P_LINK;
        if (a_linked(-1, k)) {
            double v2 = H.pv(P_GAIN + k * 2);
            a_setsl(P_GAIN + k * 2 + 1, v2);
            LASTG[k * 2 + 1] = v2;
        }
    }
    if (p >= P_USED && p < P_USED + 8) {
        int c = p - P_USED, k = c / 2, pc = c - (c % 2) + (1 - c % 2);
        if (v >= 0.5 && a_linked(-1, k)) {
            double v2 = H.pv(P_GAIN + pc);
            a_setsl(P_GAIN + c, v2);
            LASTG[c] = v2;
        }
    }
}

// ---------------------------------------------------------------- slot keeping
void Act::a_new_id()
{
    if (myslot >= 0) si(slot(myslot).owner, 0);
    static thread_local std::mt19937_64 rng((uint64_t)(now_sec() * 7919e6) ^ (uint64_t)(uintptr_t)this);
    int64_t id = 100000 + (int64_t)(rng() % 900000);
    a_setsl(P_ID, (double)id);
    myid = id;
    myslot = -1;
}

static void clear_slot(Slot &b)
{
    // everything after the owner field
    char *p = (char *)&b + offsetof(Slot, nonce);
    memset(p, 0, sizeof(Slot) - offsetof(Slot, nonce));
}

void Act::a_claim()
{
    Shared *S = get();
    myid = (int64_t)H.pv(P_ID);
    if (myid < 100000) a_new_id();
    if (myslot >= 0) {
        Slot &b = slot(myslot);
        if (li(b.owner) != myid || li(b.nonce) != mynonce) myslot = -1;
    }
    if (myslot < 0) {
        int f = a_find_id(myid, -1);
        if (f >= 0) {
            Slot &b = slot(f);
            if (li(b.nonce) == mynonce) {
                myslot = f;
            } else {
                // Another instance carries this ID (a copied plug-in), or this one restarted.
                // Watch that slot's heartbeat for a quarter second to tell which.
                if (DUPS != f) { DUPS = f; DUPH = li(b.hbc); DUPM = ld(b.mheart); DUPT = 0; }
                DUPT += samplesblock / srate;
                // alive: its audio runs, or (bypassed) its main thread still keeps the slot
                if (li(b.hbc) != DUPH || ld(b.mheart) != DUPM) {
                    a_new_id();
                    DUPS = -1;
                } else if (DUPT >= 0.25) {
                    si(b.nonce, mynonce);
                    st(b.heart, now_sec());
                    myslot = f;
                    DUPS = -1;
                    S->h.slotver.fetch_add(1);
                }
            }
        } else {
            double t = now_sec();
            for (int s = 0; s < NSLOT && f < 0; ++s) {
                Slot &b = slot(s);
                int64_t o = li(b.owner);
                // free, or left behind (no audio and no main thread for 2 s: not merely bypassed)
                if (o < 100000 || (t - ld(b.heart) > 2 && t - ld(b.mheart) > 2)) {
                    if (b.owner.compare_exchange_strong(o, myid)) f = s;
                }
            }
            if (f >= 0) {
                Slot &b = slot(f);
                clear_slot(b);
                si(b.nonce, mynonce);
                st(b.heart, now_sec());
                int64_t mx = li(S->h.maxslot);
                while (f + 1 > mx && !S->h.maxslot.compare_exchange_weak(mx, f + 1)) {}
                S->h.slotver.fetch_add(1);
                myslot = f;
            }
        }
    }
}

// two live slots with one ID (both started at once): the higher slot takes a new ID
void Act::a_dupcheck()
{
    AU_DUPT += samplesblock / srate;
    if (AU_DUPT >= 0.5) {
        AU_DUPT = 0;
        bool dup = false;
        int mx = (int)std::min<int64_t>(NSLOT, li(get()->h.maxslot));
        for (int s = 0; s < mx; ++s)
            if (s < myslot && li(slot(s).owner) == myid && a_live(s, a_now)) dup = true;
        if (dup) a_new_id();
    }
}

void Act::a_reset_hold()
{
    for (int c = 0; c < 8; ++c) { PKH[c] = PKD[c]; PKHT[c] = 0; RMH[c] = -200; RMHT[c] = 0; CLIPF[c] = 0; }
}

int Act::a_first_used(int s) const
{
    int f = -1;
    int n = a_nch(s);
    for (int c = n - 1; c >= 0; --c)
        if (a_used(s, c)) f = c;
    return f < 0 ? 0 : f;
}

// drive link: the source moved by delta (in its group gi of sng groups; sng 0 = all of this
// point's groups); move the opposite way, and remember it was the link that moved it
void Act::a_addgain(int gi, double delta, int sng)
{
    int ng = a_groups(-1, AGA, AGN);
    for (int g = 0; g < ng; ++g) {
        if (sng == ng ? g == gi : true) {
            int a = AGA[g];
            double cv = H.pv(P_GAIN + a);
            if (cv > GFLOOR) {
                double nv = emax(-99.9, emin(60, cv - delta));
                a_apply(P_GAIN + a, nv);
                DRVS[a] += nv - cv;
                if (AGN[g] == 2) DRVS[a + 1] += nv - cv;
            }
        }
    }
}

// Every Act on this chain whose drive link points here moves this Act the opposite way when its
// first fader moves (minus what drive links did to it). While its link isn't engaged, nothing moves.
void Act::a_drive_follow()
{
    int mx = (int)std::min<int64_t>(NSLOT, li(get()->h.maxslot));
    for (int s = 0; s < mx; ++s) {
        Slot &b = slot(s);
        int64_t id = li(b.owner);
        if (s != myslot && id >= 100000 && ld(b.p[P_DRIVE]) == (double)myid &&
            li(b.kind) == KIND_POINT && a_live(s, a_now) && sameChain(s)) {
            int a = a_first_used(s);
            double gv = ld(b.p[P_GAIN + a]);
            gv = gv > GFLOOR ? gv - ld(b.drvs[a]) : GFLOOR;
            if (DFID[s] != id || DFLAST[s] <= GFLOOR || gv <= GFLOOR) {
                DFID[s] = id;
                DFLAST[s] = gv;
            } else if (gv != DFLAST[s]) {
                if (ld(b.p[P_DRIVEON]) >= 0.5) a_addgain(0, gv - DFLAST[s], 0);
                DFLAST[s] = gv;
            }
        } else {
            DFID[s] = 0;
        }
    }
}

// ---------------------------------------------------------------- Auto
void Act::a_auto_end(int res)
{
    tgpend = false;
    AST = AS_IDLE;
    AU_RESULT = res;
    AU_RESSEQ += 1;
    AU_DONET = a_now;
}

// previous Act (or Gain Stage) in this chain, by chain order
int Act::a_prev_slot() const
{
    int best = -1;
    double bo = -1000000000;
    int mx = (int)std::min<int64_t>(NSLOT, li(get()->h.maxslot));
    for (int s = 0; s < mx; ++s) {
        const Slot &b = slot(s);
        if (s != myslot && li(b.owner) >= 100000 && a_live(s, a_now) && sameChain(s)) {
            double o = ld(b.order);
            if (o < a_order && o > bo) { bo = o; best = s; }
        }
    }
    return best;
}

// Reference mode: can this instance aim at the instance in slot s? 0 = yes, else the reason
int Act::a_ref_check(int s) const
{
    const Slot &b = slot(s);
    if (sameChain(s) && ld(b.order) > a_order) return R_REFAFTER;
    const Slot &me = slot(myslot);
    if (li(me.nid) == myid && (li(me.nflags) & NF_REFFED)) return R_REFAFTER;
    return 0;
}

// Auto all: is this instance's reference busy in the same run? (a loop of references doesn't wait)
bool Act::a_ref_busy() const
{
    bool busy = false;
    int s = a_find_id((int64_t)H.pv(P_REF), -1);
    if (s >= 0 && s != myslot && a_live(s, a_now)) {
        const Slot &b = slot(s);
        if (li(b.runid) == ARUN && li(b.astate) != AS_IDLE) {
            bool cyc = false;
            int t = s, k = 0;
            while (k < 8 && t >= 0 && !cyc && iround(ld(slot(t).p[P_AMODE])) == 2) {
                t = a_find_id((int64_t)ld(slot(t).p[P_REF]), -1);
                if (t == myslot) cyc = true;
                k++;
            }
            busy = !cyc;
        }
    }
    return busy;
}

// Auto all runs one chain in order: wait while an earlier point of this run is busy
bool Act::a_preds_busy() const
{
    bool busy = false;
    int mx = (int)std::min<int64_t>(NSLOT, li(get()->h.maxslot));
    for (int s = 0; s < mx; ++s) {
        const Slot &b = slot(s);
        if (s != myslot && li(b.owner) >= 100000 && a_live(s, a_now) && sameChain(s) &&
            li(b.runid) == ARUN && ld(b.order) < a_order) {
            if (li(b.astate) != AS_IDLE) busy = true;
        }
    }
    return busy;
}

void Act::a_auto_start(int mode, int64_t run)
{
    AMODE = mode;
    ARUN = run;
    AU_DUR = act_time_sec(H.pv(P_TIME));
    if (AU_DUR == 0 && run != 0) AU_DUR = 5;
    AU_KIN = H.pv(P_MEAS) >= 1.5;
    memset(KIN, 0, sizeof KIN);
    for (int c = 0; c < 8; ++c) { MPK[c] = 0; MSUM[c] = 0; MBACC[c] = 0; CSUM[c] = 0; CPK[c] = 0; }
    AU_NB = 0; AU_BPOS = 0; AU_NS = 0; AU_MN = 0; AU_CN = 0; AU_T = 0; AU_QT = 0; AU_SETTLE = 0;
    AU_SEEN = false; AU_CSEQ = -1; AU_LIMIT = 0;
    AU_SRC = -1; AU_SRCID = 0;
    AST = run != 0 ? AS_QUEUED : AS_WAIT;
    if (mode == 1) {
        AU_SRC = a_prev_slot();
        if (AU_SRC < 0) a_auto_end(R_NOPREV);
        else AU_SRCID = li(slot(AU_SRC).owner);
    }
    if (mode == 2) {
        AU_SRCID = (int64_t)H.pv(P_REF);
        AU_SRC = (AU_SRCID >= 100000 && AU_SRCID != myid) ? a_find_id(AU_SRCID, -1) : -1;
        if (AU_SRC < 0) a_auto_end(R_NOREF);
        else if (!a_live(AU_SRC, a_now)) a_auto_end(R_REFGONE);
        else {
            int why = a_ref_check(AU_SRC);
            if (why) a_auto_end(why);
        }
    }
}

// a new mode, measure or reference makes the last aimed-for level meaningless
void Act::a_aim_check()
{
    int m = iround(H.pv(P_AMODE)), me = iround(H.pv(P_MEAS));
    double r = H.pv(P_REF);
    if (AU_LMODE >= 0 && (m != AU_LMODE || me != AU_LMEAS || (m == 2 && r != AU_LREF)) && H.pv(P_AIM) > -150)
        a_setsl(P_AIM, -200);
    AU_LMODE = m;
    AU_LMEAS = me;
    AU_LREF = r;
}

// level of an auto-stage group (anchor channel a, n channels) from the input bins
double Act::a_level_auto(int a, int n)
{
    double m = H.pv(P_MEAS);
    double lv = -200;
    if (m < 0.5) {
        for (int c = a; c < a + n; ++c)
            if (MPK[c] > 0) lv = emax(lv, 20 * std::log10(MPK[c]));
    } else if (m < 1.5) {
        // RMS of the louder channel, ignoring 10 ms bins more than 20 dB below the loudest one
        for (int c = a; c < a + n; ++c) {
            double maxe = 0;
            for (int i = 0; i < AU_NB; ++i) { double e = MB[c * MAXB + i]; if (e > maxe) maxe = e; }
            double thr = emax(maxe * 0.01, 0.0000001);
            double sum = 0;
            int cnt = 0;
            for (int i = 0; i < AU_NB; ++i) { double e = MB[c * MAXB + i]; if (e >= thr) { sum += e; cnt++; } }
            if (cnt > 0) lv = emax(lv, 10 * std::log10(sum / cnt));
        }
    } else {
        // integrated loudness (BS.1770): 400 ms blocks every 100 ms, gated at -70 LUFS and 10 LU below
        int nz = AU_NB / 10;
        for (int j = 0; j < nz; ++j) {
            double z = 0;
            for (int i = j * 10; i < j * 10 + 10; ++i)
                for (int c = a; c < a + n; ++c) z += MB[c * MAXB + i];
            ZW[j] = z / 10;
        }
        int nw = nz - 3;
        if (nw < 1) {
            double z = 0;
            for (int i = 0; i < AU_NB; ++i)
                for (int c = a; c < a + n; ++c) z += MB[c * MAXB + i];
            z = AU_NB > 0 ? z / AU_NB : 0;
            if (z > 0) lv = -0.691 + 10 * std::log10(z);
        } else {
            double gsum = 0;
            int gcnt = 0;
            for (int j = 0; j < nw; ++j) {
                double w = (ZW[j] + ZW[j + 1] + ZW[j + 2] + ZW[j + 3]) / 4;
                if (w > 0 && -0.691 + 10 * std::log10(w) > -70) { gsum += w; gcnt++; }
            }
            if (gcnt > 0) {
                double rel = -0.691 + 10 * std::log10(gsum / gcnt) - 10;
                gsum = 0;
                gcnt = 0;
                for (int j = 0; j < nw; ++j) {
                    double w = (ZW[j] + ZW[j + 1] + ZW[j + 2] + ZW[j + 3]) / 4;
                    if (w > 0 && -0.691 + 10 * std::log10(w) > emax(-70, rel)) { gsum += w; gcnt++; }
                }
                if (gcnt > 0) lv = -0.691 + 10 * std::log10(gsum / gcnt);
            }
        }
    }
    if (lv < -70) lv = -200;
    return lv;
}

// Match: this point's input level for a group, over the same blocks as the previous point's output
double Act::a_level_in(int a, int n) const
{
    double m = H.pv(P_MEAS);
    double lv = -200;
    if (m < 0.5) {
        for (int c = a; c < a + n; ++c)
            if (MPK[c] > 0) lv = emax(lv, 20 * std::log10(MPK[c]));
    } else {
        double e = 0;
        for (int c = a; c < a + n; ++c) e += MSUM[c];
        e = AU_MN > 0 ? e / ((double)n * AU_MN) : 0;
        if (e > 0) lv = 10 * std::log10(e);
    }
    if (lv < -70) lv = -200;
    return lv;
}

// Match: the previous point's output level on this group's source channel(s)
double Act::a_level_src(int a, int n) const
{
    double m = H.pv(P_MEAS);
    int src = iround(H.pv(P_MSRC + a));
    if (src == 9 || src == 10) src = 0;
    int cn = a_nch(AU_SRC);
    int c1, c2;
    if (src == 0) {
        // Auto: the same channel(s), wrapped onto the previous point's channel count
        if (n == 2) { int k = (a / 2) % (cn / 2); c1 = k * 2; c2 = c1 + 1; }
        else { c1 = a % cn; c2 = -1; }
    } else {
        if (src <= 8) { c1 = src - 1; c2 = -1; }
        else { c1 = (src - 11) * 2; c2 = c1 + 1; }
    }
    if (c1 >= cn) c1 = c1 % cn;
    if (c2 >= cn) c2 = -1;
    if (c2 >= 0 && !a_used(AU_SRC, c2)) c2 = -1;
    if (!a_used(AU_SRC, c1)) {
        if (c2 >= 0) { c1 = c2; c2 = -1; }
        else c1 = a_first_used(AU_SRC);
    }
    double lv = -200;
    if (m < 0.5) {
        double e = emax(CPK[c1], c2 >= 0 ? CPK[c2] : 0);
        if (e > 0) lv = 20 * std::log10(e);
    } else {
        double e = c2 >= 0 ? (CSUM[c1] + CSUM[c2]) / 2 : CSUM[c1];
        e = AU_CN > 0 ? e / AU_CN : 0;
        if (e > 0) lv = 10 * std::log10(e);
    }
    if (lv < -70) lv = -200;
    return lv;
}

// Reference: the reference's output level over all its used channels
double Act::a_level_ref() const
{
    double m = H.pv(P_MEAS);
    int cn = a_nch(AU_SRC);
    double lv = -200, e = 0;
    int k = 0;
    for (int c = 0; c < cn; ++c) {
        if (a_used(AU_SRC, c)) {
            if (m < 0.5) e = emax(e, CPK[c]);
            else { e += CSUM[c]; k++; }
        }
    }
    if (m < 0.5) {
        if (e > 0) lv = 20 * std::log10(e);
    } else {
        e = (k > 0 && AU_CN > 0) ? e / ((double)k * AU_CN) : 0;
        if (e > 0) lv = 10 * std::log10(e);
    }
    if (lv < -70) lv = -200;
    return lv;
}

void Act::a_auto_finish()
{
    // the last, partial 10 ms bin
    if (AMODE == 0 && AU_BPOS > 0 && AU_NB < MAXB) {
        for (int c = 0; c < 8; ++c) MB[c * MAXB + AU_NB] = MBACC[c] / AU_BPOS;
        AU_NB += 1;
    }
    int ng = a_groups(-1, AGA, AGN);
    double tgt = H.pv(P_TGT);
    bool any = false;
    int lim = 0;
    double aim = -200;
    for (int c = 0; c < 8; ++c) ARL[c] = -200;
    for (int g = 0; g < ng; ++g) {
        int a = AGA[g], n = AGN[g];
        double lvl, newg;
        if (AMODE == 0) {
            lvl = a_level_auto(a, n);
            newg = tgt - lvl;
        } else {
            lvl = a_level_in(a, n);
            double cl = (AMODE != 0 && iround(H.pv(P_MSRC + a)) == 9) ? a_level_ref() : a_level_src(a, n);
            if (cl <= -150) lvl = -200;
            newg = cl - lvl;
            // what this group now aims for, in the units the box shows (LUFS: BS.1770 sums the channels)
            if (lvl > -150 && aim <= -150) aim = cl + (H.pv(P_MEAS) >= 1.5 ? 10 * std::log10((double)n) - 0.691 : 0);
        }
        if (lvl > -150) {
            newg = std::floor(newg * 1000 + 0.5) / 1000;
            if (newg < -99.9) { newg = -99.9; lim = 1; }
            if (newg > 60) { newg = 60; lim = 1; }
            a_apply(P_GAIN + a, newg);
            any = true;
        }
        ARL[a] = lvl;
        if (n == 2) ARL[a + 1] = lvl;
    }
    AU_LIMIT = lim;
    if (AMODE != 0 && any) { a_setsl(P_AIM, aim); AU_LMODE = iround(H.pv(P_AMODE)); }
    a_auto_end(any ? (AMODE == 0 ? R_AUTO : (AMODE == 1 ? R_MATCH : R_REF)) : R_NOSIG);
}

void Act::a_auto_block()
{
    if (AST == AS_QUEUED) {
        // give every point of the run time to receive it, then wait until the earlier ones (and, in
        // Reference mode, the reference) are done and their new gain has settled for 0.3 s
        AU_QT += samplesblock / srate;
        if (a_preds_busy() || (AMODE == 2 && a_ref_busy()) || tgs > 0) { AU_SEEN = true; AU_SETTLE = 0; }
        else AU_SETTLE += samplesblock / srate;
        // Auto all: a Set asked for with it starts now, and the output waits for it
        if (AU_QT >= 0.1 && (!AU_SEEN || AU_SETTLE >= 0.3)) {
            AST = AS_WAIT;
            if (tgpend) { tgpend = false; if (tgs == 0) H.hkTgStart(); }
        }
    }
    Slot *sb = nullptr;
    if (AMODE >= 1 && AST >= AS_WAIT) {
        // the previous point (Match) or the reference must still be there
        AU_SRC = a_find_id(AU_SRCID, AU_SRC);
        if (AU_SRC < 0 || !a_live(AU_SRC, a_now)) {
            a_auto_end(AMODE == 1 ? R_PREVGONE : R_REFGONE);
        } else {
            sb = &slot(AU_SRC);
            if (AU_KIN) st(sb->kreq, a_now);
        }
    }
    if (AST == AS_WAIT) {
        bool sig = false;
        if (AMODE == 0) {
            for (int c = 0; c < 8; ++c)
                if (usedA[c] && IBP[c] >= 0.00316) sig = true;
        } else if (sb) {
            if (!AU_KIN || li(sb->kact) >= 1) {
                int cn = a_nch(AU_SRC);
                for (int c = 0; c < cn; ++c)
                    if (ld(sb->bpk[c]) >= 0.00316) sig = true;
            }
            // Reference: this instance needs signal too (the reference may be on another track)
            if (AMODE == 2) {
                bool ow = false;
                for (int c = 0; c < 8; ++c)
                    if (usedA[c] && IBP[c] >= 0.00316) ow = true;
                sig = sig && ow;
            }
        }
        if (sig && tgs == 0) {
            AST = AS_MEAS;
            AU_CSEQ = AMODE >= 1 ? li(sb->blkseq) : -1;
        }
    } else if (AST == AS_MEAS) {
        if (AMODE == 2 && sb) {
            // the reference's blocks and this instance's blocks are summed separately over the same
            // stretch of time (another track's blocks don't always line up with this one's)
            int64_t seq = li(sb->blkseq);
            if (seq != AU_CSEQ) {
                bool jump = seq - AU_CSEQ > 1;
                AU_CSEQ = seq;
                int cn = a_nch(AU_SRC);
                for (int c = 0; c < cn; ++c) {
                    CSUM[c] += AU_KIN ? ld(sb->bke[c]) : ld(sb->ben[c]);
                    if (ld(sb->bpk[c]) > CPK[c]) CPK[c] = ld(sb->bpk[c]);
                    // blocks were missed: the reference's decaying peak meter still holds their peak
                    if (jump) CPK[c] = emax(CPK[c], epow(10, ld(sb->pkd[c]) / 20));
                }
                AU_CN += li(sb->blklen);
            }
            for (int c = 0; c < 8; ++c)
                if (usedA[c]) { MSUM[c] += IBE[c]; if (IBP[c] > MPK[c]) MPK[c] = IBP[c]; }
            AU_MN += a_prevblk;
            AU_T = AU_MN / srate;
        } else if (AMODE == 1 && sb) {
            // the previous point's last block and this point's last block line up
            int64_t seq = li(sb->blkseq);
            if (seq != AU_CSEQ) {
                AU_CSEQ = seq;
                int cn = a_nch(AU_SRC);
                for (int c = 0; c < cn; ++c) {
                    CSUM[c] += AU_KIN ? ld(sb->bke[c]) : ld(sb->ben[c]);
                    if (ld(sb->bpk[c]) > CPK[c]) CPK[c] = ld(sb->bpk[c]);
                }
                AU_CN += li(sb->blklen);
                for (int c = 0; c < 8; ++c)
                    if (usedA[c]) { MSUM[c] += IBE[c]; if (IBP[c] > MPK[c]) MPK[c] = IBP[c]; }
                AU_MN += a_prevblk;
            }
            AU_T = AU_MN / srate;
        } else {
            AU_T = AU_NS / srate;
        }
        if ((AU_DUR > 0 && AU_T >= AU_DUR) || AU_NB >= MAXB || AU_T >= 60) a_auto_finish();
    }
    if (a_inm)
        for (int c = 0; c < 8; ++c) { IBP[c] = 0; IBE[c] = 0; }
}

void Act::a_cmd(int cmd, double x, double y, double z)
{
    // the Act's parameters; a plug-in's Set row settings through its hook
    if (cmd == C_SET) {
        int p = iround(x);
        if (x >= 1 && x <= NPUB && p != P_ID) a_apply(p, y);
        else if (H.hkRset(p) > 0) a_apply(H.hkRset(p), y);
    }
    if (cmd == C_ADDGAIN) a_addgain(iround(x), y, iround(z));
    if (cmd == C_AUTO) a_auto_start(iround(x), (int64_t)y);
    // x == 1: from Auto all, start it when the queued Auto starts; otherwise now
    if (cmd == C_TGSTART) {
        if (eq(x, 1)) { if (AST == AS_QUEUED) tgpend = true; }
        else if (tgs == 0) H.hkTgStart();
    }
    if (cmd == C_TGSTOP && tgs > 0) tgs = 0;
    if ((cmd == C_STOP || cmd == C_CANCEL) && tgs > 0) tgs = 0;
    if (cmd == C_STOP) {
        if (AST == AS_MEAS) {
            if (AU_T > 0.05) a_auto_finish();
            else a_auto_end(R_CANCEL);
        } else if (AST != AS_IDLE) a_auto_end(R_CANCEL);
    }
    if (cmd == C_CANCEL && AST != AS_IDLE) a_auto_end(R_CANCEL);
    if (cmd == C_RESETHOLD) a_reset_hold();
}

// commands from the Gain Stage, the windows and REAPER-side helpers
void Act::a_ring()
{
    Slot &b = slot(myslot);
    int64_t w = b.ringW.load(std::memory_order_acquire), r = li(b.ringR);
    if (w - r > RINGN) r = w - RINGN;
    if (w < r) r = w;
    while (r < w) {
        CmdEntry &e = b.ring[r % RINGN];
        a_cmd(iround(ld(e.c)), ld(e.x), ld(e.y), ld(e.z));
        r++;
    }
    b.ringR.store(r, std::memory_order_release);
}

void Act::a_gains()
{
    int n = a_nch(-1);
    for (int c = 0; c < 8; ++c) {
        bool u = c < n && H.pv(P_USED + c) >= 0.5;
        double t = emax(GFLOOR, H.pv(P_GAIN + c));
        if (u) {
            if (!usedA[c] || !a_inited) {
                CUR[c] = t; RTGT[c] = t; RAMP[c] = false; STEP[c] = 0;
                GLIN[c] = t <= GFLOOR ? 0 : epow(10, t / 20);
            } else if (t != RTGT[c]) {
                RTGT[c] = t;
                STEP[c] = (t - CUR[c]) / a_glide;
                RAMP[c] = true;
            }
        } else if (usedA[c]) {
            PKD[c] = -200; PKH[c] = -200; RMH[c] = -200; RMSMS[c] = 0; CLIPF[c] = 0;
        }
        usedA[c] = u;
    }
    a_inited = true;
}

// output meters from the last block, published with the block's raw numbers for Match
void Act::a_meters()
{
    Slot &b = slot(myslot);
    double dur = a_prevblk / srate;
    double holds = act_hold_sec(H.pv(P_HOLD));
    double k = 1 - std::exp(-dur / 0.3);
    // channels past the channel count only need clearing once
    int nm = std::max(nchs, AU_MCLR);
    AU_MCLR = nchs;
    for (int c = 0; c < nm; ++c) {
        if (usedA[c]) {
            double pk = BPK[c], pkdb = pk > 0.0000000001 ? 20 * std::log10(pk) : -200;
            PKD[c] = emax(pkdb, PKD[c] - 20 * dur);
            if (PKD[c] < -200) PKD[c] = -200;
            if (pk >= 1) CLIPF[c] = 1;
            if (holds == 0) PKH[c] = PKD[c];
            else if (pkdb >= PKH[c]) { PKH[c] = pkdb; PKHT[c] = 0; }
            else {
                PKHT[c] += dur;
                if (holds > 0 && PKHT[c] > holds) { PKH[c] = PKD[c]; PKHT[c] = 0; }
            }
            double ms = a_prevblk > 0 ? BEN[c] / a_prevblk : 0;
            RMSMS[c] += k * (ms - RMSMS[c]);
            double rdb = RMSMS[c] > 0.0000000001 ? 10 * std::log10(RMSMS[c]) : -200;
            if (holds == 0) RMH[c] = rdb;
            else if (rdb >= RMH[c]) { RMH[c] = rdb; RMHT[c] = 0; }
            else {
                RMHT[c] += dur;
                if (holds > 0 && RMHT[c] > holds) { RMH[c] = rdb; RMHT[c] = 0; }
            }
            st(b.pkd[c], PKD[c]); st(b.pkh[c], PKH[c]); st(b.rms[c], rdb); st(b.rmh[c], RMH[c]); st(b.clip[c], CLIPF[c]);
            st(b.bpk[c], pk); st(b.ben[c], BEN[c]); st(b.bke[c], BKE[c]);
        } else {
            st(b.pkd[c], -200); st(b.pkh[c], -200); st(b.rms[c], -200); st(b.rmh[c], -200); st(b.clip[c], 0);
            st(b.bpk[c], 0); st(b.ben[c], 0); st(b.bke[c], 0);
        }
        BPK[c] = 0; BEN[c] = 0; BKE[c] = 0;
    }
    si(b.blklen, a_prevblk);
    b.blkseq.fetch_add(1, std::memory_order_release);
}

// can this plug-in key from the one in slot s: same track and chain (not the record input chain, not take FX),
// earlier in it, and neither inside a container (the JSFX's a_key_ok)
bool Act::keyOk(int s) const
{
    if (s < 0 || s == myslot) return false;
    const Slot &b = slot(s);
    if (li(b.owner) < 100000 || !a_live(s, a_now)) return false;
    return li(b.track) == a_trk && (li(b.pflags) & 7) == (a_pf & 7) && ld(b.order) < a_order && !(li(b.pflags) & PL_CONTAINER) &&
           !(a_pf & PL_CONTAINER);
}

// ---------------------------------------------------------------- audio key, writer side
// Writes this plug-in's output into a lane for a later plug-in on the same track (a TwoStorms Gate
// keying from it), only while one is asking (it counts areq up every block it runs).
void Act::a_aud_block()
{
    a_wr = false;
    if (myslot < 0) return;
    Shared *S = get();
    Slot &b = slot(myslot);
    int ln = myslot % AUD_LANES;
    AudioLane &L = S->lane[ln];
    int64_t ask = li(b.areq);
    if (ask != a_wrask) { a_wrask = ask; if (ask > 0) a_wrat = a_now; }
    bool want = (a_now - a_wrat < 0.5) && samplesblock <= AUD_R;
    if (want) {
        int64_t o = li(L.slot1);
        if (o == 0 || (o == myslot + 1 && li(L.ownerid) == myid) || a_now - ld(L.lastw) > 0.5) {
            si(L.slot1, myslot + 1);
            si(L.ownerid, myid);
            st(L.lastw, a_now);
            si(b.alane, ln + 1);
            si(b.astart, a_wp);
            si(b.alen, samplesblock);
            b.aseq.fetch_add(1, std::memory_order_release);
            a_wrlane = ln;
            a_wr = true;
        } else {
            si(b.alane, -1);
        }
    } else {
        // nobody is asking: give the lane back
        if (li(L.slot1) == myslot + 1 && li(L.ownerid) == myid) si(L.slot1, 0);
        si(b.alane, 0);
    }
}

void Act::audioKey(double *const *out, int nch, int n)
{
    if (a_wr) {
        AudioLane &L = get()->lane[a_wrlane];
        const double *l = out[0], *r = out[1];
        for (int i = 0; i < n; ++i) {
            L.l[a_wp].store((float)l[i], std::memory_order_relaxed);
            L.r[a_wp].store((float)r[i], std::memory_order_relaxed);
            a_wp = (a_wp + 1) & AUD_MASK;
        }
    }
    // a Tuner listening to this plug-in mutes it (it heard the output above)
    double tg = a_tmute ? 0 : 1;
    if (a_tmg == 1 && tg == 1) return;
    double step = 1 / std::max(1.0, 0.01 * srate);
    double g = a_tmg;
    for (int i = 0; i < n; ++i) {
        g = g < tg ? std::min(tg, g + step) : std::max(tg, g - step);
        for (int c = 0; c < nch; ++c) out[c][i] *= g;
    }
    a_tmg = g;
}

void Act::a_publish()
{
    Slot &b = slot(myslot);
    st(b.heart, a_now);
    b.hbc.fetch_add(1, std::memory_order_relaxed);
    st(b.srate, srate);
    si(b.kind, kind);
    si(b.subkind, subkind);
    si(b.track, a_trk);
    si(b.trackno, plTrackNo.load(std::memory_order_relaxed));
    si(b.pflags, a_pf);
    st(b.order, a_order);
    si(b.astate, AST);
    si(b.amode, AMODE);
    si(b.runid, ARUN);
    st(b.donet, AU_DONET);
    si(b.result, AU_RESULT);
    si(b.resseq, AU_RESSEQ);
    si(b.limit, AU_LIMIT);
    st(b.atime, AU_T);
    st(b.adur, AU_DUR);
    si(b.kact, a_kout_was ? 1 : 0);
    si(b.on, kind == KIND_MASTER ? (H.pv(P_GS_ON) >= 0.5 ? 1 : 0) : 1);
    // the Gain Stage's Set row
    st(b.tgs, tgs);
    st(b.tgel, tgel);
    st(b.tgav, tgav);
    H.hkPublish(b);
    // parameters and results every 4th block (plenty for the windows and the reference copies)
    if ((AU_PUBN += 1) >= 4) {
        AU_PUBN = 0;
        for (int p = 1; p <= NPUB; ++p) st(b.p[p], H.pv(p));
        for (int c = 0; c < 8; ++c) { st(b.rlvl[c], ARL[c]); st(b.drvs[c], DRVS[c]); }
    }
}

// K-weighting filter, state in st[0..3]
double Act::a_kw(double x, double *s) const
{
    double y1 = KB0 * x + s[0];
    s[0] = KB1 * x - KA1 * y1 + s[1];
    s[1] = KB2 * x - KA2 * y1;
    double y2 = y1 + s[2];
    s[2] = -2 * y1 - KC1 * y2 + s[3];
    s[3] = y1 - KC2 * y2;
    return y2;
}

// ---------------------------------------------------------------- @block (the Act's part)
void Act::block(int nframes)
{
    samplesblock = nframes;
    a_now = now_sec();
    a_claim();
    if (myslot >= 0) a_dupcheck();
    // placement from the main thread; unknown = a chain of its own
    a_pf = plFlags.load(std::memory_order_relaxed);
    int64_t tk = plTrack.load(std::memory_order_relaxed);
    a_trk = tk != 0 ? tk : -(myid > 0 ? myid : 1);
    if (myslot >= 0) {
        a_order = plOrder.load(std::memory_order_relaxed);
        a_ring();
        a_aim_check();
        a_drive_follow();
    }
    a_gains();
    nchs = a_nch(-1);
    a_auto_block();
    a_kout_was = a_kout;
    if (myslot >= 0) {
        a_meters();
        // the next point is matching loudness against this one: K-weight the output too
        a_kout = a_now - ld(slot(myslot).kreq) < 0.5;
        if (a_kout && !a_kout_was) memset(KOUT, 0, sizeof KOUT);
        a_publish();
    } else {
        a_kout = false;
        for (int c = 0; c < 8; ++c) { BPK[c] = 0; BEN[c] = 0; BKE[c] = 0; }
    }
    a_aud_block();
    a_tmute = myslot >= 0 && a_now - ld(slot(myslot).tmute) < 0.25;
    a_inm = AST >= AS_WAIT;
    a_binon = AST == AS_MEAS && AMODE == 0;
    a_kin = a_inm && AU_KIN;
    a_prevblk = nframes;
    if (kind == KIND_MASTER) {
        // ON/OFF: a linear fade over the glide time
        a_ont = H.pv(P_GS_ON) >= 0.5 ? 1 : 0;
        if (a_onv < 0) a_onv = a_ont;
        a_onrate = 1 / std::max(1.0, H.pv(P_GS_GLIDE) * srate / 1000);
        a_mfast = nchs == 2 && usedA[0] && usedA[1] && !RAMP[0] && !RAMP[1] && !a_inm && !a_kout && a_onv == a_ont;
    }
    slotPub.store(myslot, std::memory_order_relaxed);
    idPub.store(myid, std::memory_order_relaxed);
}

// ---------------------------------------------------------------- @sample (the Act's part)
// One channel's gain for one sample, with the measuring Auto needs (a_inm). Same order as the JSFX.
inline double Act::gainSample(int c, double a_x, double onv)
{
    if (a_inm) {
        double a_ax = std::fabs(a_x);
        if (a_ax > IBP[c]) IBP[c] = a_ax;
        double a_e = a_kin ? sqr(a_kw(a_x, KIN + c * 4)) : a_x * a_x;
        IBE[c] += a_e;
        if (a_binon) {
            MBACC[c] += a_e;
            if (a_ax > MPK[c]) MPK[c] = a_ax;
        }
    }
    if (RAMP[c]) {
        CUR[c] += STEP[c];
        if ((STEP[c] > 0 && CUR[c] >= RTGT[c]) || (STEP[c] < 0 && CUR[c] <= RTGT[c]) || STEP[c] == 0) {
            CUR[c] = RTGT[c];
            RAMP[c] = false;
        }
        GLIN[c] = CUR[c] <= GFLOOR ? 0 : epow(10, CUR[c] / 20);
    }
    double a_y = a_x * GLIN[c] * onv;   // onv: 1 except in the Gain Stage (exact either way)
    double a_ay = std::fabs(a_y);
    if (a_ay > BPK[c]) BPK[c] = a_ay;
    BEN[c] += a_y * a_y;
    if (a_kout) BKE[c] += sqr(a_kw(a_y, KOUT + c * 4));
    return a_y;
}

void Act::process(double *const *ch, int nchAvail, int nframes)
{
    int nc = std::min(nchs, nchAvail);
    // the Gain Stage: its ON/OFF factor for each sample (others: none, 1)
    const double *ov = nullptr;
    if (kind == KIND_MASTER) {
        if ((int)onvBuf.size() < nframes) onvBuf.resize(nframes);
        for (int i = 0; i < nframes; ++i) {
            if (a_onv != a_ont) a_onv = a_onv < a_ont ? std::min(a_ont, a_onv + a_onrate) : std::max(a_ont, a_onv - a_onrate);
            onvBuf[i] = a_onv;
        }
        ov = onvBuf.data();
    }
    if (!a_binon) {
        // channels are independent: one channel at a time
        for (int c = 0; c < nc; ++c) {
            double *x = ch[c];
            if (!usedA[c]) {
                if (ov) for (int i = 0; i < nframes; ++i) x[i] *= ov[i];
                continue;
            }
            if (ov && a_mfast) {
                // the Gain Stage's fast path: the gain and the (steady) ON/OFF in one factor
                double g = GLIN[c] * ov[0], bpk = BPK[c], ben = BEN[c];
                for (int i = 0; i < nframes; ++i) {
                    double a_y = x[i] * g;
                    x[i] = a_y;
                    double a_ay = std::fabs(a_y);
                    if (a_ay > bpk) bpk = a_ay;
                    ben += a_y * a_y;
                }
                BPK[c] = bpk;
                BEN[c] = ben;
            } else if (ov) {
                for (int i = 0; i < nframes; ++i) x[i] = gainSample(c, x[i], ov[i]);
            } else if (!a_inm && !a_kout && !RAMP[c]) {
                // the usual case: a steady gain, nothing measuring
                double g = GLIN[c], bpk = BPK[c], ben = BEN[c];
                for (int i = 0; i < nframes; ++i) {
                    double a_y = x[i] * g;
                    x[i] = a_y;
                    double a_ay = std::fabs(a_y);
                    if (a_ay > bpk) bpk = a_ay;
                    ben += a_y * a_y;
                }
                BPK[c] = bpk;
                BEN[c] = ben;
            } else {
                for (int i = 0; i < nframes; ++i) x[i] = gainSample(c, x[i], 1.0);
            }
        }
        return;
    }
    // Auto Target is measuring: 10 ms bins across every channel, sample by sample
    for (int i = 0; i < nframes; ++i) {
        double o = ov ? ov[i] : 1.0;
        for (int c = 0; c < nc; ++c) {
            if (usedA[c]) ch[c][i] = gainSample(c, ch[c][i], o);
            else if (ov) ch[c][i] *= o;
        }
        AU_NS += 1;
        AU_BPOS += 1;
        if (AU_BPOS >= a_lbinlen) {
            if (AU_NB < MAXB) {
                for (int c = 0; c < 8; ++c) { MB[c * MAXB + AU_NB] = MBACC[c] / AU_BPOS; MBACC[c] = 0; }
                AU_NB += 1;
            }
            AU_BPOS = 0;
        }
    }
}

// @slider: a host change (automation, a preset) to one side of a linked pair moves the other side too
void Act::sliderSync()
{
    int n = a_nch(-1);
    for (int k = 0; k < 4; ++k) {
        int a = k * 2, b = a + 1;
        if (H.pv(P_LINK + k) >= 0.5 && b < n && H.pv(P_USED + a) >= 0.5 && H.pv(P_USED + b) >= 0.5) {
            double ga = H.pv(P_GAIN + a), gb = H.pv(P_GAIN + b);
            if (ga != gb) {
                if (ga != LASTG[a]) H.putQuiet(P_GAIN + b, ga);
                else H.putQuiet(P_GAIN + a, gb);
            }
        }
    }
    for (int c = 0; c < 8; ++c) LASTG[c] = H.pv(P_GAIN + c);
}

} // namespace ts
