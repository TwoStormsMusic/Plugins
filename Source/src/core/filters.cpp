#include "filters.h"
#include "eel.h"
#include <algorithm>
#include <cmath>

namespace ts {
namespace flt {

static const double QT[6] = {0.5411961, 1.306563, 0.5097956, 0.6013449, 0.8999762, 2.5629154};

// the inverse filter: numerator and denominator swap
void inv(double *o)
{
    double b0 = o[0], b1 = o[1], b2 = o[2], a1 = o[3], a2 = o[4];
    o[0] = 1 / b0; o[1] = a1 / b0; o[2] = a2 / b0; o[3] = b1 / b0; o[4] = b2 / b0;
}

//   kind 0: bell (s^2 + s A/Q + 1) / (s^2 + s/(A Q) + 1)      kind 1: low shelf (boost)
//   kind 2: band pass (s/Q) / (s^2 + s/Q + 1)                  kind 3: low pass 1 / (s^2 + s/Q + 1)
bool m3(int kind, double A, double Q, double w, double *o)
{
    double rho, z;
    if (kind == 0) { rho = 1; z = 0.5 / (A * Q); }
    else if (kind == 1) { rho = 1 / std::sqrt(A); z = 0.5 / Q; }
    else { rho = 1; z = 0.5 / Q; }
    double wp = w * rho, a1, a2;
    if (z < 1) {
        double e = std::exp(-z * wp);
        a1 = -2 * e * std::cos(wp * std::sqrt(1 - z * z));
        a2 = e * e;
    } else {
        double sq = std::sqrt(z * z - 1), p1 = std::exp(-wp * (z - sq)), p2 = std::exp(-wp * (z + sq));
        a1 = -(p1 + p2);
        a2 = p1 * p2;
    }
    double x2 = PI / w;
    x2 *= x2;
    double qq = Q * Q, hn, h0, hf;
    if (kind == 0) {
        hn = (sqr(1 - x2) + x2 * A * (A / qq)) / (sqr(1 - x2) + x2 / (A * A * qq)); h0 = 1; hf = sqr(A * A);
    } else if (kind == 1) {
        hn = A * A * ((sqr(A - x2) + x2 * (A / qq)) / (sqr(1 - A * x2) + x2 * (A / qq))); h0 = sqr(A * A); hf = A * A;
    } else if (kind == 2) {
        hn = (x2 / qq) / (sqr(1 - x2) + x2 / qq); h0 = 0; hf = 1;
    } else {
        hn = 1 / (sqr(1 - x2) + x2 / qq); h0 = 1; hf = qq;
    }
    double d0 = 1 + a1 + a2, dn = 1 - a1 + a2, cw = std::cos(w);
    double ww = std::sqrt(h0) * d0, vv = std::sqrt(hn) * dn;
    double d2f = 1 + a1 * a1 + a2 * a2 + 2 * (a1 + a1 * a2) * cw + 2 * a2 * std::cos(2 * w);
    double s1 = (1 - cw) * 0.5, s0 = 1 - s1, s2 = 4 * s0 * s1;
    double t2 = (hf * d2f - ww * ww * s0 - vv * vv * s1) / s2;
    double u = (ww + vv) * 0.5, b1 = (ww - vv) * 0.5, disc = u * u + t2;
    if (disc >= 0) {
        double b0 = (u + std::sqrt(disc)) * 0.5, b2 = u - b0;
        if (std::fabs(b0) > 0.000000001 && std::fabs(b2 / b0) < 0.9999 && std::fabs(b1 / b0) <= 1 + b2 / b0 + 0.000000001 && a2 < 0.9999 &&
            std::fabs(a1) < 1 + a2 - 0.000001) {
            o[0] = b0; o[1] = b1; o[2] = b2; o[3] = a1; o[4] = a2;
            return true;
        }
    }
    return false;
}

// the classic RBJ bell / shelf / notch / band pass (the fallback), from w and sin/cos
void rbj(int type, double w, double g, double q, double *o)
{
    double cw = std::cos(w), sw = std::sin(w), al = sw / (2 * q), A = epow(10, g / 40), sq, a0;
    if (type == 0) {
        a0 = 1 + al / A;
        o[0] = (1 + al * A) / a0; o[1] = -2 * (cw / a0); o[2] = (1 - al * A) / a0; o[3] = o[1]; o[4] = (1 - al / A) / a0;
    }
    if (type == 1) {
        sq = 2 * std::sqrt(A) * al;
        a0 = (A + 1) + (A - 1) * cw + sq;
        o[0] = A * (((A + 1) - (A - 1) * cw + sq) / a0);
        o[1] = 2 * A * (((A - 1) - (A + 1) * cw) / a0);
        o[2] = A * (((A + 1) - (A - 1) * cw - sq) / a0);
        o[3] = -2 * (((A - 1) + (A + 1) * cw) / a0);
        o[4] = ((A + 1) + (A - 1) * cw - sq) / a0;
    }
    if (type == 2) {
        sq = 2 * std::sqrt(A) * al;
        a0 = (A + 1) - (A - 1) * cw + sq;
        o[0] = A * (((A + 1) + (A - 1) * cw + sq) / a0);
        o[1] = -2 * A * (((A - 1) + (A + 1) * cw) / a0);
        o[2] = A * (((A + 1) + (A - 1) * cw - sq) / a0);
        o[3] = 2 * (((A - 1) - (A + 1) * cw) / a0);
        o[4] = ((A + 1) - (A - 1) * cw - sq) / a0;
    }
    if (type == 3) {
        a0 = 1 + al;
        o[0] = 1 / a0; o[1] = -2 * (cw / a0); o[2] = o[0]; o[3] = o[1]; o[4] = (1 - al) / a0;
    }
    if (type == 4) {
        a0 = 1 + al;
        o[0] = al / a0; o[1] = 0; o[2] = -al / a0; o[3] = -2 * (cw / a0); o[4] = (1 - al) / a0;
    }
}

double qe(int type, double g, double q, double qm)
{
    return (qm > 0 && type <= 2) ? emin(100, q * (1 + (eq(qm, 1) ? 0.5 : (eq(qm, 2) ? 1 : 2)) * (type == 0 ? 1 : 0.5) * (emin(std::fabs(g), 30) / 18))) : q;
}

int coef(int type, double f, double g, double q, double qm, double *o, double srate)
{
    double w = 2 * PI * (std::min(f, srate * 0.49) / srate);
    q = emax(0.05, q);
    q = qe(type, g, q, qm);
    int n = 0;
    if (type <= 2 && std::fabs(g) >= 0.001) {
        double A = epow(10, std::fabs(g) / 40);
        bool ok = m3(type == 0 ? 0 : 1, A, q, w, o);
        if (ok) {
            if (type == 0) { if (g < 0) inv(o); }
            if (type == 1) { if (g < 0) inv(o); }
            if (type == 2) {
                A *= A;
                if (g >= 0) { inv(o); o[0] *= A; o[1] *= A; o[2] *= A; }
                else { o[0] /= A; o[1] /= A; o[2] /= A; }
            }
        } else {
            rbj(type, w, g, q, o);
        }
        n = 1;
    }
    if (type == 3) {
        // notch: zeros exactly at the centre; the pole radius is the analog's and the pole angle makes the Nyquist gain the analog's
        double cw = std::cos(w), c1 = PI / w;
        c1 *= c1;
        double qq = q * q, e = std::exp(-w / q);
        double gn = std::sqrt(sqr(1 - c1) / (sqr(1 - c1) + c1 / qq));
        double c2 = (1 + cw) / (1 - cw);
        o[3] = (gn - c2) * ((1 + e) / (c2 + gn));
        o[4] = e;
        o[0] = (1 + o[3] + o[4]) / (2 - 2 * cw);
        o[1] = -2 * o[0] * cw;
        o[2] = o[0];
        if (!(e < 0.9999 && std::fabs(o[3]) < 1 + e - 0.000001 && o[0] > 0.000000001)) rbj(3, w, 0, q, o);
        n = 1;
    }
    if (type == 4) {
        if (!m3(2, 1, q, w, o)) rbj(4, w, 0, q, o);
        n = 1;
    }
    if (type >= 5) {
        bool hp = type <= 8;
        int sl = hp ? type - 5 : type - 9;
        if (sl == 0) {
            if (hp) {
                double k = std::tan(w * 0.5);
                o[0] = 1 / (1 + k); o[1] = -o[0]; o[2] = 0; o[3] = (k - 1) / (1 + k); o[4] = 0;
            } else {
                // one pole, DC gain 1, Nyquist gain the analog's
                double e = std::exp(-w), gn = 1 / std::sqrt(1 + sqr(PI / w));
                o[3] = -e;
                o[0] = ((1 - e) + gn * (1 + e)) * 0.5;
                o[1] = ((1 - e) - gn * (1 + e)) * 0.5;
                o[2] = 0;
                o[4] = 0;
            }
            n = 1;
        } else {
            int ns = sl == 1 ? 1 : (sl == 2 ? 2 : 4);
            for (int i = 0; i < ns; ++i) {
                double qq = sl == 1 ? q : (sl == 2 ? QT[i] : QT[2 + i]);
                if (i == ns - 1 && sl >= 2) qq = emax(0.1, qq * (q / 0.7071068));
                if (!(!hp && m3(3, 1, qq, w, o + i * 5))) {
                    double cw = std::cos(w), sw = std::sin(w), al = sw / (2 * qq), a0 = 1 + al;
                    if (hp) { o[i * 5] = (1 + cw) * (0.5 / a0); o[i * 5 + 1] = -(1 + cw) / a0; }
                    else { o[i * 5] = (1 - cw) * (0.5 / a0); o[i * 5 + 1] = (1 - cw) / a0; }
                    o[i * 5 + 2] = o[i * 5];
                    o[i * 5 + 3] = -2 * (cw / a0);
                    o[i * 5 + 4] = (1 - al) / a0;
                }
            }
            n = ns;
        }
    }
    return n;
}

int coefBw(int type, double f, double g, double q, double qm, double *o, double srate)
{
    double w = 2 * PI * (std::min(f, srate * 0.49) / srate);
    q = emax(0.05, q);
    q = qe(type, g, q, qm);
    int n = 0;
    if (type <= 2 && std::fabs(g) >= 0.001) {
        double A = epow(10, std::fabs(g) / 40);
        bool ok = m3(type == 0 ? 0 : 1, A, q, w, o);
        if (ok) {
            if (type == 0) { if (g < 0) inv(o); }
            if (type == 1) { if (g < 0) inv(o); }
            if (type == 2) {
                A *= A;
                if (g >= 0) { inv(o); o[0] *= A; o[1] *= A; o[2] *= A; }
                else { o[0] /= A; o[1] /= A; o[2] /= A; }
            }
        } else {
            rbj(type, w, g, q, o);
        }
        n = 1;
    }
    if (type == 3) {
        // notch: zeros exactly at the centre; the pole radius is the analog's and the pole angle makes the Nyquist gain the analog's
        double cw = std::cos(w), c1 = PI / w;
        c1 *= c1;
        double qq = q * q, e = std::exp(-w / q);
        double gn = std::sqrt(sqr(1 - c1) / (sqr(1 - c1) + c1 / qq));
        double c2 = (1 + cw) / (1 - cw);
        o[3] = (gn - c2) * ((1 + e) / (c2 + gn));
        o[4] = e;
        o[0] = (1 + o[3] + o[4]) / (2 - 2 * cw);
        o[1] = -2 * o[0] * cw;
        o[2] = o[0];
        if (!(e < 0.9999 && std::fabs(o[3]) < 1 + e - 0.000001 && o[0] > 0.000000001)) rbj(3, w, 0, q, o);
        n = 1;
    }
    if (type == 4) {
        if (!m3(2, 1, q, w, o)) rbj(4, w, 0, q, o);
        n = 1;
    }
    if (type >= 5) {
        // Butterworth: an odd order starts with a first-order section, then a biquad per pole pair
        bool hp = cutHp(type);
        int np = cutNp(cutS(type));
        if (np & 1) {
            if (hp) {
                double k = std::tan(w * 0.5);
                o[0] = 1 / (1 + k); o[1] = -o[0]; o[2] = 0; o[3] = (k - 1) / (1 + k); o[4] = 0;
            } else {
                // one pole, DC gain 1, Nyquist gain the analog's
                double e = std::exp(-w), gn = 1 / std::sqrt(1 + sqr(PI / w));
                o[3] = -e;
                o[0] = ((1 - e) + gn * (1 + e)) * 0.5;
                o[1] = ((1 - e) - gn * (1 + e)) * 0.5;
                o[2] = 0;
                o[4] = 0;
            }
            n = 1;
        }
        for (int i = 0; i < np / 2; ++i) {
            double qq = 0.5 / std::cos(PI * ((2 * i + 1 + (np & 1)) / (2.0 * np)));
            if (!(!hp && m3(3, 1, qq, w, o + n * 5))) {
                double cw = std::cos(w), sw = std::sin(w), al = sw / (2 * qq), a0 = 1 + al;
                if (hp) { o[n * 5] = (1 + cw) * (0.5 / a0); o[n * 5 + 1] = -(1 + cw) / a0; }
                else { o[n * 5] = (1 - cw) * (0.5 / a0); o[n * 5 + 1] = (1 - cw) / a0; }
                o[n * 5 + 2] = o[n * 5];
                o[n * 5 + 3] = -2 * (cw / a0);
                o[n * 5 + 4] = (1 - al) / a0;
            }
            n += 1;
        }
    }
    return n;
}

double mag2(const double *o, int n, double cw, double c2)
{
    double p = 1;
    for (int i = 0; i < n * 5; i += 5) {
        double b0 = o[i], b1 = o[i + 1], b2 = o[i + 2], a1 = o[i + 3], a2 = o[i + 4];
        double nu = b0 * b0 + b1 * b1 + b2 * b2 + 2 * (b0 * b1 + b1 * b2) * cw + 2 * b0 * b2 * c2;
        double de = 1 + a1 * a1 + a2 * a2 + 2 * (a1 + a1 * a2) * cw + 2 * a2 * c2;
        p *= nu / emax(0.000000000001, de);
    }
    return p;
}

} // namespace flt
} // namespace ts
