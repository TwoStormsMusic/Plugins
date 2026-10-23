#include "editor.h"
#include "../core/act.h"
#include "../core/clock.h"
#include "../core/eel.h"
#include "../core/plugin.h"
#include "../core/shm.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ts {
namespace gui {

Editor::Editor(Plugin &p) : P(p) {}

Editor::~Editor()
{
    if (hooked_) destroy();
}

bool Editor::apiSupported(const char *api, bool floating)
{
    if (floating) return false;
#ifdef _WIN32
    return !strcmp(api, CLAP_WINDOW_API_WIN32);
#elif defined(__APPLE__)
    (void)api;
    return false;
#else
    return !strcmp(api, CLAP_WINDOW_API_X11);
#endif
}

const char *Editor::preferredApi()
{
#ifdef _WIN32
    return CLAP_WINDOW_API_WIN32;
#else
    return CLAP_WINDOW_API_X11;
#endif
}

void Editor::sizeFromScale(float sc, uint32_t *w, uint32_t *h)
{
    *w = (uint32_t)std::lround(designW_ * sc);
    *h = (uint32_t)std::lround(designH_ * sc);
}

#ifdef _WIN32
// REAPER asks extensions whether a window is a text field before it runs its keyboard shortcuts:
// while a TwoStorms value box is being typed into, the keys go to it.
void *win32HwndInfoHook();
static int s_hookUsers = 0;
#endif

bool Editor::create(const char *api, bool floating)
{
    if (!apiSupported(api, floating)) return false;
#ifdef _WIN32
    if (P.reaper.ok() && !hooked_) {
        hooked_ = true;
        if (s_hookUsers++ == 0) P.reaper.registerHook("hwnd_info", win32HwndInfoHook());
    }
#endif
    view_.reset(P.createView());
    if (!view_) return false;
    ui_.pa = this;
    free_ = view_->freeSize(baseW_, baseH_);
    if (free_) {
        // any size: the one it was left at, or the base
        view_->savedSize(lw_, lh_);
        if (lw_ < 1 || lh_ < 1) { lw_ = baseW_; lh_ = baseH_; }
        w_ = (int)std::lround(lw_ * hostScale_);
        h_ = (int)std::lround(lh_ * hostScale_);
        freeLayout();
        canvas_.resize(w_, h_);
        win_.reset(PlatformWindow::create(*this));
        return win_ != nullptr;
    }
    view_->designSize(designW_, designH_);
    // the shared zoom: another window may have changed it since this instance last opened
    shm::Header &H = shm::get()->h;
    int64_t zv = H.zoomv.load();
    if (zv > 0) P.putQuiet(P_ZOOM, (double)iclamp((int)H.zoom.load(), 0, 5));
    zoomSeen_ = iround(P.get(P_ZOOM));
    zoomV_ = zv;
    scale_ = (float)act_zoom(P.get(P_ZOOM)) * hostScale_;
    uint32_t w, h;
    sizeFromScale(scale_, &w, &h);
    w_ = (int)w;
    h_ = (int)h;
    canvas_.resize(w_, h_);
    ui_.pa = this;
    win_.reset(PlatformWindow::create(*this));
    return win_ != nullptr;
}

void Editor::destroy()
{
#ifdef _WIN32
    if (hooked_) {
        hooked_ = false;
        if (--s_hookUsers == 0) P.reaper.registerHook("-hwnd_info", win32HwndInfoHook());
    }
#endif
    if (timer_ != CLAP_INVALID_ID && P.hTimer_) P.hTimer_->unregister_timer(P.host, timer_);
    timer_ = CLAP_INVALID_ID;
#ifndef _WIN32
    if (win_ && win_->fd() >= 0 && P.hFd_) P.hFd_->unregister_fd(P.host, win_->fd());
#endif
    if (view_) view_->closed();
    win_.reset();
    view_.reset();
}

// a free-size view: the design fits the window's shorter side (relative to the base) and grows along the other
void Editor::freeLayout()
{
    if (w_ < 1 || h_ < 1) return;
    scale_ = std::min((float)w_ / baseW_, (float)h_ / baseH_);
    designW_ = (float)w_ / scale_;
    designH_ = (float)h_ / scale_;
    if (view_) view_->sizeChanged(lw_, lh_, designW_, designH_);
}

bool Editor::setScale(double s)
{
    hostScale_ = (float)std::max(0.5, std::min(4.0, s));
    if (free_) {
        w_ = (int)std::lround(lw_ * hostScale_);
        h_ = (int)std::lround(lh_ * hostScale_);
        freeLayout();
        canvas_.resize(w_, h_);
        if (win_) win_->setSize(w_, h_);
        stillOk_ = false;
        needFull_ = true;
        return true;
    }
    scale_ = (float)act_zoom(P.get(P_ZOOM)) * hostScale_;
    return true;
}

bool Editor::getSize(uint32_t *w, uint32_t *h)
{
    if (free_) {
        *w = (uint32_t)std::max(1, w_);
        *h = (uint32_t)std::max(1, h_);
        return true;
    }
    sizeFromScale(scale_, w, h);
    return true;
}

bool Editor::resizeHints(clap_gui_resize_hints *h)
{
    h->can_resize_horizontally = true;
    h->can_resize_vertically = true;
    if (free_) {
        h->preserve_aspect_ratio = false;
        h->aspect_ratio_width = 0;
        h->aspect_ratio_height = 0;
        return true;
    }
    h->preserve_aspect_ratio = true;
    h->aspect_ratio_width = (uint32_t)std::lround(designW_);
    h->aspect_ratio_height = (uint32_t)std::lround(designH_);
    return true;
}

bool Editor::adjustSize(uint32_t *w, uint32_t *h)
{
    if (free_) {
        // any shape, down to half the base size
        *w = std::max(*w, (uint32_t)std::lround(baseW_ * 0.5f * hostScale_));
        *h = std::max(*h, (uint32_t)std::lround(baseH_ * 0.5f * hostScale_));
        *w = std::min(*w, 16384u);
        *h = std::min(*h, 16384u);
        return true;
    }
    // keep the proportions; the width decides (between 70% and 300%)
    float sc = std::max(0.7f, std::min(3.0f, (float)*w / designW_));
    float sh = (float)*h / designH_;
    if (sh < sc) sc = std::max(0.7f, sh);
    sizeFromScale(sc, w, h);
    return true;
}

bool Editor::setSize(uint32_t w, uint32_t h)
{
    w_ = (int)w;
    h_ = (int)h;
    stillOk_ = false;
    if (free_) {
        lw_ = (float)w / hostScale_;
        lh_ = (float)h / hostScale_;
        freeLayout();
    } else {
        scale_ = std::min((float)w / designW_, (float)h / designH_);
    }
    canvas_.resize(w_, h_);
    if (win_) win_->setSize(w_, h_);
    needFull_ = true;
    return true;
}

bool Editor::setParent(const clap_window *w)
{
    if (!win_ || !win_->attach(w, w_, h_)) return false;
#ifndef _WIN32
    if (win_->fd() >= 0 && P.hFd_ && P.hFd_->register_fd) P.hFd_->register_fd(P.host, win_->fd(), CLAP_POSIX_FD_READ);
#endif
    if (!win_->ownTimer() && P.hTimer_ && P.hTimer_->register_timer) P.hTimer_->register_timer(P.host, 16, &timer_);
    return true;
}

bool Editor::show()
{
    visible_ = true;
    if (win_) win_->show(true);
    if (view_) view_->opened();
    needFull_ = true;
    render();
    return true;
}

bool Editor::hide()
{
    visible_ = false;
    if (win_) win_->show(false);
    if (view_) view_->closed();
    return true;
}

void Editor::onTimer(clap_id id)
{
    if (id == timer_) tick();
}

void Editor::onFd(int, clap_posix_fd_flags_t)
{
    if (win_) win_->pump();
    if (inputPending_) render();
}

// ---------------------------------------------------------------- events
void Editor::evMove(float x, float y) { in_.mx = x; in_.my = y; inputPending_ = true; }
void Editor::evLeave() { if (!(in_.buttons & 1)) { in_.mx = -1000; in_.my = -1000; } inputPending_ = true; }
void Editor::evButton(int b, bool down)
{
    if (down) { in_.buttons |= b; in_.pressed |= b; }
    else { in_.buttons &= ~b; in_.released |= b; }
    inputPending_ = true;
}
void Editor::evWheel(float dy, float dx) { in_.wheel += dy; in_.hwheel += dx; inputPending_ = true; }
void Editor::evMods(int m) { if (m != in_.mods) { in_.mods = m; inputPending_ = true; } }
void Editor::evKey(int k) { in_.keys.push_back(k); inputPending_ = true; }
void Editor::evChar(uint32_t c) { in_.chars.push_back(c); inputPending_ = true; }
void Editor::evDrop(const std::vector<std::string> &files, float px, float py)
{
    if (view_ && !files.empty()) view_->filesDropped(files, px / scale_, py / scale_);
    needFull_ = true;
}

void Editor::tick()
{
    if (win_) win_->pump();
    if (!visible_) return;
    double t = now_sec();
    // input: draw at once; otherwise the meters at 30 frames a second
    bool want = view_ && view_->poll(t);
    if (inputPending_ || needFull_ || ui_.changed || want || (view_ && t - lastFrame_ >= view_->frameInterval() && view_->animating()))
        render();
}

// the zoom is shared: changing it in one TwoStorms window changes it in every one
void Editor::zoomSync()
{
    if (free_) return;   // a free-size window keeps its own size
    shm::Header &H = shm::get()->h;
    int z = iround(P.get(P_ZOOM));
    int64_t zv = H.zoomv.load();
    if (zv > 0 && zv != zoomV_) {
        zoomV_ = zv;
        int gz = iclamp((int)H.zoom.load(), 0, 5);
        if (gz != z) { P.putQuiet(P_ZOOM, gz); z = gz; }
    } else if (zoomSeen_ >= 0 && z != zoomSeen_) {
        H.zoom.store(z);
        zoomV_ = H.zoomv.fetch_add(1) + 1;
    }
    if (z != zoomSeen_) {
        zoomSeen_ = z;
        applyZoom();
    }
}

uint64_t Editor::stateHash()
{
    uint64_t h = 1469598103934665603ULL;
    for (int id : P.T.ids()) {
        double v = P.get(id);
        uint64_t b;
        memcpy(&b, &v, 8);
        h = (h ^ b) * 1099511628211ULL;
    }
    h = (h ^ view_->statusHash()) * 1099511628211ULL;
    {
        int s = P.act.slotPub.load(std::memory_order_relaxed);
        bool tm = s >= 0 && now_sec() - shm::slot(s).tmute.load(std::memory_order_relaxed) < 0.3;
        h = (h ^ (uint64_t)tm) * 1099511628211ULL;
    }
    h = (h ^ (uint64_t)w_ << 20 ^ (uint64_t)h_) * 1099511628211ULL;
    return h;
}

void Editor::applyZoom()
{
    if (free_) return;
    float sc = (float)act_zoom(P.get(P_ZOOM)) * hostScale_;
    uint32_t w, h;
    sizeFromScale(sc, &w, &h);
    if ((int)w == w_ && (int)h == h_) return;
    if (P.hGui_ && P.hGui_->request_resize && P.hGui_->request_resize(P.host, w, h)) {
        // the host calls setSize
    } else {
        setSize(w, h);
    }
}

void Editor::render()
{
    if (!view_ || !win_ || w_ <= 0 || h_ <= 0) return;
    double t = now_sec();
    zoomSync();
    // a window whose height follows its contents (the Act's gain groups): ask the host to resize
    if (!free_) {
        float dw, dh;
        view_->designSize(dw, dh);
        if (dw != designW_ || dh != designH_) {
            designW_ = dw;
            designH_ = dh;
            applyZoom();
        }
    }
    P.guiOpenBeat.store(t);
    canvas_.resetClip();
    // fit the design into the window, centered
    float sc = std::min((float)w_ / designW_, (float)h_ / designH_);
    float ox = std::floor(((float)w_ - designW_ * sc) * 0.5f), oy = std::floor(((float)h_ - designH_ * sc) * 0.5f);
    bool full = ui_.busy() || ui_.dragging() || ui_.editKey != 0 || view_->wantsFull() || ui_.momKey != 0;
    uint64_t hsh = stateHash();
    bool needStill = !stillOk_ || inputPending_ || needFull_ || ui_.changed || hsh != lastHash_;
    size_t npx = (size_t)w_ * h_;
    if (full) {
        canvas_.clear(th::bg());
        ui_.changed = false;
        ui_.pass = Ui::PASS_ALL;
        ui_.beginFrame(canvas_, in_, t, sc, ox, oy);
        view_->frame(ui_);
        ui_.endFrame();
        stillOk_ = false;
    } else {
        if (needStill) {
            canvas_.clear(th::bg());
            ui_.changed = false;
            ui_.pass = Ui::PASS_STATIC;
            ui_.beginFrame(canvas_, in_, t, sc, ox, oy);
            view_->frame(ui_);
            ui_.endFrame();
            still_.assign(canvas_.pixels(), canvas_.pixels() + npx);
            stillOk_ = true;
            lastHash_ = stateHash();
        } else {
            // only the moving parts, when the view says where they are
            dynR_.clear();
            dynPx_.clear();
            if (view_->dynamicRegions(dynR_) && !dynR_.empty()) {
                for (const R &r : dynR_) {
                    int x0 = std::max(0, (int)std::floor(ox + r.x * sc) - 1), y0 = std::max(0, (int)std::floor(oy + r.y * sc) - 1);
                    int x1 = std::min(w_, (int)std::ceil(ox + r.r() * sc) + 1), y1 = std::min(h_, (int)std::ceil(oy + r.b() * sc) + 1);
                    if (x1 <= x0 || y1 <= y0) continue;
                    for (int y = y0; y < y1; ++y)
                        memcpy(canvas_.pixels() + (size_t)y * w_ + x0, still_.data() + (size_t)y * w_ + x0, (size_t)(x1 - x0) * 4);
                    dynPx_.insert(dynPx_.end(), {x0, y0, x1 - x0, y1 - y0});
                    if (getenv("TS_DYNDBG")) fprintf(stderr, "dyn %d %d %d %d\n", x0, y0, x1 - x0, y1 - y0);
                }
            } else {
                memcpy(canvas_.pixels(), still_.data(), npx * 4);
            }
        }
        ui_.pass = Ui::PASS_DYN;
        ui_.beginFrame(canvas_, in_, t, sc, ox, oy);
        view_->frame(ui_);
        ui_.endFrame();
        ui_.pass = Ui::PASS_ALL;
    }
    bool partial = !full && !needStill && !dynPx_.empty();
    if (full || needStill) dynPx_.clear();
    // the keyboard while typing
    bool kb = ui_.wantKeyboard();
    if (kb && !hadKeyboard_) win_->takeFocus();
    if (kb != hadKeyboard_) win_->textInput(kb);
    hadKeyboard_ = kb;
    if (ui_.cursor != cursor_) { cursor_ = ui_.cursor; win_->setCursor(cursor_); }
    static const bool prof = getenv("TS_PROFILE") != nullptr;
    if (prof) {
        static double acc = 0;
        static int cnt = 0;
        acc += now_sec() - t;
        if (++cnt == 60) { fprintf(stderr, "[ts] frame %.2f ms (%dx%d)\n", acc / cnt * 1000, w_, h_); acc = 0; cnt = 0; }
    }
    if (partial) win_->presentRects(canvas_.pixels(), w_, h_, dynPx_.data(), (int)dynPx_.size() / 4);
    else win_->present(canvas_.pixels(), w_, h_);
    // events of this frame are spent
    in_.pressed = 0;
    in_.released = 0;
    in_.wheel = 0;
    in_.hwheel = 0;
    in_.keys.clear();
    in_.chars.clear();
    inputPending_ = false;
    needFull_ = false;
    lastFrame_ = t;
}

// ---------------------------------------------------------------- parameters
// keys: id (< 256) is this instance; (slot + 2) * 256 + id is the instance in that slot
static inline int kslot(int k) { return k / 256 - 2; }
static inline int kp(int k) { return k % 256; }

double Editor::get(int key)
{
    if (key < 256 && P.vpTarget(key)) return P.vpGet(key);
    if (key < 256) return P.get(key);
    int s = kslot(key);
    return (s >= 0 && s < shm::NSLOT) ? shm::slot(s).p[kp(key)].load() : 0;
}

void Editor::begin(int key)
{
    if (key < 256 && P.vpTarget(key)) { P.beginEdit(P.vpTarget(key)); return; }
    if (key < 256 && !(key >= P_GAIN && key < P_GAIN + 8)) P.beginEdit(key);
    else if (key < 256) { P.beginEdit(key); if (key % 2 == 0) P.beginEdit(key + 1); else P.beginEdit(key - 1); }
}

void Editor::set(int key, double v)
{
    if (key < 256 && P.vpTarget(key)) { P.apply(P.vpTarget(key), P.vpPacked(key, P.T.clamp(key, v)), true); return; }
    if (key < 256) { P.apply(key, v, true); return; }
    int s = kslot(key), p = kp(key);
    v = P.T.clamp(p, v);
    shm::send(s, shm::C_SET, p, v, 0);
    shm::slot(s).p[p].store(v);
}

void Editor::end(int key)
{
    if (key < 256 && P.vpTarget(key)) { P.endEdit(P.vpTarget(key)); return; }
    if (key < 256 && !(key >= P_GAIN && key < P_GAIN + 8)) P.endEdit(key);
    else if (key < 256) { P.endEdit(key); if (key % 2 == 0) P.endEdit(key + 1); else P.endEdit(key - 1); }
}

double Editor::def(int key) { return key < 256 ? P.pdef(key) : P.T.def(kp(key)); }
double Editor::lo(int key) { const ParamDef *d = P.T.get(kp(key)); return d ? d->min : 0; }
double Editor::hi(int key) { const ParamDef *d = P.T.get(kp(key)); return d ? d->max : 1; }
void Editor::format(int key, double v, char *b, int n) { P.T.format(kp(key), v, b, n); }
bool Editor::parse(int key, const char *s, double *v) { return P.T.parse(kp(key), s, v); }

} // namespace gui
} // namespace ts
