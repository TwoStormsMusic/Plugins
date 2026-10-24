// The TwoStorms filter design (the JSFX suite's ea_* functions, the same in the EQ, Chorus, Delay and Saturator),
// ported line for line. Every filter is a matched biquad: the poles come from the analog prototype (matched z) and
// the numerator is set so the magnitude equals the analog filter's at DC, at the centre frequency and at Nyquist.
// A cut is the exact inverse of the boost of the same shape. If a design is not valid, the classic RBJ filter is used.
// Sections are 5 numbers each (b0 b1 b2 a1 a2, a0 = 1).
#pragma once

namespace ts {
namespace flt {

// filter types (the EQ's band types): 0 bell, 1 low shelf, 2 high shelf, 3 notch, 4 band pass,
// 5..8 high pass (6, 12, 24, 48 dB/oct), 9..12 low pass (the same slopes)
enum { BELL = 0, LSHELF = 1, HSHELF = 2, NOTCH = 3, BANDPASS = 4, HP6 = 5, HP12 = 6, HP24 = 7, HP48 = 8, LP6 = 9, LP12 = 10, LP24 = 11, LP48 = 12 };

void inv(double *o);
// the matched design for analog prototypes (kind 0 bell, 1 low shelf boost, 2 band pass, 3 low pass); w in rad/sample;
// returns true and fills o[0..4] when valid
bool m3(int kind, double A, double Q, double w, double *o);
void rbj(int type, double w, double g, double q, double *o);
// the gain-dependent Q (qm 0 off, 1 light, 2 medium, 3 strong)
double qe(int type, double g, double q, double qm);
// a band's sections at o[0..19]; returns how many (0 = the band does nothing)
int coef(int type, double f, double g, double q, double qm, double *o, double srate);
// the EQ's version: its cuts are Butterworth at any order (types 5..16, see cutS), up to 8 sections at o[0..39]
int coefBw(int type, double f, double g, double q, double qm, double *o, double srate);
// the EQ's high and low passes: is type t a high pass, its slope (0..5 = 6, 12, 18, 24, 48, 96 dB/oct), its order,
// and the type for a slope (13 / 14 high pass 18 / 96, 15 / 16 low pass 18 / 96 came later)
inline bool cutHp(int t) { return (t >= 5 && t <= 8) || t == 13 || t == 14; }
inline int cutS(int t)
{
    if (t == 13 || t == 15) return 2;
    if (t == 14 || t == 16) return 5;
    int s = t <= 8 ? t - 5 : t - 9;
    return s + (s >= 2 ? 1 : 0);
}
inline int cutNp(int sl) { return sl <= 3 ? sl + 1 : (sl == 4 ? 8 : 16); }
inline int cutTy(bool hp, int sl) { return sl == 2 ? (hp ? 13 : 15) : (sl == 5 ? (hp ? 14 : 16) : (hp ? 5 : 9) + (sl < 2 ? sl : sl - 1)); }
// power response |H|^2 of n sections at cos(w) = cw, cos(2w) = c2
double mag2(const double *o, int n, double cw, double c2);

// one section running (direct form 2 transposed, as the JSFX runs them)
struct Section {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
};

} // namespace flt
} // namespace ts
