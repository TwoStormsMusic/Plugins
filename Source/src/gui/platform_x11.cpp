// The window on Linux: an X11 child of the host's window, its own display connection, frames
// copied with XPutImage (through MIT-SHM when the server has it).
#if !defined(_WIN32) && !defined(__APPLE__)
#include "editor.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <vector>

// libX11 and libXext are loaded when the first window opens, not linked: the plug-in then loads in
// any process (a headless render, a host without X), and the binary doesn't pin library versions.
namespace {
struct X11Api {
    bool ok = false, shm = false;
#define TSX(name) decltype(&::name) name = nullptr;
    TSX(XOpenDisplay) TSX(XCloseDisplay) TSX(XCreateWindow) TSX(XDestroyWindow) TSX(XCreateGC) TSX(XFreeGC)
    TSX(XCreateFontCursor) TSX(XFreeCursor) TSX(XDefineCursor) TSX(XMapWindow) TSX(XUnmapWindow) TSX(XResizeWindow)
    TSX(XFlush) TSX(XSync) TSX(XPending) TSX(XNextEvent) TSX(XLookupString) TSX(XPutImage) TSX(XCreateImage)
    TSX(XSetInputFocus) TSX(XSetErrorHandler)
    TSX(XShmQueryVersion) TSX(XShmCreateImage) TSX(XShmAttach) TSX(XShmDetach) TSX(XShmPutImage)
#undef TSX
    void load()
    {
        void *x = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!x) x = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
        if (!x) return;
        bool all = true;
#define TSL(h, name) name = (decltype(name))dlsym(h, #name); if (!name) all = false;
        TSL(x, XOpenDisplay) TSL(x, XCloseDisplay) TSL(x, XCreateWindow) TSL(x, XDestroyWindow) TSL(x, XCreateGC) TSL(x, XFreeGC)
        TSL(x, XCreateFontCursor) TSL(x, XFreeCursor) TSL(x, XDefineCursor) TSL(x, XMapWindow) TSL(x, XUnmapWindow)
        TSL(x, XResizeWindow) TSL(x, XFlush) TSL(x, XSync) TSL(x, XPending) TSL(x, XNextEvent) TSL(x, XLookupString)
        TSL(x, XPutImage) TSL(x, XCreateImage) TSL(x, XSetInputFocus) TSL(x, XSetErrorHandler)
        ok = all;
        void *e = dlopen("libXext.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!e) e = dlopen("libXext.so", RTLD_NOW | RTLD_LOCAL);
        if (e) {
            bool s = true;
            all = true;
            TSL(e, XShmQueryVersion) TSL(e, XShmCreateImage) TSL(e, XShmAttach) TSL(e, XShmDetach) TSL(e, XShmPutImage)
            s = all;
            shm = s;
        }
#undef TSL
    }
};
X11Api &XA()
{
    static X11Api a;
    static bool once = (a.load(), true);
    (void)once;
    return a;
}
} // namespace

#define XOpenDisplay XA().XOpenDisplay
#define XCloseDisplay XA().XCloseDisplay
#define XCreateWindow XA().XCreateWindow
#define XDestroyWindow XA().XDestroyWindow
#define XCreateGC XA().XCreateGC
#define XFreeGC XA().XFreeGC
#define XCreateFontCursor XA().XCreateFontCursor
#define XFreeCursor XA().XFreeCursor
#define XDefineCursor XA().XDefineCursor
#define XMapWindow XA().XMapWindow
#define XUnmapWindow XA().XUnmapWindow
#define XResizeWindow XA().XResizeWindow
#define XFlush XA().XFlush
#define XSync XA().XSync
#define XPending XA().XPending
#define XNextEvent XA().XNextEvent
#define XLookupString XA().XLookupString
#define XPutImage XA().XPutImage
#define XCreateImage XA().XCreateImage
#define XSetInputFocus XA().XSetInputFocus
#define XSetErrorHandler XA().XSetErrorHandler
#define XShmQueryVersion XA().XShmQueryVersion
#define XShmCreateImage XA().XShmCreateImage
#define XShmAttach XA().XShmAttach
#define XShmDetach XA().XShmDetach
#define XShmPutImage XA().XShmPutImage

namespace ts {
namespace gui {

class X11Window : public PlatformWindow {
public:
    explicit X11Window(Editor &e) : E(e) {}
    ~X11Window() override
    {
        if (!dpy) return;
        freeImage();
        for (auto &c : cursors) if (c) XFreeCursor(dpy, c);
        if (gc) XFreeGC(dpy, gc);
        if (win) XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
    }

    bool attach(const clap_window *parent, int w, int h) override
    {
        if (!XA().ok) return false;
        dpy = XOpenDisplay(nullptr);
        if (!dpy) return false;
        int scr = DefaultScreen(dpy);
        vis = DefaultVisual(dpy, scr);
        depth = DefaultDepth(dpy, scr);
        XSetWindowAttributes a{};
        a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask | KeyReleaseMask |
                       EnterWindowMask | LeaveWindowMask | StructureNotifyMask | FocusChangeMask;
        a.background_pixel = 0x0b0b0d;
        a.border_pixel = 0;
        a.colormap = DefaultColormap(dpy, scr);
        win = XCreateWindow(dpy, (Window)parent->x11, 0, 0, (unsigned)w, (unsigned)h, 0, depth, InputOutput, vis,
                            CWEventMask | CWBackPixel | CWBorderPixel | CWColormap, &a);
        if (!win) return false;
        gc = XCreateGC(dpy, win, 0, nullptr);
        cursors[CUR_ARROW] = XCreateFontCursor(dpy, XC_left_ptr);
        cursors[CUR_HAND] = XCreateFontCursor(dpy, XC_hand2);
        cursors[CUR_HRESIZE] = XCreateFontCursor(dpy, XC_sb_h_double_arrow);
        cursors[CUR_IBEAM] = XCreateFontCursor(dpy, XC_xterm);
        cursors[CUR_MOVE] = XCreateFontCursor(dpy, XC_fleur);
        cursors[CUR_VRESIZE] = XCreateFontCursor(dpy, XC_sb_v_double_arrow);
        int major, minor;
        Bool pix;
        useShm = XA().shm && XShmQueryVersion(dpy, &major, &minor, &pix) && getenv("DISPLAY") && strncmp(getenv("DISPLAY"), ":", 1) == 0;
        XMapWindow(dpy, win);
        XFlush(dpy);
        return true;
    }

    void setSize(int w, int h) override
    {
        if (!dpy) return;
        XResizeWindow(dpy, win, (unsigned)w, (unsigned)h);
        XFlush(dpy);
    }

    void show(bool on) override
    {
        if (!dpy) return;
        if (on) XMapWindow(dpy, win);
        else XUnmapWindow(dpy, win);
        XFlush(dpy);
    }

    void freeImage()
    {
        if (img) {
            if (shmOn) { XShmDetach(dpy, &shmInfo); XSync(dpy, False); }
            if (shmOn) { shmdt(shmInfo.shmaddr); img->data = nullptr; }
            else img->data = nullptr;   // the pixels belong to the canvas
            XDestroyImage(img);
            img = nullptr;
            shmOn = false;
        }
    }

    bool makeShm(int w, int h)
    {
        img = XShmCreateImage(dpy, vis, (unsigned)depth, ZPixmap, nullptr, &shmInfo, (unsigned)w, (unsigned)h);
        if (!img) return false;
        shmInfo.shmid = shmget(IPC_PRIVATE, (size_t)img->bytes_per_line * img->height, IPC_CREAT | 0600);
        if (shmInfo.shmid < 0) { XDestroyImage(img); img = nullptr; return false; }
        shmInfo.shmaddr = img->data = (char *)shmat(shmInfo.shmid, nullptr, 0);
        shmInfo.readOnly = False;
        // a failed attach (a remote display) shows up as an X error: catch it
        XErrorHandler old = XSetErrorHandler([](Display *, XErrorEvent *) -> int { s_shmFailed = true; return 0; });
        s_shmFailed = false;
        XShmAttach(dpy, &shmInfo);
        XSync(dpy, False);
        XSetErrorHandler(old);
        shmctl(shmInfo.shmid, IPC_RMID, nullptr);
        if (s_shmFailed) {
            shmdt(shmInfo.shmaddr);
            img->data = nullptr;
            XDestroyImage(img);
            img = nullptr;
            useShm = false;
            return false;
        }
        shmOn = true;
        imgW = w;
        imgH = h;
        return true;
    }

    void present(const uint32_t *px, int w, int h) override
    {
        if (!dpy) return;
        if (useShm) {
            if (!img || imgW != w || imgH != h) { freeImage(); makeShm(w, h); }
            if (img && shmOn) {
                if (img->bytes_per_line == w * 4) memcpy(img->data, px, (size_t)w * h * 4);
                else
                    for (int y = 0; y < h; ++y) memcpy(img->data + (size_t)y * img->bytes_per_line, px + (size_t)y * w, (size_t)w * 4);
                XShmPutImage(dpy, win, gc, img, 0, 0, 0, 0, (unsigned)w, (unsigned)h, False);
                XFlush(dpy);
                return;
            }
        }
        XImage *im = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, (char *)px, (unsigned)w, (unsigned)h, 32, w * 4);
        if (!im) return;
        XPutImage(dpy, win, gc, im, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
        im->data = nullptr;
        XDestroyImage(im);
        XFlush(dpy);
    }

    void presentRects(const uint32_t *px, int w, int h, const int *rc, int n) override
    {
        if (!dpy) return;
        if (useShm) {
            if (!img || imgW != w || imgH != h || !shmOn) { present(px, w, h); return; }
            for (int k = 0; k < n; ++k) {
                int x = rc[4 * k], y = rc[4 * k + 1], rw = rc[4 * k + 2], rh = rc[4 * k + 3];
                for (int yy = y; yy < y + rh; ++yy)
                    memcpy(img->data + (size_t)yy * img->bytes_per_line + (size_t)x * 4, px + (size_t)yy * w + x, (size_t)rw * 4);
                XShmPutImage(dpy, win, gc, img, x, y, x, y, (unsigned)rw, (unsigned)rh, False);
            }
            XFlush(dpy);
            return;
        }
        XImage *im = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, (char *)px, (unsigned)w, (unsigned)h, 32, w * 4);
        if (!im) return;
        for (int k = 0; k < n; ++k)
            XPutImage(dpy, win, gc, im, rc[4 * k], rc[4 * k + 1], rc[4 * k], rc[4 * k + 1], (unsigned)rc[4 * k + 2], (unsigned)rc[4 * k + 3]);
        im->data = nullptr;
        XDestroyImage(im);
        XFlush(dpy);
    }

    int fd() override { return dpy ? ConnectionNumber(dpy) : -1; }

    void setCursor(Cursor c) override
    {
        if (!dpy) return;
        XDefineCursor(dpy, win, cursors[c] ? cursors[c] : cursors[0]);
        XFlush(dpy);
    }

    void takeFocus() override
    {
        if (!dpy) return;
        XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
        XFlush(dpy);
    }

    static int mods(unsigned st)
    {
        int m = 0;
        if (st & ShiftMask) m |= M_SHIFT;
        if (st & ControlMask) m |= M_CTRL;
        if (st & Mod1Mask) m |= M_ALT;
        return m;
    }

    void pump() override
    {
        if (!dpy) return;
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.xany.window != win) continue;
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) E.evExpose();
                break;
            case MotionNotify:
                E.evMods(mods(ev.xmotion.state));
                E.evMove((float)ev.xmotion.x, (float)ev.xmotion.y);
                break;
            case ButtonPress:
            case ButtonRelease: {
                bool down = ev.type == ButtonPress;
                E.evMods(mods(ev.xbutton.state));
                E.evMove((float)ev.xbutton.x, (float)ev.xbutton.y);
                unsigned b = ev.xbutton.button;
                if (b == 1) E.evButton(1, down);
                else if (b == 2) E.evButton(4, down);
                else if (b == 3) E.evButton(2, down);
                else if (down && b == 4) E.evWheel(1, 0);
                else if (down && b == 5) E.evWheel(-1, 0);
                else if (down && b == 6) E.evWheel(0, -1);
                else if (down && b == 7) E.evWheel(0, 1);
                break;
            }
            case LeaveNotify:
                E.evLeave();
                break;
            case KeyPress: {
                char buf[16];
                KeySym ks = 0;
                int n = XLookupString(&ev.xkey, buf, sizeof buf, &ks, nullptr);
                E.evMods(mods(ev.xkey.state));
                int k = K_NONE;
                switch (ks) {
                case XK_Return: case XK_KP_Enter: k = K_ENTER; break;
                case XK_Escape: k = K_ESC; break;
                case XK_BackSpace: k = K_BACK; break;
                case XK_Delete: case XK_KP_Delete: k = K_DEL; break;
                case XK_Left: k = K_LEFT; break;
                case XK_Right: k = K_RIGHT; break;
                case XK_Home: k = K_HOME; break;
                case XK_End: k = K_END; break;
                case XK_Tab: k = K_TAB; break;
                default: break;
                }
                if (k != K_NONE) E.evKey(k);
                else
                    for (int i = 0; i < n; ++i)
                        if ((unsigned char)buf[i] >= 32) E.evChar((unsigned char)buf[i]);
                break;
            }
            case KeyRelease:
                E.evMods(mods(ev.xkey.state));
                break;
            default:
                break;
            }
        }
    }

private:
    Editor &E;
    Display *dpy = nullptr;
    Window win = 0;
    GC gc = nullptr;
    Visual *vis = nullptr;
    int depth = 24;
    ::Cursor cursors[8] = {};
    bool useShm = false, shmOn = false;
    XShmSegmentInfo shmInfo{};
    XImage *img = nullptr;
    int imgW = 0, imgH = 0;
    static bool s_shmFailed;
};

bool X11Window::s_shmFailed = false;

PlatformWindow *PlatformWindow::create(Editor &e) { return new X11Window(e); }

} // namespace gui
} // namespace ts
#endif
