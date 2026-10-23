#include "ir_engine.h"
#include "../../core/eelrand.h"
#include "../../core/fft.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ts {
namespace irl {

int latencyOf(double v)
{
    int t = (int)std::floor(v + 0.5);
    return t <= 0 ? 0 : (t == 1 ? 64 : (t == 2 ? 128 : 256));
}

// Butterworth slopes as biquads in a row: 12 dB/oct one, 24 two, 48 four (their Qs, from QT[QS0[slope]])
static const double QT[7] = {0.7071067811865476, 0.5411961001461970, 1.3065629648763766, 0.5097955791041592,
                             0.6013448869350453, 0.8999762231364156, 2.5629154477415055};
static const int QS0[3] = {0, 1, 3}, QSN[3] = {1, 2, 4};

Engine::~Engine()
{
    for (int s = 0; s < 2; ++s) {
        delete cur[s];
        delete inbox[s].exchange(nullptr);
    }
    IrData *p;
    while (retq.pop(p)) delete p;
    for (int i = 0; i < nret; ++i) delete ret[i];
}

// ======================================================================== IRs in and out (the main thread's side)
void Engine::give(int s, IrData *d)
{
    if (!d) d = new IrData();
    delete inbox[s].exchange(d);   // one not taken yet is replaced
}

void Engine::giveNow(int s, IrData *d)
{
    delete cur[s];
    cur[s] = d && d->frames > 0 ? d : nullptr;
    if (d && !cur[s]) delete d;
    RSD[s] = cur[s] != nullptr;
    m_dirty = true;
}

void Engine::collect()
{
    IrData *p;
    while (retq.pop(p)) delete p;
}

// the audio side: an IR handed over replaces the slot's (not while one is being resampled: that reads them)
void Engine::take()
{
    while (nret > 0 && retq.push(ret[nret - 1])) nret--;
    if (bk_ph == 1) return;
    for (int s = 0; s < 2; ++s) {
        IrData *n = inbox[s].exchange(nullptr);
        if (!n) continue;
        IrData *old = cur[s];
        cur[s] = n->frames > 0 ? n : nullptr;
        if (!cur[s]) giveBack(n);
        if (old) giveBack(old);
        RSD[s] = cur[s] != nullptr;
        m_dirty = true;
        seq.store(seq.load(std::memory_order_relaxed) | 1u, std::memory_order_release);
    }
}

// the window's copy of what it shows: only between builds (seq even), and only if no build started while copying
bool Engine::snapshot(IrShow &v) const
{
    uint32_t s1 = seq.load(std::memory_order_acquire);
    if ((s1 & 1u) || s1 == v.seq) return false;
    IrShow t;
    t.seq = s1;
    t.srate = srate;
    for (int s = 0; s < 2; ++s) {
        t.used[s] = BKU[s] && IRL[s] > 0;
        t.irl[s] = IRL[s]; t.irst[s] = IRST[s]; t.irls[s] = IRLS[s]; t.irpc[s] = IRPC[s]; t.irg[s] = IRG[s];
        int nc = IRCH[s];
        for (int c = 0; c < 2; ++c) {
            if (c < nc && IRL[s] > 0) t.irs[s][c].assign(IRS.begin() + (s * 2 + c) * MAXT, IRS.begin() + (s * 2 + c) * MAXT + IRL[s]);
            if (t.used[s] && c < IRPC[s]) t.spr[s][c].assign(SPR.begin() + (s * 2 + c) * MAXT, SPR.begin() + (s * 2 + c) * MAXT + IRLS[s]);
        }
    }
    t.two = bk_two; t.match = bk_match; t.alvl = bk_alvl;
    t.alag = ALAG; t.apol = APOL; t.alvg = ALVG;
    t.norm = bk_norm; t.nrm[0] = NRM[0]; t.nrm[1] = NRM[1]; t.nrk = NRK;
    int n = m_bc >= 0 ? bk_len : 0;   // the newest filter (the JSFX showed it with the old one's length while they crossfade)
    t.gl.assign(GL, GL + n);
    t.gr.assign(GR, GR + n);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq.load(std::memory_order_relaxed) != s1) return false;
    v = std::move(t);
    return true;
}

// ======================================================================== setup
void Engine::init(double sr)
{
    srate = sr;
    if (!nonceDrawn) { eel_rand(1000000); nonceDrawn = true; }
    KT.assign(24576, 0.0);
    IRS.assign(4 * MAXT + 64, 0.0);
    SPR.assign(4 * MAXT + 64, 0.0);
    GLB.assign(MAXG + 96, 0.0);
    GRB.assign(MAXG + 96, 0.0);
    GL = GLB.data() + 16;
    GR = GRB.data() + 16;
    RING.assign(4 * RN, 0.0);
    RRING.assign(2 * RN, 0.0);
    DRY.assign(2048, 0.0);
    OUC.assign(4 * RO, 0.0);
    SWb.assign(32768, 0.0);
    TMPb.assign(32768, 0.0);
    ALX.assign(8192, 0.0);
    ALC.assign(8192, 0.0);
    LDS.assign(8 * 8192, 0.0);
    LDW.assign(8192, 0.0);
    m_T = -1; m_srw = 0; m_bc = -1; m_bn = -1; m_fading = false; m_fu = 0; m_fdone = false; m_cnt = 0; m_rp = 0; m_dp = 0;
    m_dirty = true; bk_ph = 0; m_bkn = 0;
    for (int s = 0; s < 2; ++s) RSD[s] = IRF(s) > 0;
    for (double &v : BKV) v = -1000000;
    e_first = true; c_mi0 = -200; c_mi1 = -200; c_ip0 = 0; c_ip1 = 0;
    seq.store(seq.load() | 1u);   // nothing to show until the first build
}

void Engine::reset()
{
    if (m_T >= 0) engReset();
    m_dirty = true;
}

// ======================================================================== the engine
// Zero-latency non-uniform partitioned convolution. A direct head (32 taps, zero latency only), then stages of FFT
// blocks growing 4x (the last up to 8192), each starting where its block allows no latency. The filter is the IRs
// delayed by the chosen latency, so a latency just means a larger first block and no head. Each stage keeps its newest
// input spectra; at each block end it adds the newest one times the first partition to what the others added during
// the block (spread out over the first stage's blocks, so no block does it all), transforms back and adds the result
// into the bank's output ring. A bank plays in one of three modes:
//   0: a mono input through one filter (mono IRs, centred): one real convolution, half the work of the others;
//   1: both outputs from one complex product: a mono input through left + j right, or a stereo input through one
//      filter for both sides;
//   2: a stereo input through different left and right filters, from the input split into left and right halves.
void Engine::layout()
{
    int n = m_T > 0 ? m_T : 32, i = 0;
    while (n < NMAX && i < 7) {
        int nn = std::min(4 * n, NMAX);
        SN[i] = n; SS[i] = n; SPM[i] = (nn - n) / n;
        i += 1;
        n = nn;
    }
    SN[i] = NMAX; SS[i] = NMAX; SPM[i] = (int)std::ceil((double)(MAXG - NMAX) / NMAX);
    i += 1;
    m_nst = i;
    m_hd = m_T > 0 ? 0 : 32;
    m_n0 = SN[0];
    // the pool: per stage and input slot its full, split and real spectra, per bank and partition the filter
    size_t tot = 0, acc = 0;
    for (int k = 0; k < m_nst; ++k) {
        size_t sz = 4 * (size_t)SN[k];
        tot += SPM[k] * (sz * 4 + sz / 2);
        acc += 2 * sz;
    }
    POOL.assign(tot, 0.0);
    ACCB.assign(acc, 0.0);
    double *p = POOL.data();
    for (int k = m_nst - 1; k >= 0; --k) {
        int sz = 4 * SN[k];
        for (int q = 0; q < SPM[k]; ++q) { FDZ[k * 16 + q] = p; p += sz; FDS[k * 16 + q] = p; p += sz; }
        for (int b = 0; b < 2; ++b)
            for (int q = 0; q < SPM[k]; ++q) { HPC[(b * 8 + k) * 16 + q] = p; p += sz; }
        for (int q = 0; q < SPM[k]; ++q) { FDR[k * 16 + q] = p; p += sz / 2; }
    }
    p = ACCB.data();
    for (int k = m_nst - 1; k >= 0; --k)
        for (int b = 0; b < 2; ++b) { ACP[b * 8 + k] = p; p += 4 * SN[k]; }
}

void Engine::engReset()
{
    m_bc = -1; m_bn = -1; m_fading = false; m_fu = 0; m_fdone = false; m_cnt = 0; m_rp = 0;
    std::fill(RING.begin(), RING.end(), 0.0);
    std::fill(RRING.begin(), RRING.end(), 0.0);
    std::fill(DRY.begin(), DRY.end(), 0.0);
    memset(BPN, 0, sizeof BPN); memset(BST, 0, sizeof BST); memset(SRUN, 0, sizeof SRUN); memset(SSPL, 0, sizeof SSPL);
    memset(SFZ, 0, sizeof SFZ); memset(SFR, 0, sizeof SFR); memset(BHN, 0, sizeof BHN); memset(BLN, 0, sizeof BLN);
    std::fill(OUC.begin(), OUC.end(), 0.0);
    memset(HX, 0, sizeof HX); memset(HY, 0, sizeof HY);
}

// the left and right half spectra of a full one (N + 1 points each, twice over; DC and Nyquist packed in the first)
static void spSplit(const double *z, double *d, int n)
{
    int m = 2 * n;
    double *xl = d, *xr = d + 2 * n;
    xl[0] = 2 * z[0]; xl[1] = 2 * z[m]; xr[0] = 2 * z[1]; xr[1] = 2 * z[m + 1];
    const double *p1 = z + 2, *p2 = z + 2 * (m - 1);
    xl += 2; xr += 2;
    for (int k = 0; k < n - 1; ++k) {
        double zr = p1[0], zi = p1[1], wr = p2[0], wi = p2[1];
        xl[0] = zr + wr; xl[1] = zi - wi; xr[0] = zi + wi; xr[1] = wr - zr;
        p1 += 2; p2 -= 2; xl += 2; xr += 2;
    }
}
// a mono input's real-FFT half spectrum (twice the DFT, DC and Nyquist packed first) from its full one, and back
static void frFromZ(const double *z, double *d, int n)
{
    d[0] = 2 * z[0]; d[1] = 2 * z[2 * n];
    for (int i = 2; i < 2 * n; ++i) d[i] = 2 * z[i];
}
static void zFromFr(const double *d, double *z, int n)
{
    int m = 2 * n;
    z[0] = 0.5 * d[0]; z[1] = 0; z[m] = 0.5 * d[1]; z[m + 1] = 0;
    for (int k = 1; k < n; ++k) {
        z[2 * k] = z[2 * (m - k)] = 0.5 * d[2 * k];
        z[2 * k + 1] = 0.5 * d[2 * k + 1];
        z[2 * (m - k) + 1] = -0.5 * d[2 * k + 1];
    }
}

// which stages run (a bank in use needs them; a stage starting from rest starts with no history) and which input
// spectra they keep: full (mode 1, 2), split (mode 2), real (mode 0). A form a stage starts keeping is worked out from
// the other for the history it has.
void Engine::srun()
{
    for (int i = 0; i < m_nst; ++i) {
        int r = 0, pa = 0, sp = 0, fz = 0, fr = 0;
        for (int bb = 0; bb < 2; ++bb) {
            int b = bb == 0 ? m_bc : m_bn;
            if (b < 0) continue;
            int k = BPN[b * 8 + i];
            if (k > 0) {
                r = 1;
                int md = BMODE[b];
                if (md == 0) fr = 1;
                else fz = 1;
                if (md == 2) sp = 1;
            }
            pa = std::max(pa, k);
        }
        if (r && !SRUN[i]) {
            for (int q = 0; q < SPM[i]; ++q) {
                memset(FDZ[i * 16 + q], 0, 4 * SN[i] * sizeof(double));
                memset(FDS[i * 16 + q], 0, 4 * SN[i] * sizeof(double));
                memset(FDR[i * 16 + q], 0, 2 * SN[i] * sizeof(double));
            }
            SFP[i] = 0; SQ[i] = pa; SNX[i] = -1;
        } else if (r) {
            if (fz && !SFZ[i])
                for (int q = 0; q < SPM[i]; ++q) zFromFr(FDR[i * 16 + q], FDZ[i * 16 + q], SN[i]);
            if (fr && !SFR[i])
                for (int q = 0; q < SPM[i]; ++q) frFromZ(FDZ[i * 16 + q], FDR[i * 16 + q], SN[i]);
            if (sp && !SSPL[i])
                for (int q = 0; q < SPM[i]; ++q) spSplit(FDZ[i * 16 + q], FDS[i * 16 + q], SN[i]);
        }
        SRUN[i] = r; SPA[i] = pa; SSPL[i] = sp; SFZ[i] = fz; SFR[i] = fr;
    }
}

// acc += x * h: m complex points
inline void Engine::macFull(double *acc, const double *x, const double *h, int m)
{
    for (int i = 0; i < m; ++i) {
        double ar = x[2 * i], ai = x[2 * i + 1], br = h[2 * i], bi = h[2 * i + 1];
        acc[2 * i] += ar * br - ai * bi;
        acc[2 * i + 1] += ar * bi + ai * br;
    }
}
// acc += x * h: a half spectrum (n complex points, the first holding DC and Nyquist, both real)
inline void Engine::macHalf(double *acc, const double *x, const double *h, int n)
{
    acc[0] += x[0] * h[0];
    acc[1] += x[1] * h[1];
    for (int i = 1; i < n; ++i) {
        double ar = x[2 * i], ai = x[2 * i + 1], br = h[2 * i], bi = h[2 * i + 1];
        acc[2 * i] += ar * br - ai * bi;
        acc[2 * i + 1] += ar * bi + ai * br;
    }
}
// bank b's partition p of stage i on input spectrum slot q, into its accumulator
void Engine::stMac(int b, int i, int p, int q)
{
    int k = b * 8 + i;
    if (!(p < BPN[k] && BST[k] >= 1)) return;
    int n = SN[i];
    const double *h = HPC[k * 16 + p];
    double *a = ACP[k];
    int md = BMODE[b];
    if (md == 0) macHalf(a, FDR[i * 16 + q], h, n);
    else if (md == 1) macFull(a, FDZ[i * 16 + q], h, 2 * n);
    else {
        const double *x = FDS[i * 16 + q];
        macHalf(a, x, h, n);
        macHalf(a + 2 * n, x + 2 * n, h + 2 * n, n);
    }
}
// stage i's next scheduled partition (during a block, for the next block's output); sb: the first-stage block it's in
void Engine::stSched(int i, int sb)
{
    int q = SQ[i];
    if (q >= SPA[i]) return;
    int sl = SFP[i] - (q - 1);
    if (sl < 0) sl += SPM[i];
    if (m_bc >= 0) stMac(m_bc, i, q, sl);
    if (m_bn >= 0) stMac(m_bn, i, q, sl);
    SQ[i] = q + 1;
    int kk = SN[i] / m_n0;
    SNX[i] = q + 1 < SPA[i] ? std::max(sb + 1, (int)std::floor((q + 1) * ((double)kk / SPA[i]))) : -1;
    if (SNX[i] >= kk) SNX[i] = -1;
}
// bank b's output block for stage i (its newest input spectrum in slot SFP[i]): into its output ring
void Engine::stOut(int b, int i)
{
    int k = b * 8 + i, n = SN[i], m = 2 * n, n2 = 2 * n;
    if (BPN[k] <= 0) return;
    if (BST[k] == 0) {
        BST[k] = 1;
        memset(ACP[k], 0, 2 * m * sizeof(double));
        return;
    }
    int sl = SFP[i], md = BMODE[b];
    double *a = ACP[k];
    const double *w;
    if (md == 0) {
        // one real convolution: the real inverse FFT gives m samples, the last n of them are this block
        macHalf(a, FDR[i * 16 + sl], HPC[k * 16], n);
        fft::realInverse(a, m);
        w = a + n;
    } else if (md == 1) {
        macFull(a, FDZ[i * 16 + sl], HPC[k * 16], m);
        fft::inverse(a, m);
        w = a + n2;
    } else {
        macHalf(a, FDS[i * 16 + sl], HPC[k * 16], n);
        macHalf(a + n2, FDS[i * 16 + sl] + n2, HPC[k * 16] + n2, n);
        // left in the real parts, right in the imaginary ones: W[k] = A[k] + jB[k], W[m - k] = conj(A[k]) + j conj(B[k])
        double *SW = SWb.data();
        const double *aa = a, *c = a + n2;
        double *p1 = SW + 2, *p2 = SW + 2 * (m - 1);
        SW[0] = aa[0]; SW[1] = c[0]; SW[n2] = aa[1]; SW[n2 + 1] = c[1];
        aa += 2; c += 2;
        for (int t = 0; t < n - 1; ++t) {
            double ar = aa[0], ai = aa[1], br = c[0], bi = c[1];
            p1[0] = ar - bi; p1[1] = ai + br; p2[0] = ar + bi; p2[1] = br - ai;
            aa += 2; c += 2; p1 += 2; p2 -= 2;
        }
        fft::inverse(SW, m);
        w = SW + n2;
    }
    // add it into the output ring from the next sample on (in up to two pieces where the ring wraps)
    double *o = OUC.data() + b * 2 * RO;
    int j = (m_cnt + 1) & (RO - 1), r = std::min(n, RO - j);
    double *oo = o + 2 * j;
    if (md == 0) {
        for (int t = 0; t < r; ++t) { oo[2 * t] += w[t]; oo[2 * t + 1] += w[t]; }
        for (int t = r; t < n; ++t) { o[2 * (t - r)] += w[t]; o[2 * (t - r) + 1] += w[t]; }
    } else {
        for (int t = 0; t < 2 * r; ++t) oo[t] += w[t];
        for (int t = 2 * r; t < 2 * n; ++t) o[t - 2 * r] += w[t];
    }
    BST[k] = 2;
    memset(ACP[k], 0, 2 * m * sizeof(double));
}
// stage i's block is in: its spectra (full: both channels from one FFT; real: the mono input), then each bank's
// output for the next block
void Engine::stBoundary(int i)
{
    int n = SN[i], m = 2 * n;
    // the partitions still owed this block
    while (SQ[i] < SPA[i]) stSched(i, 0);
    int sl = SFP[i] + 1;
    if (sl >= SPM[i]) sl = 0;
    SFP[i] = sl;
    if (SFZ[i]) {
        double *z = FDZ[i * 16 + sl];
        memcpy(z, m_qp - 2 * (m - 1), 2 * m * sizeof(double));
        fft::forward(z, m);
        if (SSPL[i]) spSplit(z, FDS[i * 16 + sl], n);
    }
    if (SFR[i]) {
        double *f = FDR[i * 16 + sl];
        memcpy(f, m_rq - (m - 1), m * sizeof(double));
        fft::realForward(f, m);
    }
    if (m_bc >= 0) stOut(m_bc, i);
    if (m_bn >= 0) stOut(m_bn, i);
    int kk = n / m_n0;
    SQ[i] = 1;
    SNX[i] = (SPA[i] > 1 && kk > 1) ? std::max(1, (int)std::floor((double)kk / SPA[i])) : -1;
}
// the end of a first-stage block: every stage's block end or scheduled partition that falls here
void Engine::tick()
{
    for (int i = 0; i < m_nst; ++i) {
        if (!SRUN[i]) continue;
        int n = SN[i], ph = (m_cnt + 1) & (n - 1);
        if (ph == 0) stBoundary(i);
        else if (ph / m_n0 == SNX[i]) stSched(i, ph / m_n0);
    }
}

// ======================================================================== building the filter, a little per block
// resample the IRs to the project rate, trim, fade, level, cut, delay, pan, add them up, cut into the stages'
// partitions, then crossfade to it
static double e_i0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k <= 30; ++k) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}
// slot s: get ready to resample it
void Engine::rsPrep(int s)
{
    rs_ratio = IRSR(s) / srate;
    rs_n = (int)std::min((double)MAXT, std::floor(IRF(s) / rs_ratio));
    rs_copy = std::fabs(rs_ratio - 1) < 0.000001;
    if (!rs_copy) {
        rs_c = emin(1, 1 / rs_ratio) * 0.96;
        rs_w = emin(95, 16 / rs_c);
        rs_s = 256;
        double i0b = e_i0(8);
        int nt = (int)std::floor(rs_w * rs_s) + 2;
        for (int t = 0; t < nt; ++t) {
            double d = t / rs_s, u = d / rs_w;
            double w = u < 1 ? e_i0(8 * std::sqrt(1 - u * u)) / i0b : 0;
            KT[t] = rs_c * (d < 0.000001 ? 1 : std::sin(PI * rs_c * d) / (PI * rs_c * d)) * w;
        }
    }
}
// fractional delay kernel (16 taps, windowed sinc) for a delay of fr (0..1) samples: taps at -7..8
void Engine::fdPrep(double fr)
{
    for (int j = 0; j < 16; ++j) {
        double d = j - 7 - fr, u = d / 8.5;
        FDK[j] = (std::fabs(d) < 0.000001 ? 1 : std::sin(PI * d) / (PI * d)) * (std::fabs(u) < 1 ? sqr(std::cos(0.5 * PI * u)) : 0);
    }
}
// a biquad over n samples of buf: hp = high pass, else low pass, quality q
void Engine::bqSet(double f, bool hp, double q)
{
    double w = 2 * PI * (emin(f, 0.45 * srate) / srate), cw = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    if (hp) { bq_b0 = (1 + cw) / 2 / a0; bq_b1 = -(1 + cw) / a0; }
    else { bq_b0 = (1 - cw) / 2 / a0; bq_b1 = (1 - cw) / a0; }
    bq_b2 = bq_b0;
    bq_a1 = -2 * (cw / a0);
    bq_a2 = (1 - al) / a0;
}
void Engine::bqRun(double *buf, int n)
{
    for (int i = 0; i < n; ++i) {
        double x = buf[i], y = bq_b0 * x + bq_s1;
        bq_s1 = bq_b1 * x - bq_a1 * y + bq_s2;
        bq_s2 = bq_b2 * x - bq_a2 * y;
        buf[i] = y;
    }
}

// start a build: take the settings as they are now
void Engine::bkStart()
{
    m_dirty = false; bk_ph = 1; bk_s = 0; bk_c = 0; bk_pos = 0; bk_sub = 0;
    seq.store((seq.load(std::memory_order_relaxed) | 1u), std::memory_order_release);
    bk_mono = pv(P_INM) >= 0.5;
    // the slots that sound: on, loaded, and soloed if any is
    bool a = (pv(irp(0, K_SOLO)) >= 0.5 && IRF(0) > 0) || (pv(irp(1, K_SOLO)) >= 0.5 && IRF(1) > 0);
    for (int s = 0; s < 2; ++s) BKU[s] = pv(irp(s, K_ON)) >= 0.5 && IRF(s) > 0 && (!a || pv(irp(s, K_SOLO)) >= 0.5);
    // with two IRs playing: line their starts up, match IR 2's timing and polarity to IR 1, and its loudness
    bk_two = BKU[0] && BKU[1];
    bk_align = bk_two && pv(P_ALIGN) >= 0.5;
    bk_match = bk_two && pv(P_MATCH) >= 0.5;
    bk_norm = pv(P_NORM) >= 0.5;
    bk_alvl = bk_two && pv(P_ALVL) >= 0.5 && !bk_norm;   // normalized, both are as loud already
}
// how many samples of a step fit in what's left of this block's budget (u: its cost per sample)
int Engine::bkFit(double u) const { return (int)std::min(1e9, emax(64, std::floor(bk_bud / u))); }

// 1: resample the IRs that need it (to the project rate; windowed sinc, Kaiser). An IR's taps scale with its rate
// (twice the rate, twice the taps for the same sound), so they are scaled by the ratio to keep its level.
void Engine::bkResample()
{
    int s = bk_s;
    if (s >= 2) { bk_ph = 2; bk_s = 0; bk_sub = 0; bk_pos = 0; return; }
    if (!RSD[s]) { bk_s += 1; bk_pos = 0; bk_c = 0; return; }
    if (bk_pos == 0 && bk_c == 0) {
        rsPrep(s);
        IRL[s] = rs_n;
        IRCH[s] = std::min(2, IRC(s));
        bk_bud -= rs_copy ? 16 : 3 * rs_w * rs_s;
    }
    const double *src = cur[s]->d[bk_c].data();
    double *dst = IRS.data() + (s * 2 + bk_c) * MAXT;
    double u = rs_copy ? 0.25 : 6 * rs_w;
    int n = std::min(rs_n - bk_pos, bkFit(u));
    const int irf = IRF(s);
    if (rs_copy) {
        memcpy(dst + bk_pos, src + bk_pos, n * sizeof(double));
    } else {
        for (int i = bk_pos; i < bk_pos + n; ++i) {
            double x = i * rs_ratio;
            int k = (int)emax(0, std::ceil(x - rs_w)), j = (int)emin(irf - 1, std::floor(x + rs_w));
            double v = 0;
            for (; k <= j; ++k) v += src[k] * KT[(int)std::floor(std::fabs(x - k) * rs_s + 0.5)];
            dst[i] = v * rs_ratio;
        }
        // the resampling kernel's taps before the IR's start (its pre-ringing) can't be played: they go into the
        // first tap, so the level (the low end) stays exactly the same
        if (bk_pos == 0) {
            int cnt = (int)std::ceil(rs_w / rs_ratio) + 1;
            for (int ii = 1; ii <= cnt; ++ii) {
                double x = -ii * rs_ratio;
                int j = (int)emin(irf - 1, std::floor(x + rs_w));
                double v = 0;
                for (int k = 0; k <= j; ++k) v += src[k] * KT[(int)std::floor(std::fabs(x - k) * rs_s + 0.5)];
                dst[0] += v * rs_ratio;
            }
        }
    }
    bk_bud -= n * u;
    bk_pos += n;
    if (bk_pos >= rs_n) {
        bk_pos = 0;
        bk_c += 1;
        if (bk_c >= IRC(s)) { bk_c = 0; RSD[s] = false; RSN_DONE[s] += 1; bk_s += 1; }
    }
}

// an IR's loudness: its response to pink noise, from its first 16384 samples. Two measures: A-weighted (matching two
// IRs, as the JSFX does) and K-weighted (normalizing: the weighting of LUFS, ITU-R BS.1770, which counts the low end
// as programme loudness does). For normalizing, its spectrum is kept (each channel's, in the bins measured) to measure
// both IRs together as they will play, and a unit impulse's loudness by the same measure (the reference).
static double kWeight2(double f)
{
    // BS.1770's two stages (its 48 kHz coefficients): the head's high shelf, then the high pass
    double w = 2 * PI * f / 48000, c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    auto mag2 = [&](double b0, double b1, double b2, double a1, double a2) {
        double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2), dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        return (nr * nr + ni * ni) / (dr * dr + di * di);
    };
    return mag2(1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585) *
           mag2(1, -2, 1, -1.99004745483398, 0.99007225036621);
}
void Engine::ldMeas(int s)
{
    const int m = 16384, h = m / 2;
    int n = std::min(IRLS[s], m);
    const double *src = SPR.data() + s * 2 * MAXT, *src2 = SPR.data() + (s * 2 + std::min(1, IRPC[s] - 1)) * MAXT;
    double *SW = SWb.data();
    for (int i = 0; i < n; ++i) { SW[2 * i] = src[i]; SW[2 * i + 1] = src2[i]; }
    memset(SW + 2 * n, 0, 2 * (m - n) * sizeof(double));
    fft::forward(SW, m);
    double acc = 0, acck = 0, ref = 0, *X = LDS.data() + s * 4 * h, *W = LDW.data();
    const double ftop = std::min(20000.0, 0.45 * srate);
    for (int k = 1; k < h; ++k) {
        double f = k * (srate / m);
        double a = SW[2 * k], b = SW[2 * k + 1], c = SW[2 * (m - k)], d = SW[2 * (m - k) + 1];
        if (f >= 30 && f <= 16000) {
            double f2 = f * f;
            double ra = 148693636 * f2 * (f2 / ((f2 + 424.36) * std::sqrt((f2 + 11599.29) * (f2 + 544496.41)) * (f2 + 148693636)));
            acc += (a * a + b * b + c * c + d * d) * 0.5 * ra * (ra / f);
        }
        // the two channels went in as the real and imaginary parts: each one's own spectrum
        X[4 * k] = 0.5 * (a + c); X[4 * k + 1] = 0.5 * (b - d);
        X[4 * k + 2] = 0.5 * (b + d); X[4 * k + 3] = 0.5 * (c - a);
        W[k] = f >= 20 && f <= ftop ? kWeight2(f) / f : 0;
        acck += (a * a + b * b + c * c + d * d) * 0.5 * W[k];
        ref += 2 * W[k];
    }
    IRLD[s] = acc;
    IRLK[s] = acck;
    LDREF = ref;
}
// normalizing: each IR to the loudness of a unit impulse (as loud as the input, by the same measure), then both
// together, as they will play at 0 dB (polarity, timing), to that loudness too, so a second IR blends in rather than
// adding up (up to +6 dB back where the two cancel). The levels then act on top: turning one down turns it down (the
// two aren't normalized again).
void Engine::normalize()
{
    NRM[0] = NRM[1] = 1;
    NRK = 1;
    if (!bk_norm || LDREF <= 0) return;
    for (int t = 0; t < 2; ++t)
        if (BKU[t] && IRL[t] > 0 && IRLK[t] > 0) NRM[t] = std::max(1e-4, std::min(1e3, std::sqrt(LDREF / IRLK[t])));
    if (!bk_two) return;
    const int h = 8192;
    const double *X0 = LDS.data(), *X1 = LDS.data() + 4 * h, *W = LDW.data();
    // each at 0 dB: only its polarity (and IR 2's, matched to IR 1)
    double a0 = (pv(irp(0, K_PHS)) >= 0.5 ? -1 : 1) * NRM[0], a1 = (pv(irp(1, K_PHS)) >= 0.5 ? -1 : 1) * APOL * NRM[1];
    double dl = BKD[1] - BKD[0], sum = 0;
    for (int k = 1; k < h; ++k) {
        if (W[k] == 0) continue;
        double ph = -PI * k / h * dl, cr = std::cos(ph), ci = std::sin(ph);
        for (int c = 0; c < 2; ++c) {
            double xr = X1[4 * k + 2 * c], xi = X1[4 * k + 2 * c + 1];
            double yr = a0 * X0[4 * k + 2 * c] + a1 * (xr * cr - xi * ci), yi = a0 * X0[4 * k + 2 * c + 1] + a1 * (xr * ci + xi * cr);
            sum += W[k] * (yr * yr + yi * yi);
        }
    }
    if (sum > 0) NRK = std::min(2.0, std::sqrt(LDREF / sum));
    bk_bud -= 30.0 * h;
}
// IR 2's timing and polarity against IR 1: the cross-correlation of their first 10 ms (both as mono) over +-2 ms;
// the strongest peak gives the lag, its sign the polarity
void Engine::alMatch()
{
    int lc = (int)emax(16, emin(std::floor(0.01 * srate), (double)std::min(IRLS[0], IRLS[1])));
    int kk = (int)std::floor(0.002 * srate);
    double *x0 = ALX.data(), *x1 = ALX.data() + 4096, *C = ALC.data();
    const double *S = SPR.data();
    for (int i = 0; i < lc; ++i) x0[i] = IRPC[0] > 1 ? (S[i] + S[MAXT + i]) * 0.5 : S[i];
    for (int i = 0; i < lc; ++i) x1[i] = IRPC[1] > 1 ? (S[2 * MAXT + i] + S[3 * MAXT + i]) * 0.5 : S[2 * MAXT + i];
    int bt = 0;
    double bv = -1;
    for (int t = -kk; t <= kk; ++t) {
        double c = 0;
        for (int n = std::max(0, t); n < std::min(lc, lc + t); ++n) c += x0[n] * x1[n - t];
        C[t + kk] = c;
        if (std::fabs(c) > bv) { bv = std::fabs(c); bt = t; }
    }
    // to a hundredth of a sample: the correlation is band-limited, so it is sinc-interpolated around its peak
    double dd = 0;
    if (bv > 0) {
        double f = bt - 1, bf = bv;
        for (int it = 0; it < 201; ++it) {
            double v = 0;
            int j = (int)emax(-kk, std::floor(f) - 16), je = (int)emin(kk, std::floor(f) + 16);
            for (; j <= je; ++j) {
                double d = f - j, w = std::fabs(d) < 0.000001 ? 1 : std::sin(PI * d) / (PI * d);
                v += C[j + kk] * w * (0.5 + 0.5 * std::cos(PI * (d / 17)));
            }
            if (std::fabs(v) > bf) { bf = std::fabs(v); dd = f - bt; }
            f += 0.01;
        }
    }
    ALAG = bv > 0 ? bt + dd : 0;
    APOL = (bv > 0 && C[bt + kk] < 0) ? -1 : 1;
    bk_bud -= (double)lc * (2 * kk + 1) * 3;
}

// 2: each slot that sounds, into its own buffer (one or two channels): from its start (lined up: just before its
// first sound) to where its tail falls under the trim level (with a short fade), at its stereo width; its loudness
// (to match two); its high and low pass
void Engine::bkSlot()
{
    int s = bk_s;
    if (s >= 2) { bk_ph = 3; bk_s = 0; bk_sub = 0; bk_pos = 0; return; }
    if (!BKU[s] || IRL[s] <= 0) { bk_s += 1; bk_sub = 0; bk_pos = 0; return; }
    if (bk_sub == 0) {
        int nch = (IRC(s) > 1 && pv(P_STIR) >= 0.5) ? 2 : 1;
        IRPC[s] = nch;
        const double *src = IRS.data() + s * 2 * MAXT, *src2 = IRS.data() + (s * 2 + std::min(1, IRC(s) - 1)) * MAXT;
        int n = IRL[s];
        double pk = 0;
        for (int i = 0; i < n; ++i) pk = emax(pk, emax(std::fabs(src[i]), std::fabs(src2[i])));
        // the start: a little before the first sample within 20 dB of the peak (lined up), or the file's own start
        int st = 0;
        if (bk_align) {
            double thr = pk * 0.1;
            int i1 = 0;
            while (i1 < n - 1 && emax(std::fabs(src[i1]), std::fabs(src2[i1])) < thr) i1 += 1;
            st = (int)emax(0, i1 - emax(8, std::floor(0.00015 * srate)));
        }
        // the end: the last sample over the trim level (relative to the peak), then a 2 ms fade (under that level)
        int ls = n;
        if (pv(P_TTON) >= 0.5) {
            double thr = pk * epow(10, pv(P_TTDB) / 20);
            int i = n - 1;
            while (i > st && emax(std::fabs(src[i]), std::fabs(src2[i])) < thr) i -= 1;
            ls = (int)emin(n, i + 1 + std::floor(0.002 * srate));
        }
        ls = std::max(st + 16, std::min(ls, st + MAXT));
        // the fade where the tail was trimmed, or 10 ms where the IR was too long and cut short
        int fd = ls < n ? (int)std::floor(0.002 * srate) : 0;
        if (IRF(s) * (srate / IRSR(s)) > MAXT + 1 && ls >= st + MAXT) fd = (int)std::floor(0.01 * srate);
        IRST[s] = st;
        IRLS[s] = ls - st;
        bk_fd = std::max(0, std::min(ls - st, fd));
        bk_bud -= 3.0 * n;
        bk_pos = 0;
        bk_sub = 1;
    } else if (bk_sub == 1) {
        int ls = IRLS[s], nch = IRPC[s], st = IRST[s];
        int n = std::min(ls - bk_pos, bkFit(10));
        const double *src = IRS.data() + s * 2 * MAXT + st, *src2 = IRS.data() + (s * 2 + std::min(1, IRC(s) - 1)) * MAXT + st;
        double *dst = SPR.data() + s * 2 * MAXT;
        double w = pv(irp(s, K_WID)) * 0.01;
        int f0 = ls - bk_fd;
        for (int i = bk_pos; i < bk_pos + n; ++i) {
            double l = src[i], r = src2[i];
            if (i >= f0) {
                double v = 0.5 + 0.5 * std::cos(PI * ((double)(i - f0 + 1) / (bk_fd + 1)));
                l *= v;
                r *= v;
            }
            if (nch == 2) {
                // stereo width: the side part scaled (0% mono, 100% as recorded, 200% twice as wide)
                double m = (l + r) * 0.5, sd = (l - r) * 0.5 * w;
                dst[i] = m + sd;
                dst[MAXT + i] = m - sd;
            } else {
                dst[i] = IRC(s) > 1 ? (l + r) * 0.5 : l;
            }
        }
        bk_bud -= 10.0 * n;
        bk_pos += n;
        if (bk_pos >= ls) { bk_pos = 0; bk_c = 0; bk_sub = 2; }
    } else if (bk_sub == 2) {
        if (bk_alvl || bk_norm) { ldMeas(s); bk_bud -= 60000; }
        bk_sub = 3; bk_c = 0; bk_pos = 0;
    } else if (bk_sub <= 4) {
        // the high pass (3) and the low pass (4): each section of its slope, each channel in turn
        int k = bk_sub == 3 ? K_HPON : K_LPON;
        int c = (int)emax(0, emin(2, std::floor(pv(irp(s, bk_sub == 3 ? K_HPS : K_LPS)) + 0.5)));
        if (pv(irp(s, k)) < 0.5) {
            bk_sub += 1; bk_c = 0; bk_pos = 0;
        } else {
            int sec = bk_c / IRPC[s];
            double q0 = QT[QS0[c] + sec];
            if (bk_pos == 0) { bqSet(pv(irp(s, k + 1)), bk_sub == 3, q0); bq_s1 = bq_s2 = 0; }
            int n = std::min(IRLS[s] - bk_pos, bkFit(8));
            bqRun(SPR.data() + (s * 2 + (bk_c % IRPC[s])) * MAXT + bk_pos, n);
            bk_bud -= 8.0 * n;
            bk_pos += n;
            if (bk_pos >= IRLS[s]) {
                bk_pos = 0;
                bk_c += 1;
                if (bk_c >= QSN[c] * IRPC[s]) { bk_c = 0; bk_sub += 1; }
            }
        }
    } else {
        bk_s += 1; bk_sub = 0; bk_pos = 0;
    }
}

// 3: both into the filter, after the latency: each with its level, polarity, delay (the fraction of a sample by a
// windowed sinc) and pan; IR 2 matched to IR 1 first (timing, polarity, loudness) when that's on
void Engine::bkSum()
{
    int s = bk_s;
    if (bk_sub == 0) {
        std::fill(GLB.begin(), GLB.end(), 0.0);
        std::fill(GRB.begin(), GRB.end(), 0.0);
        bk_len = 0;
        bk_bud -= MAXG / 4;
        ALAG = 0; APOL = 1; ALVG = 0;
        if (bk_match) alMatch();
        if (bk_alvl && IRLD[0] > 0 && IRLD[1] > 0) ALVG = 10 * std::log10(IRLD[0] / IRLD[1]);
        for (int t = 0; t < 2; ++t) {
            BKG[t] = epow(10, pv(irp(t, K_LVL)) / 20) * (pv(irp(t, K_PHS)) >= 0.5 ? -1 : 1);
            BKD[t] = pv(irp(t, K_DLY)) * 0.001 * srate;
            if (t == 1) { BKG[1] *= APOL * epow(10, ALVG / 20); BKD[1] += emax(0, ALAG); }
            else BKD[0] += emax(0, -ALAG);
            IRG[t] = BKG[t];
        }
        normalize();
        if (bk_norm)
            for (int t = 0; t < 2; ++t) { BKG[t] *= NRM[t] * NRK; IRG[t] = BKG[t]; }
        bk_s = 0;
        bk_sub = 1;
    } else if (s >= 2) {
        // nothing sounds: the input passes (delayed by the latency), with no convolution at all
        bk_pass = bk_len == 0;
        if (bk_len == 0) { GL[m_T] = 1; GR[m_T] = 1; bk_len = m_T + 1; }
        bk_same = true;
        for (int i = 0; bk_same && i < bk_len; ++i)
            if (GL[i] != GR[i]) bk_same = false;
        bk_bud -= bk_len;
        bk_ph = 4; bk_s = 0; bk_sub = 0; bk_pos = 0;
    } else if (!BKU[s] || IRL[s] <= 0) {
        bk_s += 1; bk_sub = 1; bk_pos = 0;
    } else if (bk_sub == 1) {
        double d = BKD[s];
        bk_di = (int)std::floor(d);
        bk_fr = d - bk_di;
        if (bk_fr < 0.001) bk_fr = 0;
        if (bk_fr > 0.999) { bk_fr = 0; bk_di += 1; }
        if (bk_fr > 0) fdPrep(bk_fr);
        double pn = pv(irp(s, K_PAN)) * 0.01;
        bk_gl = pn > 0 ? std::cos(pn * 0.5 * PI) : 1;
        bk_gr = pn < 0 ? std::cos(-pn * 0.5 * PI) : 1;
        bk_base = m_T + bk_di;
        bk_ls = std::max(0, std::min(IRLS[s], MAXG - 9 - bk_base));
        bk_c = 0; bk_pos = 0; bk_sub = 2;
    } else {
        int c = bk_c, n;
        const double *src = SPR.data() + (s * 2 + std::min(c, IRPC[s] - 1)) * MAXT;
        double *dst = c ? GR : GL;
        double g = BKG[s] * (c ? bk_gr : bk_gl);
        if (bk_fr > 0) {
            n = std::min(bk_ls - bk_pos, bkFit(20));
            double *t = dst + bk_base + bk_pos - 7;
            for (int i = bk_pos; i < bk_pos + n; ++i, ++t) {
                double v = src[i] * g;
                for (int q = 0; q < 16; ++q) t[q] += v * FDK[q];
            }
            bk_bud -= 20.0 * n;
        } else {
            n = std::min(bk_ls - bk_pos, bkFit(2));
            double *t = dst + bk_base;
            for (int i = bk_pos; i < bk_pos + n; ++i) t[i] += src[i] * g;
            bk_bud -= 2.0 * n;
        }
        bk_pos += n;
        if (bk_pos >= bk_ls) {
            bk_pos = 0;
            bk_c += 1;
            if (bk_c >= 2) {
                if (bk_ls > 0) bk_len = std::max(bk_len, bk_base + bk_ls + (bk_fr > 0 ? 9 : 0));
                bk_s += 1;
                bk_sub = 1;
            }
        }
    }
}

// 4: into the bank not playing: its head, then each stage's partitions (one per step); then start it
void Engine::bkPart()
{
    if (m_bn >= 0) { bk_bud = 0; return; }
    int b = m_bc >= 0 ? 1 - m_bc : 0;
    if (bk_s == 0 && bk_pos == 0) {
        BLN[b] = bk_len; BSAME[b] = bk_same; BHN[b] = std::min(m_hd, bk_len);
        BMODE[b] = (bk_mono && bk_same) ? 0 : ((bk_mono || bk_same) ? 1 : 2);
        BPASS[b] = bk_pass;
        if (bk_pass) { BHN[b] = 0; bk_s = m_nst; }
        for (int i = 0; i < 32; ++i) {
            HDL[b * 32 + i] = i < BHN[b] ? GL[i] : 0;
            HDR[b * 32 + i] = i < BHN[b] ? GR[i] : 0;
        }
        for (int i = 0; i < m_nst; ++i) {
            BPN[b * 8 + i] = (!bk_pass && bk_len > SS[i]) ? std::min(SPM[i], (int)std::ceil((double)(bk_len - SS[i]) / SN[i])) : 0;
            BST[b * 8 + i] = 0;
        }
    }
    int st = bk_s;
    if (st >= m_nst) {
        // it plays alongside the old one until each of its stages has a full block, then they crossfade (see block())
        std::fill(OUC.begin() + b * 2 * RO, OUC.begin() + (b + 1) * 2 * RO, 0.0);
        if (m_bc < 0) m_bc = b;
        else m_bn = b;
        srun();
        bk_ph = 0;
        m_bkn += 1;
        seq.store((seq.load(std::memory_order_relaxed) | 1u) + 1, std::memory_order_release);
    } else if (bk_pos >= BPN[b * 8 + st]) {
        bk_s += 1;
        bk_pos = 0;
    } else {
        int n = SN[st], m = 2 * n, nn = SS[st] + bk_pos * n;
        double *h = HPC[(b * 8 + st) * 16 + bk_pos];
        if (BMODE[b] == 0) {
            // one real filter: its real-FFT half spectrum, scaled for the real inverse FFT (which gives 2m times) and the
            // input's real FFT (twice the DFT)
            double sc = 1.0 / (4 * m);
            for (int i = 0; i < n; ++i) { int j = nn + i; h[i] = j < bk_len ? GL[j] * sc : 0; }
            memset(h + n, 0, n * sizeof(double));
            fft::realForward(h, m);
        } else if (BMODE[b] == 1) {
            // one complex filter: left + j right (mono input), or the one filter (stereo input, both the same); the
            // inverse FFT's 1 / m folded in
            double sc = 1.0 / m;
            for (int i = 0; i < n; ++i) {
                int j = nn + i;
                double vl = j < bk_len ? GL[j] : 0, vr = j < bk_len ? GR[j] : 0;
                h[2 * i] = vl * sc;
                h[2 * i + 1] = bk_mono ? vr * sc : 0;
            }
            memset(h + m, 0, m * sizeof(double));
            fft::forward(h, m);
        } else {
            // left and right half spectra, scaled for the inverse FFT and the doubled input halves
            double *SW = SWb.data();
            for (int i = 0; i < n; ++i) {
                int j = nn + i;
                SW[2 * i] = j < bk_len ? GL[j] : 0;
                SW[2 * i + 1] = j < bk_len ? GR[j] : 0;
            }
            memset(SW + m, 0, m * sizeof(double));
            fft::forward(SW, m);
            double sc = 1.0 / (4 * m);
            spSplit(SW, h, n);
            for (int i = 0; i < 2 * m; ++i) h[i] *= sc;
        }
        bk_bud -= 8.0 * m;
        bk_pos += 1;
    }
}
// as much of the build as this block's budget allows
void Engine::bkRun(double bud)
{
    bk_bud = bud;
    int guard = 0;
    while (bk_ph > 0 && bk_bud > 0 && guard < 64) {
        if (bk_ph == 1) bkResample();
        else if (bk_ph == 2) bkSlot();
        else if (bk_ph == 3) bkSum();
        else bkPart();
        bk_bud -= 16;
        guard += 1;
    }
}

// ======================================================================== per block
void Engine::block(int samplesblock)
{
    ab.block(U, A.now(), false, samplesblock, srate);
    take();
    // the latency (and the sample rate) set the engine's stages; a change starts it again
    int t = latencyOf(pv(P_LAT));
    if (t != m_T || srate != m_srw) {
        m_T = t;
        m_srw = srate;
        layout();
        engReset();
        m_dirty = true;
        if (bk_ph != 0) seq.store((seq.load(std::memory_order_relaxed) | 1u), std::memory_order_release);
        bk_ph = 0;
    }
    m_inmono = pv(P_INM) >= 0.5;
    // a setting the filter depends on changed: build it again (one build at a time; changes during one start another)
    for (int s = 0; s < 2; ++s)
        for (int k = 0; k <= K_SOLO; ++k) {
            int p = irp(s, k);
            double v = pv(p);
            if (v != BKV[p]) { BKV[p] = v; m_dirty = true; }
        }
    static const int G[] = {P_STIR, P_ALVL, P_INM, P_ALIGN, P_MATCH, P_TTON, P_TTDB, P_NORM};
    for (int p : G) {
        double v = pv(p);
        if (v != BKV[p]) { BKV[p] = v; m_dirty = true; }
    }
    if (m_dirty && bk_ph == 0) bkStart();
    // the build gets about 200 operations' worth per sample of this block (times budgetMul: native code is faster)
    if (bk_ph > 0) bkRun(samplesblock * 200 * budgetMul);
    // the new filter: once each of its stages has played a full block, crossfade to it (20 ms)
    if (m_bn >= 0 && !m_fading) {
        bool ok = true;
        for (int i = 0; i < m_nst; ++i)
            if (BPN[m_bn * 8 + i] > 0 && BST[m_bn * 8 + i] != 2) ok = false;
        if (ok) { m_fading = true; m_fu = 0; m_fdu = 1 / (0.02 * srate); }
    }
    if (m_fdone) {
        m_fdone = false; m_fading = false; m_fu = 0;
        for (int i = 0; i < m_nst; ++i) { BST[m_bc * 8 + i] = 0; BPN[m_bc * 8 + i] = 0; }
        m_bc = m_bn;
        m_bn = -1;
        srun();
    }
    // dry/wet (glides)
    double mix = pv(P_MIX) * 0.01;
    c_dm = 1 - mix;
    c_wmt = mix;
    c_zk = 1 - std::exp(-1 / (0.01 * srate));
    if (e_first) { c_dms = c_dm; c_wms = c_wmt; e_first = false; }
    // meters for the window: input peaks
    double dur = samplesblock / srate;
    c_mi0 = emax(c_ip0 > 0.0000000001 ? 20 * std::log10(c_ip0) : -200, c_mi0 - 20 * dur);
    c_mi1 = emax(c_ip1 > 0.0000000001 ? 20 * std::log10(c_ip1) : -200, c_mi1 - 20 * dur);
    c_ip0 = 0;
    c_ip1 = 0;
    mi0.store(c_mi0, std::memory_order_relaxed);
    mi1.store(c_mi1, std::memory_order_relaxed);
}

void Engine::afterAct()
{
    if (!idDrawn && A.myid != 0) { eel_rand(899999); idDrawn = true; }
}

// ======================================================================== per sample
void Engine::samples(const double *const *in, double *const *out, int n)
{
    if ((int)br0.size() < n) { br0.resize(n); br1.resize(n); bq0.resize(n); bq1.resize(n); }
    double *ring = RING.data(), *rring = RRING.data(), *dry = DRY.data(), *ouc = OUC.data();
    for (int i = 0; i < n; ++i) {
        c_dms += c_zk * (c_dm - c_dms);
        c_wms += c_zk * (c_wmt - c_wms);
        double c_r0 = in[0][i], c_r1 = in[1][i];
        // the raw input, delayed by the latency (dry/wet and Bypass)
        dry[m_dp] = c_r0;
        dry[1024 + m_dp] = c_r1;
        int q = (m_dp - m_T) & 1023;
        double m_rd0 = dry[q], m_rd1 = dry[1024 + q];
        m_dp = (m_dp + 1) & 1023;
        // into the input history (Mono: both summed, in the real parts)
        double xl, xr, hy;
        if (m_inmono) { xl = (c_r0 + c_r1) * 0.5; xr = 0; hy = xl; }
        else { xl = c_r0; xr = c_r1; hy = xr; }
        double *aq = ring + 2 * m_rp;
        aq[0] = xl; aq[1] = xr;
        m_qp = aq + 2 * RN;
        m_qp[0] = xl; m_qp[1] = xr;
        // ... and the mono sum on its own (mode 0)
        double tt = m_inmono ? xl : (c_r0 + c_r1) * 0.5;
        rring[m_rp] = tt;
        m_rq = rring + m_rp + RN;
        m_rq[0] = tt;

        // the filter in use (and, while a new one comes in, that one too): the head (zero latency), then the stages'
        // output
        double cl = 0, cr = 0, nl = 0, nr = 0;
        if (m_hd) {
            hp = (hp - 1) & 31;
            HX[hp] = HX[hp + 32] = xl;
            if (!m_inmono) HY[hp] = HY[hp + 32] = hy;
            const double *hx = HX + hp, *hyv = HY + hp;
            for (int bb = 0; bb < 2; ++bb) {
                int b = bb == 0 ? m_bc : m_bn;
                if (b < 0 || BPASS[b]) continue;
                const double *hl = HDL + b * 32, *hr = HDR + b * 32;
                double l = 0;
                for (int k = 0; k < 32; ++k) l += hl[k] * hx[k];
                double r;
                if (BMODE[b] == 0) r = l;
                else {
                    r = 0;
                    const double *src = m_inmono ? hx : hyv;
                    for (int k = 0; k < 32; ++k) r += hr[k] * src[k];
                }
                if (bb == 0) { cl = l; cr = r; }
                else { nl = l; nr = r; }
            }
        }
        double *ao = ouc + 2 * (m_cnt & (RO - 1));
        if (m_bc >= 0) { double *p = ao + m_bc * 2 * RO; cl += p[0]; cr += p[1]; p[0] = 0; p[1] = 0; }
        if (m_bn >= 0) { double *p = ao + m_bn * 2 * RO; nl += p[0]; nr += p[1]; p[0] = 0; p[1] = 0; }
        // no IR sounding: the input itself (delayed by the latency)
        if (m_bc >= 0 && BPASS[m_bc]) { cl = m_rd0; cr = m_rd1; }
        if (m_bn >= 0 && BPASS[m_bn]) { nl = m_rd0; nr = m_rd1; }
        if ((m_cnt & (m_n0 - 1)) == m_n0 - 1) tick();
        m_cnt = (m_cnt + 1) & 1048575;
        m_rp = m_cnt & (RN - 1);
        // the crossfade to a new filter
        if (m_fading) {
            m_fu += m_fdu;
            if (m_fu >= 1) { m_fu = 1; m_fdone = true; }
            cl += (nl - cl) * m_fu;
            cr += (nr - cr) * m_fu;
        }
        out[0][i] = m_rd0 * c_dms + cl * c_wms;
        out[1][i] = m_rd1 * c_dms + cr * c_wms;
        br0[i] = c_r0; br1[i] = c_r1; bq0[i] = m_rd0; bq1[i] = m_rd1;
    }
}

// after the built-in Act: meters and level matching (only for the window), held Bypass
void Engine::post(double *const *out, int n)
{
    for (int i = 0; i < n; ++i) {
        if (ab.uion) {
            double a = std::fabs(br0[i]);
            if (a > c_ip0) c_ip0 = a;
            a = std::fabs(br1[i]);
            if (a > c_ip1) c_ip1 = a;
        }
        ab.post(br0[i], br1[i], out[0][i], out[1][i], bq0[i], bq1[i]);
    }
}

// ======================================================================== reading a .wav file
static uint32_t rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

IrData *readWav(const std::string &path, std::string &err)
{
    std::vector<unsigned char> b;
    {
        std::error_code ec;
        std::filesystem::path fp = std::filesystem::u8path(path);   // (UTF-8 names, on Windows too)
        uintmax_t sz = std::filesystem::file_size(fp, ec);
        if (ec) { err = "couldn't open the file"; return nullptr; }
        if (sz < 44 || sz > 512ull * 1024 * 1024) { err = "not a .wav file"; return nullptr; }
        std::ifstream in(fp, std::ios::binary);
        if (!in) { err = "couldn't open the file"; return nullptr; }
        b.resize((size_t)sz);
        in.read((char *)b.data(), (std::streamsize)sz);
        if (!in) { err = "couldn't read the file"; return nullptr; }
    }
    bool rf64 = !memcmp(b.data(), "RF64", 4);
    if ((memcmp(b.data(), "RIFF", 4) && !rf64) || memcmp(b.data() + 8, "WAVE", 4)) { err = "not a .wav file"; return nullptr; }
    int fmt = 0, nch = 0, bits = 0;
    uint32_t sr = 0;
    const unsigned char *data = nullptr;
    uint64_t dlen = 0;
    size_t p = 12;
    while (p + 8 <= b.size()) {
        const unsigned char *c = b.data() + p;
        uint64_t len = rd32(c + 4);
        if (!memcmp(c, "fmt ", 4) && len >= 16) {
            fmt = rd16(c + 8);
            nch = rd16(c + 10);
            sr = rd32(c + 12);
            bits = rd16(c + 22);
            if (fmt == 0xFFFE && len >= 40) fmt = rd16(c + 32);   // WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes
        } else if (!memcmp(c, "data", 4)) {
            data = c + 8;
            dlen = (len == 0xFFFFFFFFu || rf64) ? b.size() - (p + 8) : len;
            dlen = std::min<uint64_t>(dlen, b.size() - (p + 8));
            break;
        }
        p += 8 + len + (len & 1);
    }
    if (!data || nch < 1 || sr < 8000 || !(fmt == 1 || fmt == 3) || !(bits == 16 || bits == 24 || bits == 32 || bits == 64 || bits == 8)) {
        err = "unsupported .wav format";
        return nullptr;
    }
    if (fmt == 3 && !(bits == 32 || bits == 64)) { err = "unsupported .wav format"; return nullptr; }
    int bps = bits / 8;
    uint64_t frames = dlen / ((uint64_t)bps * nch);
    int fr = (int)std::min<uint64_t>(frames, MAXR);
    if (fr <= 0) { err = "the file has no audio"; return nullptr; }
    IrData *d = new IrData();
    d->frames = fr;
    d->chans = std::min(2, nch);
    d->sr = sr;
    for (int c = 0; c < d->chans; ++c) d->d[c].resize(fr);
    for (int i = 0; i < fr; ++i)
        for (int c = 0; c < d->chans; ++c) {
            const unsigned char *s = data + ((size_t)i * nch + c) * bps;
            double v = 0;
            if (fmt == 3) {
                if (bits == 32) { float x; memcpy(&x, s, 4); v = x; }
                else { double x; memcpy(&x, s, 8); v = x; }
            } else if (bits == 8) v = ((int)s[0] - 128) / 128.0;
            else if (bits == 16) v = (int16_t)rd16(s) / 32768.0;
            else if (bits == 24) v = (double)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) / 8388608.0;
            else v = (int32_t)rd32(s) / 2147483648.0;
            d->d[c][i] = v;
        }
    return d;
}

} // namespace irl
} // namespace ts
