#include "presets.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace ts {

std::string presetStr(std::initializer_list<std::pair<int, double>> kv) { return presetStr(std::vector<std::pair<int, double>>(kv)); }
std::string presetStr(const std::vector<std::pair<int, double>> &kv)
{
    std::string s;
    char b[48];
    for (const auto &p : kv) {
        snprintf(b, sizeof b, "%s%d=%.10g", s.empty() ? "" : " ", p.first, p.second);
        s += b;
    }
    return s;
}

std::vector<std::pair<int, double>> parsePresetString(const std::string &s)
{
    std::vector<std::pair<int, double>> out;
    const char *p = s.c_str();
    while (*p) {
        while (*p && !(*p == '-' || *p == '.' || (*p >= '0' && *p <= '9'))) ++p;
        if (!*p) break;
        char *e = nullptr;
        long id = strtol(p, &e, 10);
        if (e == p) { ++p; continue; }
        p = e;
        if (*p != '=') continue;
        ++p;
        double v = strtod(p, &e);
        if (e == p) continue;
        p = e;
        if (id >= 1 && id < 256) out.push_back({(int)id, v});
    }
    return out;
}

static void mkdirs(const std::string &path)
{
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if ((path[i] == '/' || path[i] == '\\') && i > 0) {
#ifdef _WIN32
            _mkdir(cur.c_str());
#else
            mkdir(cur.c_str(), 0755);
#endif
        }
    }
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

std::string PresetStore::defaultDir()
{
#ifdef _WIN32
    const char *a = getenv("APPDATA");
    std::string base = a ? a : ".";
    return base + "\\TwoStorms\\Presets";
#else
    const char *x = getenv("XDG_CONFIG_HOME");
    std::string base;
    if (x && *x) base = x;
    else {
        const char *h = getenv("HOME");
        base = std::string(h ? h : ".") + "/.config";
    }
    return base + "/TwoStorms/Presets";
#endif
}

void PresetStore::setup(const std::string &plugName, const std::string &resourcePath)
{
    name_ = plugName;
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    if (!resourcePath.empty()) dir_ = resourcePath + sep + "Data" + sep + "TwoStorms" + sep + "Presets";
    else dir_ = defaultDir();
}

std::string PresetStore::file() const
{
#ifdef _WIN32
    return dir_ + "\\TwoStorms " + name_ + ".txt";
#else
    return dir_ + "/TwoStorms " + name_ + ".txt";
#endif
}

static std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

static std::string lower(std::string s)
{
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// the helper's clean_name: no "|", tabs or line breaks; nothing at the start that would make a menu
// command (spaces, # ! < >); no trailing spaces; at most 63 bytes (never cutting a UTF-8 character)
std::string PresetStore::cleanName(const std::string &in)
{
    std::string s = in;
    for (auto &c : s) if (c == '|' || c == '\r' || c == '\n' || c == '\t') c = ' ';
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '#' || s[a] == '!' || s[a] == '<' || s[a] == '>')) ++a;
    s = s.substr(a);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\f' || s.back() == '\v')) s.pop_back();
    if (s.size() > 63) {
        size_t n = 63;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
        s.resize(n);
    }
    return s;
}

static bool isNumChar(char c) { return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E'; }

std::vector<UserPreset> PresetStore::load() const
{
    std::vector<UserPreset> out;
    FILE *f = fopen(file().c_str(), "rb");
    if (!f) return out;
    std::string all;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    fclose(f);
    UserPreset *cur = nullptr;
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t e = all.find('\n', pos);
        if (e == std::string::npos) e = all.size();
        std::string l = all.substr(pos, e - pos);
        pos = e + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        // [name] (anything after "]" but spaces makes it not a name)
        if (!l.empty() && l[0] == '[') {
            size_t r = l.rfind(']');
            bool ok = r != std::string::npos;
            for (size_t i = r + 1; ok && i < l.size(); ++i) if (l[i] != ' ' && l[i] != '\t') ok = false;
            if (ok) {
                std::string nm = cleanName(l.substr(1, r - 1));
                if (nm.empty()) { cur = nullptr; continue; }
                out.push_back(UserPreset());
                cur = &out.back();
                cur->name = nm;
                continue;
            }
        }
        if (!cur) continue;
        std::string t = trim(l);
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
        bool digits = !k.empty();
        for (char c : k) if (c < '0' || c > '9') digits = false;
        if (digits) {
            bool numv = !v.empty();
            for (char c : v) if (!isNumChar(c)) numv = false;
            char *end = nullptr;
            double d = numv ? strtod(v.c_str(), &end) : 0;
            if (numv && end && *end == 0) {
                long id = strtol(k.c_str(), nullptr, 10);
                cur->values.push_back({(int)id, d});
            }
        } else if (k == "extra") {
            const char *q = v.c_str();
            while (*q) {
                while (*q && !isNumChar(*q)) ++q;
                if (!*q) break;
                const char *st = q;
                while (*q && isNumChar(*q)) ++q;
                std::string tok(st, q);
                char *end = nullptr;
                double d = strtod(tok.c_str(), &end);
                if (end && *end == 0) cur->extra.push_back(d);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const UserPreset &a, const UserPreset &b) { return lower(a.name) < lower(b.name); });
    if ((int)out.size() > MAXN) out.resize(MAXN);
    return out;
}

// the helper's number format: whole numbers as integers, others with 10 significant digits
static std::string num(double v)
{
    char b[64];
    if (v == std::floor(v) && std::fabs(v) < 1e15) snprintf(b, sizeof b, "%lld", (long long)v);
    else snprintf(b, sizeof b, "%.10g", v);
    return b;
}

bool PresetStore::write(const std::vector<UserPreset> &all) const
{
    mkdirs(dir_);
    std::string tmp = file() + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "# TwoStorms %s: user presets, written by the TwoStorms helper script.\n", name_.c_str());
    fprintf(f, "# Each preset: [its name], then setting = value (setting numbers are the plug-in's slider numbers).\n");
    for (auto &p : all) {
        fprintf(f, "\n[%s]\n", p.name.c_str());
        for (auto &kv : p.values) fprintf(f, "%d=%s\n", kv.first, num(kv.second).c_str());
        if (!p.extra.empty()) {
            std::string t;
            for (size_t i = 0; i < p.extra.size(); ++i) { if (i) t += ' '; t += num(p.extra[i]); }
            fprintf(f, "extra=%s\n", t.c_str());
        }
    }
    bool ok = fclose(f) == 0;
    if (!ok) return false;
#ifdef _WIN32
    return MoveFileExA(tmp.c_str(), file().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(tmp.c_str(), file().c_str()) == 0;
#endif
}

bool PresetStore::exists(const std::string &name) const
{
    std::string n = lower(cleanName(name));
    for (auto &x : load())
        if (lower(x.name) == n) return true;
    return false;
}

bool PresetStore::save(const UserPreset &in, bool *full)
{
    if (full) *full = false;
    UserPreset p = in;
    p.name = cleanName(p.name);
    if (p.name.empty()) return false;
    auto all = load();
    bool found = false;
    for (auto &x : all)
        if (lower(x.name) == lower(p.name)) { x = p; found = true; }
    if (!found) {
        if ((int)all.size() >= MAXN) { if (full) *full = true; return false; }
        all.push_back(p);
    }
    std::stable_sort(all.begin(), all.end(), [](const UserPreset &a, const UserPreset &b) { return lower(a.name) < lower(b.name); });
    return write(all);
}

bool PresetStore::remove(const std::string &name)
{
    auto all = load();
    std::vector<UserPreset> keep;
    for (auto &x : all)
        if (lower(x.name) != lower(name)) keep.push_back(x);
    return write(keep);
}

} // namespace ts
