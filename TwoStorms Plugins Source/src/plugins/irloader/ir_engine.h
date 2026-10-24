// TwoStorms IR Loader's sound: two impulse responses, blended. Each with on, level, pan, delay, phase, stereo width,
// high and low pass (12 / 24 / 48 dB/oct) and solo; with two, their starts lined up, IR 2's timing, polarity and
// loudness matched to IR 1. Normalized (on by default): each as loud as the input, two blended rather than added.
// Tail trim, stereo IRs or summed, stereo or mono input, dry/wet. True zero latency (a direct head and non-uniformly
// partitioned FFT convolution) or 64 / 128 / 256 samples of latency for less CPU; changes crossfade. IRs at any sample
// rate are resampled to the project's (windowed sinc, Kaiser). Ported line for line from twostorms_ir_loader.jsfx (the
// filter is built a little per block, as there; normalizing is the CLAP's own).
#pragma once
#include "../../core/abmatch.h"
#include "../../core/act.h"
#include "../../core/plugin.h"
#include <atomic>
#include <string>
#include <vector>

namespace ts {
namespace irl {

constexpr int IRB = 50, IRK = 20;
enum { K_ON = 0, K_LVL, K_PAN, K_DLY, K_PHS, K_HPON, K_HP, K_LPON, K_LP, K_HPS, K_LPS, K_WID, K_SOLO };
enum { P_LAT = 90, P_INM = 91, P_STIR = 92, P_ALVL = 93, P_MIX = 94, P_ALIGN = 95, P_MATCH = 96, P_TTON = 97, P_TTDB = 98,
       P_SMOOTH = 99 /* the window's response smoothing */, P_NORM = 100 };
inline int irp(int s, int k) { return IRB + s * IRK + k; }
constexpr int MAXR = 262144;   // an IR file: up to this many frames per channel are kept (and saved with the project)
constexpr int MAXT = 131072;   // an IR at the project rate: up to this many samples (2.7 s at 48 kHz)
constexpr int MAXG = 131072;   // the filter (both IRs, their delays and the latency): up to this many taps
int latencyOf(double v);       // P_LAT -> samples

// an IR as loaded: its own sample rate and channels (one or two)
struct IrData {
    int frames = 0, chans = 1;
    double sr = 48000;
    std::vector<double> d[2];
};

// what the window shows, copied out of the engine after each build (see Engine::snapshot)
struct IrShow {
    uint32_t seq = 0;
    bool used[2] = {}, two = false, match = false, alvl = false, norm = false;
    int irl[2] = {}, irst[2] = {}, irls[2] = {}, irpc[2] = {};
    double irg[2] = {}, alag = 0, apol = 1, alvg = 0, srate = 48000;
    double nrm[2] = {1, 1}, nrk = 1;   // normalizing: each IR's gain, and both together's
    std::vector<double> irs[2][2];   // each IR at the project rate (the waveform)
    std::vector<double> spr[2][2];   // ... trimmed, faded, filtered (its response, times irg)
    std::vector<double> gl, gr;      // the filter as it plays (both together)
};

class EngineHost {
public:
    virtual ~EngineHost() {}
    virtual double pv(int id) const = 0;
};

class Engine {
public:
    Engine(EngineHost &h, Act &a, UiLink &u) : H(h), A(a), U(u)
    {
        for (int s = 0; s < 2; ++s) inbox[s].store(nullptr);
    }
    ~Engine();
    AbMatch ab;
    double budgetMul = 8;   // the build's pace (the JSFX: 1)

    void init(double sr);
    void reset();
    void block(int samplesblock);
    void afterAct();
    void samples(const double *const *in, double *const *out, int n);
    void post(double *const *out, int n);
    int latency() const { return m_T < 0 ? 0 : m_T; }

    // main thread: hand an IR to slot s (nullptr or 0 frames: clear it); the engine takes it at its next block
    void give(int s, IrData *d);
    // main thread: free what the engine is done with
    void collect();
    // main thread: set an IR before processing starts (tests: the JSFX loads in @slider, before the first block)
    void giveNow(int s, IrData *d);
    // the window: copy what it shows (true when a new, complete build was copied)
    bool snapshot(IrShow &v) const;
    bool building() const { return (seq.load(std::memory_order_acquire) & 1u) != 0; }
    std::atomic<double> mi0{-200}, mi1{-200};

private:
    static constexpr int NMAX = 8192, RN = 16384, RO = 16384;
    EngineHost &H;
    Act &A;
    UiLink &U;
    double srate = 44100;
    bool nonceDrawn = false, idDrawn = false;
    std::atomic<IrData *> inbox[2];
    IrData *cur[2] = {};
    Spsc<IrData *, 16> retq;   // IRs the engine is done with, for the main thread to free
    IrData *ret[16] = {};
    int nret = 0;
    void giveBack(IrData *p) { if (!retq.push(p) && nret < 16) ret[nret++] = p; }
    std::atomic<uint32_t> seq{0};
    std::vector<double> br0, br1, bq0, bq1;

    int IRF(int s) const { return cur[s] ? cur[s]->frames : 0; }
    int IRC(int s) const { return cur[s] ? cur[s]->chans : 1; }
    double IRSR(int s) const { return cur[s] ? cur[s]->sr : 48000; }
    double pv(int id) const { return H.pv(id); }

    // buffers
    std::vector<double> KT, IRS, SPR, GLB, GRB, RING, RRING, DRY, OUC, SWb, TMPb, POOL, ACCB, ALX, ALC;
    // loudness: each IR's spectrum (both channels, the bins the measure uses), the measure's weights, a unit impulse's
    std::vector<double> LDS, LDW;
    double LDREF = 0, NRM[2] = {1, 1}, NRK = 1, IRLK[2] = {};
    double *GL = nullptr, *GR = nullptr;
    double FDK[16] = {};
    int IRL[2] = {}, IRST[2] = {}, IRLS[2] = {}, IRPC[2] = {}, RSN_DONE[2] = {}, IRCH[2] = {};
    double IRG[2] = {}, IRLD[2] = {}, BKG[2] = {}, BKD[2] = {};
    bool RSD[2] = {}, BKU[2] = {};
    // the engine's stages and banks
    int SN[8] = {}, SS[8] = {}, SPM[8] = {}, SRUN[8] = {}, SFP[8] = {}, SQ[8] = {}, SNX[8] = {}, SPA[8] = {}, SSPL[8] = {};
    int SFZ[8] = {}, SFR[8] = {};
    double *FDZ[128] = {}, *FDS[128] = {}, *FDR[128] = {}, *HPC[256] = {}, *ACP[16] = {};
    int BPASS[2] = {}, BPN[16] = {}, BST[16] = {}, BLN[2] = {}, BHN[2] = {}, BSAME[2] = {}, BMODE[2] = {};
    double HDL[64] = {}, HDR[64] = {}, HX[64] = {}, HY[64] = {};
    int hp = 0;
    int m_T = -1, m_bc = -1, m_bn = -1, m_cnt = 0, m_rp = 0, m_dp = 0, m_nst = 0, m_hd = 32, m_n0 = 32, m_bkn = 0;
    double m_srw = 0, m_fu = 0, m_fdu = 0;
    bool m_fading = false, m_fdone = false, m_dirty = true, m_inmono = false;
    double *m_qp = nullptr, *m_rq = nullptr;
    // the build
    int bk_ph = 0, bk_s = 0, bk_c = 0, bk_pos = 0, bk_sub = 0, bk_fd = 0, bk_len = 0, bk_di = 0, bk_base = 0, bk_ls = 0;
    bool bk_mono = false, bk_two = false, bk_align = false, bk_match = false, bk_alvl = false, bk_pass = false, bk_same = false;
    bool bk_norm = false;
    double bk_bud = 0, bk_fr = 0, bk_gl = 1, bk_gr = 1;
    double rs_ratio = 1, rs_c = 1, rs_w = 16, rs_s = 256;
    int rs_n = 0;
    bool rs_copy = true;
    double bq_b0 = 0, bq_b1 = 0, bq_b2 = 0, bq_a1 = 0, bq_a2 = 0, bq_s1 = 0, bq_s2 = 0;
    double ALAG = 0, APOL = 1, ALVG = 0;
    double BKV[128];
    // dry/wet, meters
    double c_dm = 0, c_wmt = 1, c_zk = 0, c_dms = 0, c_wms = 1, c_mi0 = -200, c_mi1 = -200, c_ip0 = 0, c_ip1 = 0;
    bool e_first = true;

    void take();
    void layout();
    void engReset();
    void srun();
    void macFull(double *acc, const double *x, const double *h, int m);
    void macHalf(double *acc, const double *x, const double *h, int n);
    void stMac(int b, int i, int p, int q);
    void stSched(int i, int sb);
    void stOut(int b, int i);
    void stBoundary(int i);
    void tick();
    void rsPrep(int s);
    void fdPrep(double fr);
    void bqSet(double f, bool hp, double q);
    void bqRun(double *buf, int n);
    void bkStart();
    int bkFit(double u) const;
    void bkResample();
    void ldMeas(int s);
    void normalize();
    void alMatch();
    void bkSlot();
    void bkSum();
    void bkPart();
    void bkRun(double bud);
};

// reading an IR file (.wav: 16 / 24 / 32-bit PCM, 32 / 64-bit float, mono or stereo; more channels: the first two)
IrData *readWav(const std::string &path, std::string &err);

} // namespace irl
} // namespace ts
