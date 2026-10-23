#include "params.h"
#include "eel.h"
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>

namespace ts {

ParamTable::ParamTable()
{
    for (auto &d : defs_) d = nullptr;
}

ParamDef &ParamTable::add(int id, const char *name, double min, double max, double def, uint32_t flags,
                          FmtFn fmt, const char *module)
{
    ParamDef *d = new ParamDef();
    d->id = id;
    d->name = name;
    d->module = module;
    d->min = min;
    d->max = max;
    d->def = def;
    d->flags = flags;
    d->fmt = fmt;
    owned_.push_back(d);
    defs_[id] = d;
    order_.push_back(id);
    if (flags & PF_HOST) host_.push_back(id);
    if (flags & PF_AB) ab_.push_back(id);
    if (flags & (PF_AB | PF_PRESET)) pre_.push_back(id);
    return *d;
}

ParamDef &ParamTable::addEnum(int id, const char *name, std::vector<const char *> labels, double def,
                              uint32_t flags, const char *module)
{
    ParamDef &d = add(id, name, 0, (double)labels.size() - 1, def, flags | PF_INT | PF_ENUM, nullptr, module);
    d.labels = std::move(labels);
    return d;
}

double ParamTable::clamp(int id, double v) const
{
    const ParamDef *d = get(id);
    if (!d) return v;
    if (std::isnan(v)) v = d->def;
    v = v < d->min ? d->min : (v > d->max ? d->max : v);
    if (d->flags & PF_INT) v = std::floor(v + 0.5);
    return v;
}

int ParamTable::hostIndex(int id) const
{
    for (size_t i = 0; i < host_.size(); ++i)
        if (host_[i] == id) return (int)i;
    return -1;
}

void ParamTable::format(int id, double v, char *buf, int n) const
{
    const ParamDef *d = get(id);
    if (!d) { snprintf(buf, n, "%g", v); return; }
    if ((d->flags & PF_ENUM) && !d->labels.empty()) {
        int i = iclamp(iround(v), 0, (int)d->labels.size() - 1);
        snprintf(buf, n, "%s", d->labels[i]);
        return;
    }
    if (d->fmt) { d->fmt(v, buf, n); return; }
    if (d->flags & PF_BOOL) { snprintf(buf, n, "%s", v >= 0.5 ? "On" : "Off"); return; }
    if (d->flags & PF_INT) snprintf(buf, n, "%d", iround(v));
    else snprintf(buf, n, "%.2f", v);
}

// the first number in a text (sign, digits, a decimal point or comma, "k" right after it for thousands)
static bool firstNumber(const char *s, double *out)
{
    for (const char *p = s; *p; ++p) {
        if (!(*p >= '0' && *p <= '9') && !((*p == '.' || *p == ',') && p[1] >= '0' && p[1] <= '9')) continue;
        const char *q = p;
        double sgn = 1;
        if (q > s && (q[-1] == '-' || q[-1] == '+')) sgn = q[-1] == '-' ? -1 : 1;
        double ip = 0, fp = 0, dv = 1;
        bool dot = false;
        for (; *q; ++q) {
            if (*q >= '0' && *q <= '9') {
                if (dot) { dv *= 10; fp = fp * 10 + (*q - '0'); }
                else ip = ip * 10 + (*q - '0');
            } else if ((*q == '.' || *q == ',') && !dot) dot = true;
            else break;
        }
        double v = sgn * (ip + fp / dv);
        while (*q == ' ') ++q;
        if (*q == 'k' || *q == 'K') v *= 1000;
        *out = v;
        return true;
    }
    return false;
}

bool ParamTable::parse(int id, const char *s, double *out) const
{
    const ParamDef *d = get(id);
    if (d && d->tparse && d->tparse(s, out)) return true;
    if (d && (d->flags & PF_BOOL)) {
        if (!strcmp(s, "On") || !strcmp(s, "on")) { *out = 1; return true; }
        if (!strcmp(s, "Off") || !strcmp(s, "off")) { *out = 0; return true; }
    }
    if (d && (d->flags & PF_ENUM)) {
        for (size_t i = 0; i < d->labels.size(); ++i)
            if (!strcmp(s, d->labels[i])) { *out = (double)i; return true; }
    }
    if (d && d->minText && !strcmp(s, d->minText)) { *out = d->min; return true; }
    if (d && d->maxText && !strcmp(s, d->maxText)) { *out = d->max; return true; }
    // the text exactly as this setting writes a value: words around the number ("gate 40%", "L 30", "100 L")
    // say which way it goes; try the number, its opposite, and thousandths / thousands of it
    auto same = [&](double v) {
        char b[96];
        format(id, v, b, sizeof b);
        return !strcmp(b, s);
    };
    double x = 0;
    bool num = firstNumber(s, &x);
    if (d && num) {
        const double c[4] = {x, -x, x * 1000, x / 1000};
        for (double v : c)
            if (v >= d->min - 1e-9 && v <= d->max + 1e-9 && same(v)) { *out = v; return true; }
    }
    if (d && !num) {
        // a word for one value ("centre", "off"): the ends and the middle
        const double c[3] = {d->min, d->max, 0.5 * (d->min + d->max)};
        for (double v : c)
            if (same(v)) { *out = v; return true; }
        if (d->min <= 0 && d->max >= 0 && same(0)) { *out = 0; return true; }
    }
    // a whole-number setting with not too many values: the one that writes this text
    if (d && (d->flags & PF_INT) && d->max - d->min <= 200000) {
        for (double v = d->min; v <= d->max; v += 1)
            if (same(v)) { *out = v; return true; }
    }
    if (!parse_number(s, out)) {
        if (!num) return false;
        *out = x;
    }
    // "inf" means -inf for gains; for a value that can't go negative (a ratio) it means the top
    if (*out == -1000000 && d && d->min >= 0) *out = d->max;
    // seconds typed into a value kept in ms
    if (d && !strcmp(d->unit, "ms")) {
        std::string l;
        for (const char *p = s; *p; ++p) l += (char)tolower((unsigned char)*p);
        size_t k = l.find_last_not_of(' ');
        if (k != std::string::npos && l[k] == 's' && (k == 0 || l[k - 1] != 'm')) *out *= 1000;
    }
    return true;
}

ParamValues::ParamValues(const ParamTable &t) : t_(t)
{
    for (auto &x : v_) x.store(0.0, std::memory_order_relaxed);
    resetToDefaults();
}

void ParamValues::resetToDefaults()
{
    for (int id : t_.ids()) put(id, t_.def(id));
}

void fmt_db1(double v, char *b, int n) { snprintf(b, n, "%.1f dB", v); }
void fmt_db1s(double v, char *b, int n) { snprintf(b, n, "%+.1f dB", v); }
void fmt_db2s(double v, char *b, int n) { snprintf(b, n, "%+.2f dB", v); }
void fmt_gain(double v, char *b, int n)
{
    if (v <= -100) snprintf(b, n, "-inf dB");
    else snprintf(b, n, "%+.2f dB", v);
}
void fmt_pct0(double v, char *b, int n) { snprintf(b, n, "%.0f%%", v); }
void fmt_ms(double v, char *b, int n)
{
    if (v < 1) snprintf(b, n, "%.2f ms", v);
    else if (v < 10) snprintf(b, n, "%.1f ms", v);
    else if (v < 1000) snprintf(b, n, "%.0f ms", v);
    else snprintf(b, n, "%.2f s", v / 1000);
}
void fmt_hz(double v, char *b, int n)
{
    if (v >= 10000) snprintf(b, n, "%.1fk", v / 1000);
    else if (v >= 1000) snprintf(b, n, "%.2fk", v / 1000);
    else snprintf(b, n, "%.0f Hz", v);
}
void fmt_dbfs1(double v, char *b, int n) { snprintf(b, n, "%.1f dBFS", v); }
void fmt_int(double v, char *b, int n) { snprintf(b, n, "%d", iround(v)); }

// The JSFX's parse_num: digits, one dot, a leading sign, "k" multiplies by 1000, "inf" anywhere
// means -inf (returned as -1000000, the caller clamps), and unit letters are ignored.
bool parse_number(const char *s, double *out)
{
    double sgn = 1, ip = 0, fp = 0, dv = 1, kil = 1;
    bool seen_dot = false, seen_dig = false, ok = true, inf = false;
    // skip leading spaces so " -3" still has its sign first
    while (*s == ' ') ++s;
    for (int i = 0; s[i]; ++i) {
        char ch = s[i];
        if (ch == 'i' || ch == 'I') inf = true;
        else if (ch == '-' && i == 0) sgn = -1;
        else if (ch == '+' && i == 0) sgn = 1;
        else if (ch == '.' && !seen_dot) seen_dot = true;
        else if (ch == ',' && !seen_dot) seen_dot = true;   // decimal comma
        else if (ch >= '0' && ch <= '9') {
            seen_dig = true;
            if (seen_dot) { dv *= 10; fp = fp * 10 + (ch - '0'); }
            else ip = ip * 10 + (ch - '0');
        }
        else if (ch == 'k' || ch == 'K') kil = 1000;
        else if (strchr("nNfF hHzZmMsSdDbB%:", ch)) {}
        else ok = false;
    }
    if (inf) { *out = -1000000; return true; }
    if (!(ok && seen_dig)) return false;
    *out = sgn * (ip + fp / dv) * kil;
    return true;
}

} // namespace ts
