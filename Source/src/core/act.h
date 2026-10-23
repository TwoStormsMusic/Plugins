// The Act every TwoStorms plug-in has built in: output gain (per channel, linked pairs), meters,
// Auto (Target / Match / Reference), Drive link and Finale, all published in shared memory so the
// Gain Stage lists and stages it like an Act. This is the audio-thread side, ported line for line
// from the JSFX suite's shared Act code (a_* functions); act_view.h has the window side (g_*).
#pragma once
#include "params.h"
#include "shm.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace ts {

// ---- the Act's parameters (1..42), the same numbers in every TwoStorms plug-in ----
// The Gain Stage's own (as its JSFX numbers them, p; its slider numbers differ): ON/OFF and its glide
enum GainStageParam { P_GS_GLIDE = 50, P_GS_UIST = 51, P_GS_SETS = 52, P_GS_SETF = 53, P_GS_ON = 54 };

enum ActParam {
    P_NCH = 1, P_FINAL = 2, P_MEAS = 3, P_TGT = 4, P_TIME = 5, P_LINK = 6, P_GAIN = 10, P_USED = 18,
    P_MSRC = 26, P_DRIVE = 34, P_REF = 35, P_ID = 36, P_HOLD = 37, P_RANGE = 38, P_DRIVEON = 39,
    P_AMODE = 40, P_AIM = 41, P_ZOOM = 42,
};
constexpr int NPUB = 41;          // parameters 1..NPUB are published and can be set remotely
constexpr double GFLOOR = -100;   // gain floor = -inf
constexpr int DRIVE_PARAM = 1;    // drive link value meaning "a plug-in parameter"
enum { FIN_AUTO = 0, FIN_ALWAYS = 1, FIN_NEVER = 2 };
constexpr int MAXB = 6000;        // Auto bins: 6000 x 10 ms = 60 s

// Adds parameters 1..42. fixedPair: the Act is one linked stereo pair (plug-ins other than the Act
// itself): the channel options are fixed and only "Output" (P_GAIN) is a host parameter.
void addActParams(ParamTable &t, bool fixedPair);

double act_time_sec(double v);   // P_TIME index -> seconds (0 = manual)
double act_hold_sec(double v);   // P_HOLD index -> seconds (-1 = infinite)
double act_range_db(double v);   // P_RANGE index -> dB
double act_zoom(double v);       // P_ZOOM index -> scale

// What the Act needs from the plug-in it is built into.
class ActHost {
public:
    virtual ~ActHost() {}
    virtual double pv(int id) const = 0;                 // a parameter's value
    virtual void setFromAudio(int id, double v) = 0;     // set a parameter on the audio thread and tell the host
    virtual double clampParam(int id, double v) const = 0;
    // set a value without telling anyone (the @slider pair sync: the host already changed one side)
    virtual void putQuiet(int id, double v) = 0;
    // The plug-in's own Set row in the Gain Stage (the hk_* hooks): which of its parameters the
    // row sets remotely, starting it, and what to publish for it.
    virtual int hkRset(int p) { (void)p; return 0; }
    virtual void hkTgStart() {}
    virtual void hkPublish(shm::Slot &s) { (void)s; }
};

class Act {
public:
    Act(ActHost &h, int kind, int subkind);

    void init(double srate);              // @init (also on a sample-rate change)
    // @block, the Act's part. Runs after the plug-in's own block code.
    void block(int nframes);
    // @sample, the Act's part, over a whole block: the output gain on ch[0..nch) in place, plus meters
    // and Auto's measuring. nchAvail: how many of the buffers exist.
    void process(double *const *ch, int nchAvail, int nframes);
    // @slider: keep linked pairs on one fader after the host changed a gain
    void sliderSync();
    // for DSP that reads other instances (the Gate's key): a slot by ID, whether it runs, and whether it is an
    // earlier plug-in in this chain (same track and chain, not inside a container)
    int findSlot(int64_t id, int hint) const { return a_find_id(id, hint); }
    bool slotLive(int s) const { return s >= 0 && a_live(s, a_now); }
    bool keyOk(int s) const;
    // this instance's gain groups (a_groups(-1, ...)): first channel and 1 or 2 channels each
    int groups(int *ga, int *gn) const { return a_groups(-1, ga, gn); }
    // the very last step of @sample: hand this block's final output (channels 0 and 1) to a later plug-in that asked,
    // then silence all nch channels while a Tuner listening to this one has its Mute on (10 ms fades)
    void audioKey(double *const *out, int nch, int nframes);

    // commands from this instance's own window go through the same ring as remote ones
    bool send(int cmd, double x = 0, double y = 0, double z = 0) { return myslot >= 0 && shm::send(myslot, cmd, x, y, z); }

    // ---- the plug-in's Set row (c_tgs etc.) lives here because the Act's commands drive it ----
    int tgs = 0;            // 0 idle, 1 waiting for signal, 2 listening, 3 working
    double tgel = 0;        // seconds listened
    double tgav = 1;        // 1 while there is something to set
    bool tgpend = false;    // started from Auto all: begin when the queued Auto starts

    // ---- placement, written by the main thread (from REAPER) ----
    std::atomic<int64_t> plTrack{0};    // chain key (0 = unknown: this instance is its own chain)
    std::atomic<int64_t> plTrackNo{0};
    std::atomic<int64_t> plFlags{0};
    std::atomic<double> plOrder{0};
    std::atomic<int64_t> plKnown{0};    // 1 once REAPER told us where we are

    // ---- read by the window ----
    std::atomic<int> slotPub{-1};       // MYSLOT
    std::atomic<int64_t> idPub{0};      // MYID
    double now() const { return a_now; }

    int kind, subkind;
    int64_t mynonce;

    // the audio thread's copy (public for the plug-in's DSP that reads it, like the JSFX globals)
    int myslot = -1;
    int64_t myid = 0;
    int nchs = 2;           // a_nchs
    bool usedA[8] = {};     // USEDA

private:
    ActHost &H;
    double srate = 44100;
    double a_now = 0;
    int a_lbinlen = 480, a_glide = 960;
    bool a_inited = false;
    int a_prevblk = 0;
    int samplesblock = 0;

    // gains
    double LASTG[8] = {}, CUR[8] = {}, RTGT[8] = {}, STEP[8] = {}, GLIN[8] = {};
    bool RAMP[8] = {};
    // meters
    double BPK[8] = {}, BEN[8] = {}, BKE[8] = {};
    double PKD[8], PKH[8], PKHT[8] = {}, RMSMS[8] = {}, RMH[8], RMHT[8] = {}, CLIPF[8] = {};
    double KIN[32] = {}, KOUT[32] = {};
    double IBP[8] = {}, IBE[8] = {}, MPK[8] = {}, MSUM[8] = {}, MBACC[8] = {};
    double CSUM[8] = {}, CPK[8] = {}, ARL[8] = {}, DRVS[8] = {};
    int AGA[8] = {}, AGN[8] = {};
    std::vector<double> MB, ZW;
    std::vector<int64_t> DFID;
    std::vector<double> DFLAST;
    // K-weighting
    double KB0 = 0, KB1 = 0, KB2 = 0, KA1 = 0, KA2 = 0, KC1 = 0, KC2 = 0;
    // slot keeping
    int DUPS = -1;
    int64_t DUPH = 0;
    double DUPT = 0, AU_DUPT = 0, DUPM = 0;
    int AU_PUBN = 0, AU_MCLR = 0;
    // auto
    int AST = shm::AS_IDLE, AMODE = 0;
    int64_t ARUN = 0;
    double AU_DUR = 0;
    bool AU_KIN = false;
    int AU_NB = 0, AU_BPOS = 0;
    int64_t AU_NS = 0, AU_MN = 0, AU_CN = 0;
    double AU_T = 0, AU_QT = 0, AU_SETTLE = 0;
    bool AU_SEEN = false;
    int64_t AU_CSEQ = -1;
    int AU_LIMIT = 0;
    int AU_SRC = -1;
    int64_t AU_SRCID = 0;
    int AU_RESULT = 0;
    int64_t AU_RESSEQ = 0;
    double AU_DONET = 0;
    int AU_LMODE = -1, AU_LMEAS = -1;
    double AU_LREF = -1;
    bool a_inm = false, a_binon = false, a_kin = false, a_kout = false, a_kout_was = false;
    // placement this block
    int64_t a_trk = 0, a_pf = 0;
    double a_order = 0;
    // audio key
    bool a_wr = false;
    int a_wrlane = 0;
    int a_wp = 0;
    int64_t a_wrask = 0;
    double a_wrat = -100;
    // the Tuner's mute
    bool a_tmute = false;
    double a_tmg = 1;

    // a_* functions
    double a_pv(int s, int p) const;
    int a_nch(int s) const;
    bool a_used(int s, int c) const;
    bool a_linked(int s, int k) const;
    int a_groups(int s, int *ga, int *gn) const;
    int a_find_id(int64_t id, int hint) const;
    bool a_live(int s, double now) const;
    void a_setsl(int p, double v);
    void a_apply(int p, double v);
    void a_new_id();
    void a_claim();
    void a_dupcheck();
    void a_reset_hold();
    int a_first_used(int s) const;
    void a_addgain(int gi, double delta, int sng);
    void a_drive_follow();
    void a_auto_end(int res);
    int a_prev_slot() const;
    int a_ref_check(int s) const;
    bool a_ref_busy() const;
    bool a_preds_busy() const;
    void a_auto_start(int mode, int64_t run);
    void a_aim_check();
    double a_level_auto(int a, int n);
    double a_level_in(int a, int n) const;
    double a_level_src(int a, int n) const;
    double a_level_ref() const;
    void a_auto_finish();
    void a_auto_block();
    void a_cmd(int cmd, double x, double y, double z);
    void a_ring();
    void a_gains();
    void a_meters();
    void a_aud_block();
    void a_publish();
    double a_kw(double x, double *st) const;
    inline double gainSample(int c, double x, double onv);
    // the Gain Stage's ON/OFF: a linear fade over the glide time (a_onv, a_ont, a_onrate)
    double a_onv = -1, a_ont = 1, a_onrate = 1;
    bool a_mfast = false;
    std::vector<double> onvBuf;
    bool sameChain(int s) const;
};

} // namespace ts
