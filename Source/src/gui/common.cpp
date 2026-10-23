#include "common.h"
#include "../core/clock.h"
#include "../core/eel.h"
#include "../core/plugin.h"
#include "../core/shm.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>

namespace ts {
namespace gui {

void logo(Ui &ui, float x, float y, float sz)
{
    Canvas &c = ui.cv();
    // a lightning bolt in a rounded square
    ui.shadow(R(x, y, sz, sz), sz * 0.28f, sz * 0.35f, th::orange().alpha(0.22f));
    ui.fill(R(x, y, sz, sz), Paint::vertical(y, th::orangeHi(), y + sz, th::orangeLo()), sz * 0.28f);
    float pts[][2] = {{0.58f, 0.14f}, {0.27f, 0.56f}, {0.47f, 0.56f}, {0.40f, 0.88f}, {0.74f, 0.42f}, {0.53f, 0.42f}};
    c.beginPath();
    for (int i = 0; i < 6; ++i) {
        float px = ui.X(x + pts[i][0] * sz), py = ui.Y(y + pts[i][1] * sz);
        if (i == 0) c.moveTo(px, py);
        else c.lineTo(px, py);
    }
    c.fillPath(Paint(th::orangeInk()));
}

bool tunerMuted(const ActView &av)
{
    return av.myslot >= 0 && now_sec() - shm::slot(av.myslot).tmute.load(std::memory_order_relaxed) < 0.3;
}

// ---------------------------------------------------------------- presets
void presetsMenu(Ui &ui, Plugin &p, float x, float y)
{
    const auto &fac = p.K.factory();
    std::vector<MenuItem> top;
    // factory presets, in submenus by their path ("Instruments/Guitar/Clean: ..."), in the order they're listed
    struct Node {
        std::string name;
        std::vector<Node> kids;
        int id = -1;
    };
    Node root;
    for (size_t i = 0; i < fac.size(); ++i) {
        const std::string &path = fac[i].path;
        Node *n = &root;
        size_t a = 0;
        for (;;) {
            // a '/' starts a submenu only before a capital ("Instruments/Guitar/..."): "1/16", "dry/wet" stay in the name
            size_t sl = a;
            while ((sl = path.find('/', sl)) != std::string::npos && !(sl + 1 < path.size() && path[sl + 1] >= 'A' && path[sl + 1] <= 'Z')) ++sl;
            if (sl == std::string::npos) {
                Node leaf;
                leaf.name = path.substr(a);
                leaf.id = 1000 + (int)i;
                n->kids.push_back(leaf);
                break;
            }
            std::string g = path.substr(a, sl - a);
            Node *f = nullptr;
            for (auto &k : n->kids)
                if (k.id < 0 && k.name == g) { f = &k; break; }
            if (!f) {
                Node nn;
                nn.name = g;
                n->kids.push_back(nn);
                f = &n->kids.back();
            }
            n = f;
            a = sl + 1;
        }
    }
    std::function<std::vector<MenuItem>(const Node &)> build = [&](const Node &n) {
        std::vector<MenuItem> m;
        for (const auto &k : n.kids) m.push_back(k.id >= 0 ? MenuItem::item(k.name, k.id) : MenuItem::submenu(k.name, build(k)));
        return m;
    };
    top = build(root);
    if (!top.empty()) top.push_back(MenuItem::sep());
    std::vector<UserPreset> users = p.presetUserList();
    std::vector<MenuItem> um, dm;
    for (size_t i = 0; i < users.size(); ++i) {
        um.push_back(MenuItem::item(users[i].name, 2000 + (int)i));
        dm.push_back(MenuItem::item(users[i].name, 3000 + (int)i));
    }
    if (!um.empty()) top.push_back(MenuItem::submenu("User presets", um));
    else top.push_back(MenuItem::item("User presets: none saved yet", 0, false, false));
    top.push_back(MenuItem::item("Save these settings as a user preset\xE2\x80\xA6", 1));
    if (!dm.empty()) top.push_back(MenuItem::submenu("Delete a user preset", dm));
    top.push_back(MenuItem::sep());
    top.push_back(MenuItem::item("Reset to defaults", 2));
    Plugin *pp = &p;
    Ui *pu = &ui;
    ui.menu(top, x, y, [pp, pu, users](int id) {
        if (id >= 1000 && id < 2000) pp->presetFactory(id - 1000);
        else if (id >= 2000 && id < 3000 && id - 2000 < (int)users.size()) pp->presetUserApply(users[id - 2000].name);
        else if (id >= 3000 && id - 3000 < (int)users.size()) pp->presetUserDelete(users[id - 3000].name);
        else if (id == 1) {
            pu->prompt("Save these settings as a user preset", "", [pp, pu](const std::string &name) {
                std::string nm = PresetStore::cleanName(name);
                if (nm.empty()) return;
                if (pp->presetUserExists(nm))
                    pu->confirm("Replace the preset \"" + nm + "\"?", "Replace", [pp, nm]() { pp->presetUserSave(nm); });
                else pp->presetUserSave(nm);
            });
        } else if (id == 2) pp->presetReset();
    }, 230);
}

// ---------------------------------------------------------------- top bar
R topBar(Ui &ui, Plugin &p, ActView &av, CommonState &st, float W, const char *t1, const char *t2, bool hasAB, int extra)
{
    if (!ui.drawS()) return gearRect(W);
    const float H = 56;
    ui.fill(R(0, 0, W, H), Paint::vertical(0, Col::hex(0x141317), H, Col::hex(0x0d0d10)));
    ui.fill(R(0, H - 1, W, 1), Paint(th::border()));
    // a thin orange line along the very top
    ui.fill(R(0, 0, W, 2), Paint::linear(0, 0, th::orange().alpha(0.0f), W * 0.5f, 0, th::orange().alpha(0.55f)));
    logo(ui, 14, 12, 30);
    float tx = 54;
    ui.textAt(tx, 27, t1, th::orange(), F_BOLD, 15.5f);
    float w1 = ui.textW(F_BOLD, 15.5f, t1);
    ui.textAt(tx + w1 + 6, 27, t2, th::text(), F_BOLD, 15.5f);
    float tend = tx + w1 + 6 + ui.textW(F_BOLD, 15.5f, t2);
    int fin = av.fin();
    if (fin) ui.badge(tend + 10, 14, "FINALE", th::orange(), th::orange().alpha(0.16f));
    // under the name: the track, the latency ... or, while a TwoStorms Tuner listening to this plug-in has its Mute
    // on, just that
    if (tunerMuted(av)) ui.text(R(tx, 34, 400, 14), "MUTED BY TUNER", th::red(), F_SEMIBOLD, 11);
    else ui.textFit(R(tx, 34, 400, 14), st.subtitle.c_str(), th::faint(), F_REGULAR, 11);

    // right side, from the right edge
    float y = 14, h = 28;
    float x = W - 14;
    R gear(x - 32, y, 32, h);
    x -= 32 + 6;
    bool hasByp = (extra & TB_BYPASS) != 0;
    R byp(hasByp ? x - 132 : x, y, hasByp ? 132 : 0, h);
    if (hasByp) x -= 132 + 6;
    R swp(hasAB ? x - 122 : x, y, hasAB ? 122 : 0, h);
    if (hasAB) x -= 122 + 12;
    R copy(x - 86, y, 86, h);
    R bb(copy.x - 6 - 32, y, 32, h), ba(bb.x - 32, y, 32, h);
    R pre((hasAB ? ba.x - 12 : x) - 104, y, 104, h);

    if ((extra & TB_PRESETS) && ui.dropdown(pre, "Presets")) presetsMenu(ui, p, pre.x, pre.b() + 4);
    if (hasAB) {
        bool b = p.K.abParam > 0 && p.get(p.K.abParam) >= 0.5;
        // A / B as one segmented control
        if (ui.button(ba, "A", !b)) { if (b) { p.abSwap(); if (ui.mods & M_SHIFT) st.abm = true; } }
        if (ui.button(bb, "B", b)) { if (!b) { p.abSwap(); if (ui.mods & M_SHIFT) st.abm = true; } }
        if (st.abm && !ui.ldown) { p.abSwap(); st.abm = false; }
        if (ui.button(copy, b ? "Copy B \xE2\x86\x92 A" : "Copy A \xE2\x86\x92 B")) p.abCopy();
        if (ui.button(swp, "Swap (Lvl Match)", st.holdNow() == 2, true, BS_PURPLE)) {
            if (ui.mods & M_SHIFT) st.holdm = 2;
            else st.hl = st.hl == 2 ? 0 : 2;
        }
    }
    if (hasByp && ui.button(byp, "Bypass (Lvl Match)", st.holdNow() == 1, true, BS_PURPLE)) {
        if (ui.mods & M_SHIFT) st.holdm = 1;
        else st.hl = st.hl == 1 ? 0 : 1;
    }
    if (!ui.ldown) st.holdm = 0;
    if (ui.button(gear, "", st.settings, true, BS_NORMAL, IC_GEAR)) st.settings = !st.settings;
    // a note from the presets for a few seconds
    if (now_sec() - p.presetMsgT < 4)
        ui.text(R(pre.x - 220, 40, 210, 14), p.presetMsg.c_str(), th::orange(), F_MEDIUM, 11, 2);
    return gear;
}

R gearRect(float W) { return R(W - 14 - 32, 14, 32, 28); }
R settingsRect(const R &gear, float w, float h) { return R(gear.r() - w, gear.b() + 8, w, h); }

bool settingsBegin(Ui &ui, CommonState &st, const R &gear, float w, float h, R &inner)
{
    ui.blockOn = false;
    if (!st.settings) return false;
    R r = settingsRect(gear, w, h);
    // a click outside closes it (the click still does what it does there); so does the gear
    if (ui.lclick && !ui.busy() && !ui.hitRaw(r) && !ui.hitRaw(gear)) {
        st.settings = false;
        return false;
    }
    ui.shadow(r, 10, 24, Col(0, 0, 0, 0.75f));
    ui.fill(r, Paint::vertical(r.y, Col::hex(0x1a191d), r.b(), Col::hex(0x131215)), 10);
    ui.stroke(r, Paint(th::borderHi()), 10);
    // the little arrow under the gear
    Canvas &c = ui.cv();
    float ax = gear.x + gear.w * 0.5f;
    c.beginPath();
    c.moveTo(ui.X(ax - 7), ui.Y(r.y + 0.5f));
    c.lineTo(ui.X(ax), ui.Y(r.y - 7));
    c.lineTo(ui.X(ax + 7), ui.Y(r.y + 0.5f));
    c.fillPath(Paint(Col::hex(0x1a191d)));
    inner = R(r.x + 16, r.y + 14, r.w - 32, r.h - 28);
    return true;
}

void settingsEnd(Ui &) {}

// ---------------------------------------------------------------- meters
float meterFrac(double db, double range) { return (float)((std::min(6.0, std::max(-range, db)) + range) / (range + 6)); }

static void zoneFill(Ui &ui, const R &r, float f, float bright, double range, bool vertical)
{
    float fz1 = meterFrac(-12, range), fz2 = meterFrac(-3, range);
    auto seg = [&](float a, float b, const Col &c0, const Col &c1) {
        if (b <= a) return;
        if (vertical) {
            float y0 = r.b() - r.h * b, y1 = r.b() - r.h * a;
            ui.fill(R(r.x, y0, r.w, y1 - y0), Paint::vertical(y0, c1, y1, c0));
        } else {
            ui.fill(R(r.x + r.w * a, r.y, r.w * (b - a), r.h), Paint::linear(r.x + r.w * a, 0, c0, r.x + r.w * b, 0, c1));
        }
    };
    Col g0 = th::greenLo(), g1 = th::green(), a0 = th::amber(), a1 = th::orange(), r0 = th::orange(), r1 = th::red();
    if (bright != 1) { g0 = g0.alpha(bright); g1 = g1.alpha(bright); a0 = a0.alpha(bright); a1 = a1.alpha(bright); r0 = r0.alpha(bright); r1 = r1.alpha(bright); }
    seg(0, std::min(f, fz1), g0, g1);
    if (f > fz1) seg(fz1, std::min(f, fz2), a0, a1);
    if (f > fz2) seg(fz2, f, r0, r1);
}

void hMeter(Ui &ui, const R &r, int s, int c, double tgt, bool active, double range)
{
    ui.fill(r, Paint(th::well()), 2);
    double step = range <= 36 ? 3 : 6;
    for (double d = 0; d >= -range; d -= step) {
        float f = meterFrac(d, range);
        ui.fill(R(r.x + r.w * f, r.y, 1.f / ui.s, r.h), Paint(d == 0 ? th::borderHi() : th::grid()));
    }
    if (s >= 0 && active) {
        shm::Slot &b = shm::slot(s);
        double pk = b.pkd[c].load(), rm = b.rms[c].load(), ph = b.pkh[c].load(), rh = b.rmh[c].load();
        zoneFill(ui, R(r.x, r.y + 1, r.w, r.h - 2), meterFrac(pk, range), 0.55f, range, false);
        zoneFill(ui, R(r.x, r.y + r.h * 0.28f, r.w, r.h * 0.44f), meterFrac(rm, range), 1, range, false);
        if (ph > -150) ui.fill(R(r.x + r.w * meterFrac(ph, range) - 1, r.y, 2, r.h), Paint(Col(1, 1, 1, 0.95f)));
        if (rh > -150) ui.fill(R(r.x + r.w * meterFrac(rh, range) - 1, r.y + 1, 2, r.h - 2), Paint(th::purpleHi()));
    }
    if (tgt > -150) ui.fill(R(r.x + r.w * meterFrac(tgt, range) - 1, r.y - 1, 2, r.h + 2), Paint(th::purple()));
}

void vMeter(Ui &ui, const R &r, double db, bool active, double range, double hold)
{
    ui.fill(r, Paint(th::well()), 2);
    if (active) {
        zoneFill(ui, R(r.x + 1, r.y + 1, r.w - 2, r.h - 2), meterFrac(db, range), 1, range, true);
        if (hold > -150) ui.fill(R(r.x + 1, r.y + 1 + (r.h - 2) * (1 - meterFrac(hold, range)) - 1, r.w - 2, 1.5f), Paint(Col(1, 1, 1, 0.9f)));
    }
}

void grMeter(Ui &ui, const R &r, double gr)
{
    ui.fill(r, Paint(th::well()), 2);
    float f = (float)std::min(1.0, std::max(0.0, -gr / 24));
    if (f > 0) ui.fill(R(r.x + 1, r.y + 1, r.w - 2, (r.h - 2) * f), Paint::vertical(r.y, th::red(), r.y + r.h, th::orange()));
}

// ---------------------------------------------------------------- the Act card
void driveMenu(Ui &ui, Plugin &p, ActView &av, float x, float y)
{
    std::vector<int> pts = av.chainPoints(av.myslot);
    double cv = p.get(P_DRIVE);
    std::vector<MenuItem> m;
    m.push_back(MenuItem::item("None", 1, cv < DRIVE_PARAM));
    std::vector<int64_t> ids;
    for (int s : pts) {
        int64_t id = shm::slot(s).owner.load();
        ids.push_back(id);
        m.push_back(MenuItem::item(av.pointTitle(s), 10 + (int)ids.size() - 1, id == (int64_t)cv));
    }
    m.push_back(MenuItem::item(p.inReaper() ? "Plug-in parameter: click here, then move the parameter" : "Plug-in parameter (needs REAPER)", 2,
                               iround(cv) == DRIVE_PARAM, p.inReaper()));
    if (cv >= DRIVE_PARAM) m.push_back(MenuItem::item("Engaged", 3, p.get(P_DRIVEON) >= 0.5));
    Plugin *pp = &p;
    ActView *pv = &av;
    ui.menu(m, x, y, [pp, pv, ids](int r) {
        if (r == 1) pv->setDrive(-1, 0);
        else if (r == 2) pv->setDrive(-1, DRIVE_PARAM);
        else if (r == 3) pp->setOnce(P_DRIVEON, pp->get(P_DRIVEON) < 0.5 ? 1 : 0);
        else if (r >= 10 && r - 10 < (int)ids.size()) pv->setDrive(-1, ids[r - 10]);
    });
}

// the ... menu: the same entries as the instance's card in the Gain Stage
void actMenu(Ui &ui, Plugin &p, ActView &av, float x, float y)
{
    int fm = iround(p.get(P_FINAL));
    std::vector<MenuItem> m;
    m.push_back(MenuItem::item("Finale: auto (the last Act in the chain)", 1, fm == FIN_AUTO));
    m.push_back(MenuItem::item("Finale: always", 2, fm == FIN_ALWAYS));
    m.push_back(MenuItem::item("Finale: never", 3, fm == FIN_NEVER));
    m.push_back(MenuItem::sep());
    // channels, links and used channels: the standalone Act only (a plug-in's Act is one stereo pair)
    if (p.K.subkind == shm::SK_ACT) {
        int nc = av.nch(-1), cur = iround(p.get(P_NCH));
        std::vector<MenuItem> chm, lkm, usm;
        char b[64];
        for (int i = 0; i < 4; ++i) { snprintf(b, sizeof b, "%d channels", i * 2 + 2); chm.push_back(MenuItem::item(b, 40 + i, cur == i)); }
        for (int i = 0; i < nc / 2; ++i) {
            snprintf(b, sizeof b, "Link %d-%d (one fader)", i * 2 + 1, i * 2 + 2);
            lkm.push_back(MenuItem::item(b, 50 + i, p.get(P_LINK + i) >= 0.5));
        }
        for (int i = 0; i < nc; ++i) { snprintf(b, sizeof b, "Use channel %d", i + 1); usm.push_back(MenuItem::item(b, 60 + i, p.get(P_USED + i) >= 0.5)); }
        snprintf(b, sizeof b, "Channels: %d", nc);
        m.push_back(MenuItem::submenu(b, chm));
        m.push_back(MenuItem::submenu("Links", lkm));
        m.push_back(MenuItem::submenu("Channels used", usm));
        m.push_back(MenuItem::sep());
    }
    double cv = p.get(P_DRIVE);
    m.push_back(MenuItem::item("Drive link: none", 10, cv < DRIVE_PARAM));
    std::vector<int64_t> ids;
    for (int s : av.chainPoints(av.myslot)) {
        int64_t id = shm::slot(s).owner.load();
        ids.push_back(id);
        m.push_back(MenuItem::item("Drive link: " + av.pointTitle(s), 20 + (int)ids.size() - 1, id == (int64_t)cv));
    }
    m.push_back(MenuItem::item("Drive link: plug-in parameter (click, then move it)", 11, iround(cv) == DRIVE_PARAM, p.inReaper()));
    if (cv >= DRIVE_PARAM) m.push_back(MenuItem::item("Drive link engaged", 12, p.get(P_DRIVEON) >= 0.5));
    m.push_back(MenuItem::sep());
    m.push_back(MenuItem::item("Clear peak holds", 13));
    Plugin *pp = &p;
    ActView *pv = &av;
    ui.menu(m, x, y, [pp, pv, ids](int r) {
        if (r >= 1 && r <= 3) pp->setOnce(P_FINAL, r - 1);
        else if (r >= 40 && r < 44) pp->setOnce(P_NCH, r - 40);
        else if (r >= 50 && r < 54) pp->setOnce(P_LINK + r - 50, pp->get(P_LINK + r - 50) < 0.5 ? 1 : 0);
        else if (r >= 60 && r < 68) pp->setOnce(P_USED + r - 60, pp->get(P_USED + r - 60) < 0.5 ? 1 : 0);
        else if (r == 10) pv->setDrive(-1, 0);
        else if (r == 11) pv->setDrive(-1, DRIVE_PARAM);
        else if (r == 12) pp->setOnce(P_DRIVEON, pp->get(P_DRIVEON) < 0.5 ? 1 : 0);
        else if (r == 13) pp->act.send(shm::C_RESETHOLD);
        else if (r >= 20 && r - 20 < (int)ids.size()) pv->setDrive(-1, ids[r - 20]);
    });
}

static const char *const MEAS[] = {"Peak", "RMS", "LUFS"};
static const char *const TIMES[] = {"Manual", "1 s", "2 s", "3 s", "5 s", "10 s", "20 s", "30 s"};
static const char *const MODES[] = {"Target", "Match", "Reference"};

static void actCardStill(Ui &ui, Plugin &p, ActView &av, const R &r);

float actCardHeight(ActView &av)
{
    int ga[8], gn[8];
    int ng = av.groups(-1, ga, gn);
    return 46 + std::max(1, ng) * 26 + 2;
}

// the card's layout, shared by the still and the moving parts
struct CardRow {
    int a, n;          // first channel, channels
    R lab, m1, m2, src, rp, rr, gl, gbar, gbox;
};
static int cardRows(ActView &av, const R &r, CardRow *rows)
{
    int ga[8], gn[8];
    int ng = av.groups(-1, ga, gn);
    bool srcRows = av.autoMode(-1) != 0 && ng > 1;
    float x = r.x + 12, y2 = r.y + 10 + 26 + 10;
    float mw = srcRows ? 290 : 380;
    for (int g = 0; g < ng; ++g) {
        CardRow &c = rows[g];
        c.a = ga[g];
        c.n = gn[g];
        float y = y2 + g * 26;
        c.lab = R(x, y, 30, 22);
        if (c.n == 2) { c.m1 = R(x + 30, y + 1, mw, 9); c.m2 = R(x + 30, y + 12, mw, 9); }
        else { c.m1 = R(x + 30, y + 1, mw, 20); c.m2 = R(); }
        c.src = srcRows ? R(x + 30 + mw + 8, y, 82, 22) : R(x + 30 + mw, y, 0, 22);
        c.rp = R(c.src.r() + 12, y, 78, 22);
        c.rr = R(c.rp.r() + 4, y, 84, 22);
        c.gl = R(c.rr.r() + 14, y, 46, 22);
        c.gbox = R(r.r() - 12 - 90, y, 90, 22);
        c.gbar = R(c.gl.r() + 2, y, c.gbox.x - 6 - c.gl.r() - 2, 22);
    }
    return ng;
}

void actCard(Ui &ui, Plugin &p, ActView &av, const R &r)
{
    bool me = av.myslot >= 0 && av.live(av.myslot);
    actCardStill(ui, p, av, r);
    if (!ui.drawD()) return;
    // the moving parts: meters and the PK / RMS holds
    CardRow rows[8];
    int ng = cardRows(av, r, rows);
    double tgt = av.aimTick(-1), range = av.meterRange();
    for (int g = 0; g < ng; ++g) {
        CardRow &c = rows[g];
        hMeter(ui, c.m1, av.myslot, c.a, tgt, me, range);
        if (c.n == 2) hMeter(ui, c.m2, av.myslot, c.a + 1, tgt, me, range);
        double pk = -200, rm = -200;
        bool clip = false;
        if (me) {
            shm::Slot &b = shm::slot(av.myslot);
            for (int k = c.a; k < c.a + c.n; ++k) {
                pk = std::max(pk, b.pkh[k].load());
                rm = std::max(rm, b.rmh[k].load());
                if (b.clip[k].load() > 0) clip = true;
            }
        }
        ui.fill(c.rp, Paint(clip ? Col::hex(0x3a0d12) : th::well()), 4);
        ui.stroke(c.rp, Paint(clip ? th::red() : th::border()), 4);
        ui.fill(c.rr, Paint(th::well()), 4);
        ui.stroke(c.rr, Paint(th::border()), 4);
        char b1[32], b2[32];
        if (pk <= -150) snprintf(b1, sizeof b1, "-inf"); else snprintf(b1, sizeof b1, "%.1f", pk);
        if (rm <= -150) snprintf(b2, sizeof b2, "-inf"); else snprintf(b2, sizeof b2, "%.1f", rm);
        ui.text(R(c.rp.x + 7, c.rp.y, 30, c.rp.h), "PK", th::faint(), F_SEMIBOLD, 10);
        ui.text(R(c.rp.x, c.rp.y, c.rp.w - 7, c.rp.h), b1, th::text(), F_MEDIUM, 11.5f, 2, true);
        ui.text(R(c.rr.x + 7, c.rr.y, 30, c.rr.h), "RMS", th::faint(), F_SEMIBOLD, 10);
        ui.text(R(c.rr.x, c.rr.y, c.rr.w - 7, c.rr.h), b2, th::purpleHi(), F_MEDIUM, 11.5f, 2, true);
    }
}

// Match / Reference: which of the source's channels group a listens to
static void srcMenu(Ui &ui, Plugin &p, ActView &av, const R &src, int a)
{
    int m = av.autoMode(-1);
    int sel = iround(p.get(P_MSRC + a));
    int srcSlot = m == 1 ? av.prevSlot(av.myslot) : av.findId((int64_t)p.get(P_REF));
    int pnc = srcSlot >= 0 ? av.nch(srcSlot) : 2;
    std::vector<MenuItem> mm;
    mm.push_back(MenuItem::item("Same Ch", 100, sel == 0));
    mm.push_back(MenuItem::item("All Ch (combined)", 109, sel == 9));
    const char *who = m == 1 ? "Previous Act" : "Reference";
    char b[64];
    for (int i = 0; i < pnc; ++i) { snprintf(b, sizeof b, "%s ch %d", who, i + 1); mm.push_back(MenuItem::item(b, 101 + i, sel == i + 1)); }
    for (int i = 0; i < pnc / 2; ++i) {
        snprintf(b, sizeof b, "%s ch %d-%d (average)", who, i * 2 + 1, i * 2 + 2);
        mm.push_back(MenuItem::item(b, 111 + i, sel == 11 + i));
    }
    Plugin *pp = &p;
    ui.menu(mm, src.x, src.b() + 4, [pp, a](int r2) { pp->setOnce(P_MSRC + a, r2 - 100); });
}

static void actCardStill(Ui &ui, Plugin &p, ActView &av, const R &r)
{
    bool me = av.myslot >= 0 && av.live(av.myslot);
    float x = r.x + 12, y = r.y + 10, h = 26;
    if (!ui.drawS()) return;
    CardRow rows[8];
    int ng = cardRows(av, r, rows);
    ui.shadow(r, 8, 12, Col(0, 0, 0, 0.45f));
    ui.fill(r, Paint::vertical(r.y, Col::hex(0x17161a), r.b(), Col::hex(0x111114)), 8);
    ui.stroke(r, Paint(th::border()), 8);
    // ---- line 1: drive link | ... | mode | value | source | measure | time | Auto
    bool dl = p.get(P_DRIVE) >= DRIVE_PARAM, don = p.get(P_DRIVEON) >= 0.5;
    if (ui.checkbox(R(x, y + 6, 14, 14), don && dl, dl)) p.setOnce(P_DRIVEON, don ? 0 : 1);
    std::string dlab = av.driveLabel(-1);
    R drv(x + 22, y, 244, h);
    if (ui.dropdown(drv, dlab.c_str(), av.myslot >= 0)) driveMenu(ui, p, av, drv.x, drv.b() + 4);
    if (dl && don) ui.stroke(drv, Paint(th::purple().alpha(0.8f)), 5);
    R dots(drv.r() + 6, y, 32, h);
    if (ui.button(dots, "", false, true, BS_NORMAL, IC_DOTS)) actMenu(ui, p, av, dots.x, dots.b() + 4);
    int m = av.autoMode(-1);
    R mode(dots.r() + 10, y, 104, h);
    if (ui.dropdown(mode, MODES[m])) {
        ui.menu({MenuItem::item("Target", 1, m == 0), MenuItem::item("Reference", 3, m == 2), MenuItem::item("Match", 2, m == 1)}, mode.x,
                mode.b() + 4, [&p](int r2) { p.setOnce(P_AMODE, r2 == 1 ? 0 : (r2 == 3 ? 2 : 1)); });
    }
    float rx = r.r() - 12;
    R autoB(rx - 70, y, 70, h);
    R tim(autoB.x - 6 - 76, y, 76, h);
    R meas(tim.x - 6 - 80, y, 80, h);
    // Target: its box; Match / Reference: the level and name, then (one group) the source channels
    float room = meas.x - 6 - (mode.r() + 6);
    R val(mode.r() + 6, y, m == 0 ? 120 : (ng == 1 ? room - 6 - 92 : room), h);
    if (m == 0) {
        char buf[32];
        snprintf(buf, sizeof buf, p.get(P_MEAS) >= 1.5 ? "%.1f LUFS" : "%.1f dBFS", p.get(P_TGT));
        ui.valueBox(val, P_TGT, true, KD_TARGET, 0.1, 0.01, 2, buf);
    } else {
        bool ok;
        std::string t = av.valueText(-1, &ok);
        ui.fill(val, Paint(th::well()), 4);
        ui.stroke(val, Paint(th::border()), 4);
        ui.textFit(R(val.x + 8, val.y, val.w - 16, val.h), t.c_str(), ok ? th::purple() : th::faint(), F_MEDIUM, 11.5f);
    }
    // one group: its source sits on line 1; several: on each group's row
    if (m != 0 && ng == 1) {
        R src(val.r() + 6, y, meas.x - 6 - val.r() - 6, h);
        std::string st = av.srcText(-1, rows[0].a, true);
        if (ui.dropdown(src, st.c_str())) srcMenu(ui, p, av, src, rows[0].a);
    }
    int mi = iclamp(iround(p.get(P_MEAS)), 0, 2);
    if (ui.dropdown(meas, MEAS[mi])) {
        ui.menu({MenuItem::item("Peak", 1, mi == 0), MenuItem::item("RMS", 2, mi == 1), MenuItem::item("LUFS", 3, mi == 2)}, meas.x,
                meas.b() + 4, [&p](int r2) { p.setOnce(P_MEAS, r2 - 1); });
    }
    bool busy, nosig;
    std::string tt = av.autoTimeText(-1, &busy, &nosig);
    if (busy) ui.button(tim, tt.c_str(), true);
    else if (nosig) ui.button(tim, "no signal", false, false);
    else {
        int ti = iclamp(iround(p.get(P_TIME)), 0, 7);
        if (ui.dropdown(tim, TIMES[ti])) {
            std::vector<MenuItem> mm;
            for (int i = 0; i < 8; ++i) mm.push_back(MenuItem::item(TIMES[i], i + 1, i == ti));
            ui.menu(mm, tim.x, tim.b() + 4, [&p](int r2) { p.setOnce(P_TIME, r2 - 1); });
        }
    }
    if (ui.button(autoB, av.autoLabel(-1), av.autoBusy(-1), av.myslot >= 0)) av.autoClick(-1);

    // ---- one row per gain group: meters (moving part), source, readouts (a click clears the holds), gain
    if (ng == 0)
        ui.text(R(x, r.y + 46, r.w - 24, 22), "Every channel is switched off (see \xE2\x80\xA6), so everything passes through untouched.", th::dim(),
                F_REGULAR, 12);
    bool single = p.K.subkind != shm::SK_ACT;   // a plug-in's Act: one fader, called Output
    for (int g = 0; g < ng; ++g) {
        CardRow &c = rows[g];
        char lb[32];
        if (c.n == 2) snprintf(lb, sizeof lb, "%d-%d", c.a + 1, c.a + 2); else snprintf(lb, sizeof lb, "%d", c.a + 1);
        ui.text(c.lab, lb, th::dim(), F_MEDIUM, 11);
        if (c.src.w > 0) {
            std::string st = av.srcText(-1, c.a, true);
            if (ui.dropdown(c.src, st.c_str())) srcMenu(ui, p, av, c.src, c.a);
        }
        if (me && ui.hit(R(c.rp.x, c.rp.y, c.rr.r() - c.rp.x, c.rp.h)) && ui.lclick && !ui.used) { ui.used = true; p.act.send(shm::C_RESETHOLD); }
        ui.text(c.gl, single ? "Output" : "Gain", th::dim(), F_REGULAR, 12);
        ui.bar(c.gbar, P_GAIN + c.a, SC_GAIN, true, KD_GAIN, true);
        ui.valueBox(c.gbox, P_GAIN + c.a, true, KD_GAIN, 0.1, 0.01);
    }
}

} // namespace gui
} // namespace ts
