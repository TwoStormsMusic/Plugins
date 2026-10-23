// The window of a TwoStorms plug-in: the CLAP gui extension, a native child window (X11 or Win32),
// the frame loop (input, drawing, presenting) and the shared window zoom.
#pragma once
#include "canvas.h"
#include "ui.h"
#include <clap/clap.h>
#include <memory>
#include <vector>

namespace ts {
class Plugin;
namespace gui {

// One plug-in's window content (immediate mode): its design size, and a frame.
class View {
public:
    virtual ~View() {}
    virtual void designSize(float &w, float &h) = 0;
    virtual void frame(Ui &ui) = 0;
    virtual void opened() {}
    virtual void closed() {}
    // true while something moves on screen (meters, history): frames keep coming at the animation rate
    virtual bool animating() { return true; }
    // a popover is open: draw everything every frame (no cached layer)
    virtual bool wantsFull() { return false; }
    // anything shown in the still parts that isn't a parameter (status, names): when it changes,
    // the still layer is drawn again
    virtual uint64_t statusHash() { return 0; }
    // files dropped on the window (where: design units); only Windows delivers them
    virtual void filesDropped(const std::vector<std::string> &files, float x, float y) { (void)files; (void)x; (void)y; }

    // ---- a window of any size and shape (the Tuner) ----
    // true: the design at 100% is baseW x baseH, and it grows in one direction to fill the window, never stretched
    // (designSize is then whatever sizeChanged last said)
    virtual bool freeSize(float &baseW, float &baseH) { (void)baseW; (void)baseH; return false; }
    // the size it was left at (logical pixels; 0: the base size), and a new one (logical pixels, and the design units
    // it gives)
    virtual void savedSize(float &w, float &h) { w = 0; h = 0; }
    virtual void sizeChanged(float w, float h, float designW, float designH) { (void)w; (void)h; (void)designW; (void)designH; }
    // ---- the clock ----
    // every tick of the window's clock (about 60 a second), drawn or not: true asks for a frame now
    virtual bool poll(double now) { (void)now; return false; }
    // the time between frames while animating()
    virtual double frameInterval() { return 1.0 / 30; }
    // where the moving parts are (design units): when given, a frame that doesn't redraw the still layer restores,
    // draws and sends to the screen only these (the view promises to draw nothing moving outside them)
    virtual bool dynamicRegions(std::vector<R> &rs) { (void)rs; return false; }
};

class Editor;

class PlatformWindow {
public:
    virtual ~PlatformWindow() {}
    virtual bool attach(const clap_window *parent, int w, int h) = 0;
    virtual void setSize(int w, int h) = 0;
    virtual void show(bool on) = 0;
    virtual void present(const uint32_t *px, int w, int h) = 0;
    // only some rectangles changed (x, y, w, h in pixels, n of them)
    virtual void presentRects(const uint32_t *px, int w, int h, const int *rects, int n)
    {
        (void)rects;
        (void)n;
        present(px, w, h);
    }
    virtual void pump() {}
    virtual int fd() { return -1; }
    virtual void setCursor(Cursor c) { (void)c; }
    virtual void takeFocus() {}
    virtual bool ownTimer() { return false; }   // the window ticks itself (Win32 without host timers)
    virtual void textInput(bool on) { (void)on; } // a value box is being typed into
    static PlatformWindow *create(Editor &e);
};

class Editor : public ParamAccess {
public:
    explicit Editor(Plugin &p);
    ~Editor();
    static bool apiSupported(const char *api, bool floating);
    static const char *preferredApi();
    bool create(const char *api, bool floating);
    void destroy();
    bool setScale(double s);
    bool getSize(uint32_t *w, uint32_t *h);
    bool canResize() { return true; }
    bool resizeHints(clap_gui_resize_hints *h);
    bool adjustSize(uint32_t *w, uint32_t *h);
    bool setSize(uint32_t w, uint32_t h);
    bool setParent(const clap_window *w);
    bool show();
    bool hide();
    void onTimer(clap_id id);
    void onFd(int fd, clap_posix_fd_flags_t flags);

    // ---- from the platform window ----
    void evMove(float x, float y);
    void evLeave();
    void evButton(int button, bool down);   // 1 left, 2 right, 4 middle
    void evWheel(float dy, float dx);
    void evMods(int mods);
    void evKey(int key);
    void evChar(uint32_t cp);
    void evExpose() { needFull_ = true; }
    void evDrop(const std::vector<std::string> &files, float px, float py);
    void tick();   // the frame clock

    // ---- ParamAccess: this instance's parameters (keys < 256) or another's ((slot + 2) * 256 + id) ----
    double get(int key) override;
    void begin(int key) override;
    void set(int key, double v) override;
    void end(int key) override;
    double def(int key) override;
    double lo(int key) override;
    double hi(int key) override;
    void format(int key, double v, char *b, int n) override;
    bool parse(int key, const char *s, double *v) override;

    Plugin &P;
    float scale() const { return scale_; }
    // the window zoom (P_ZOOM): asks the host for the new size
    void applyZoom();

private:
    std::unique_ptr<View> view_;
    std::unique_ptr<PlatformWindow> win_;
    Canvas canvas_;
    Ui ui_;
    Input in_;
    float designW_ = 900, designH_ = 700;
    // a free-size view (View::freeSize): its base design size and the window's logical size
    bool free_ = false;
    float baseW_ = 900, baseH_ = 540, lw_ = 900, lh_ = 540;
    void freeLayout();
    float hostScale_ = 1, scale_ = 1;
    int w_ = 0, h_ = 0;
    bool visible_ = false;
    bool needFull_ = true;
    bool inputPending_ = false;
    double lastFrame_ = 0;
    std::vector<uint32_t> still_;
    std::vector<R> dynR_;
    std::vector<int> dynPx_;
    bool stillOk_ = false;
    uint64_t lastHash_ = 0;
    uint64_t stateHash();
    clap_id timer_ = CLAP_INVALID_ID;
    int zoomSeen_ = -1;
    int64_t zoomV_ = -1;
    Cursor cursor_ = CUR_ARROW;
    bool hadKeyboard_ = false;
    bool hooked_ = false;
    void render();
    void zoomSync();
    void sizeFromScale(float sc, uint32_t *w, uint32_t *h);
};

} // namespace gui
} // namespace ts
