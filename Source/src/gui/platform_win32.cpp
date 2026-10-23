// The window on Windows: a child of the host's window, frames copied with SetDIBitsToDevice.
#ifdef _WIN32
#include "editor.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <mutex>
#include <set>

namespace ts {
namespace gui {

// windows with a value box being typed into (REAPER's hwnd_info hook asks)
static std::mutex s_editMx;
static std::set<HWND> s_editing;

static int hwndInfo(HWND hwnd, INT_PTR type)
{
    if (type != 0 && type != 1) return 0;
    std::lock_guard<std::mutex> l(s_editMx);
    for (HWND h = hwnd; h; h = GetParent(h))
        if (s_editing.count(h)) return 1;   // 0: a text field; 1: skip the global shortcuts
    return 0;
}

void *win32HwndInfoHook() { return (void *)&hwndInfo; }

static HINSTANCE thisModule()
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&thisModule, &m);
    return (HINSTANCE)m;
}

class Win32Window : public PlatformWindow {
public:
    explicit Win32Window(Editor &e) : E(e) {}
    ~Win32Window() override
    {
        textInput(false);
        if (hwnd) {
            KillTimer(hwnd, 1);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
        }
    }

    static const wchar_t *cls()
    {
        static wchar_t name[64] = {0};
        if (!name[0]) {
            // one class per loaded copy of the plug-in
            swprintf(name, 64, L"TwoStormsCLAP_%p", (void *)thisModule());
            WNDCLASSW wc{};
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = &Win32Window::proc;
            wc.hInstance = thisModule();
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = name;
            RegisterClassW(&wc);
        }
        return name;
    }

    bool attach(const clap_window *parent, int w, int h) override
    {
        hwnd = CreateWindowExW(0, cls(), L"TwoStorms", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, w, h,
                               (HWND)parent->win32, nullptr, thisModule(), nullptr);
        if (!hwnd) return false;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        DragAcceptFiles(hwnd, TRUE);   // files dropped on the window (the IR Loader's .wav files)
        // the host may have no timer support: tick ourselves
        SetTimer(hwnd, 1, 16, nullptr);
        cursors[CUR_ARROW] = LoadCursor(nullptr, IDC_ARROW);
        cursors[CUR_HAND] = LoadCursor(nullptr, IDC_HAND);
        cursors[CUR_HRESIZE] = LoadCursor(nullptr, IDC_SIZEWE);
        cursors[CUR_IBEAM] = LoadCursor(nullptr, IDC_IBEAM);
        cursors[CUR_MOVE] = LoadCursor(nullptr, IDC_SIZEALL);
        cursors[CUR_VRESIZE] = LoadCursor(nullptr, IDC_SIZENS);
        return true;
    }

    bool ownTimer() override { return true; }

    void setSize(int w, int h) override
    {
        if (hwnd) SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void show(bool on) override
    {
        if (hwnd) ShowWindow(hwnd, on ? SW_SHOW : SW_HIDE);
    }

    void blit(HDC dc)
    {
        if (!last || lw <= 0 || lh <= 0) return;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = lw;
        bmi.bmiHeader.biHeight = -lh;   // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, 0, 0, (DWORD)lw, (DWORD)lh, 0, 0, 0, (UINT)lh, last, &bmi, DIB_RGB_COLORS);
    }

    void present(const uint32_t *px, int w, int h) override
    {
        last = px;
        lw = w;
        lh = h;
        if (!hwnd) return;
        HDC dc = GetDC(hwnd);
        blit(dc);
        ReleaseDC(hwnd, dc);
    }

    void presentRects(const uint32_t *px, int w, int h, const int *rc, int n) override
    {
        last = px;
        lw = w;
        lh = h;
        if (!hwnd) return;
        HDC dc = GetDC(hwnd);
        // the whole picture through a clip of the changed rectangles: GDI copies only what's inside
        HRGN rg = CreateRectRgn(0, 0, 0, 0);
        for (int k = 0; k < n; ++k) {
            HRGN r = CreateRectRgn(rc[4 * k], rc[4 * k + 1], rc[4 * k] + rc[4 * k + 2], rc[4 * k + 1] + rc[4 * k + 3]);
            CombineRgn(rg, rg, r, RGN_OR);
            DeleteObject(r);
        }
        SelectClipRgn(dc, rg);
        blit(dc);
        SelectClipRgn(dc, nullptr);
        DeleteObject(rg);
        ReleaseDC(hwnd, dc);
    }

    void setCursor(Cursor c) override
    {
        cur = c;
        SetCursor(cursors[c] ? cursors[c] : cursors[0]);
    }

    void takeFocus() override
    {
        if (hwnd) SetFocus(hwnd);
    }

    void textInput(bool on) override
    {
        std::lock_guard<std::mutex> l(s_editMx);
        if (on && hwnd) s_editing.insert(hwnd);
        else s_editing.erase(hwnd);
    }

    static int mods()
    {
        int m = 0;
        if (GetKeyState(VK_SHIFT) & 0x8000) m |= M_SHIFT;
        if (GetKeyState(VK_CONTROL) & 0x8000) m |= M_CTRL;
        if (GetKeyState(VK_MENU) & 0x8000) m |= M_ALT;
        return m;
    }

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        Win32Window *w = (Win32Window *)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!w) return DefWindowProcW(h, msg, wp, lp);
        Editor &E = w->E;
        switch (msg) {
        case WM_TIMER:
            E.tick();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            w->blit(dc);
            EndPaint(h, &ps);
            E.evExpose();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DROPFILES: {
            HDROP hd = (HDROP)wp;
            POINT pt{0, 0};
            DragQueryPoint(hd, &pt);
            std::vector<std::string> files;
            UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                UINT len = DragQueryFileW(hd, i, nullptr, 0);
                std::wstring ws(len + 1, L'\0');
                DragQueryFileW(hd, i, &ws[0], len + 1);
                ws.resize(len);
                int u = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
                std::string s8((size_t)u, '\0');
                WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s8[0], u, nullptr, nullptr);
                files.push_back(s8);
            }
            DragFinish(hd);
            E.evDrop(files, (float)pt.x, (float)pt.y);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!w->tracking) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                w->tracking = true;
            }
            E.evMods(mods());
            E.evMove((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            return 0;
        }
        case WM_MOUSELEAVE:
            w->tracking = false;
            E.evLeave();
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            SetCapture(h);
            E.evMods(mods());
            E.evMove((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            E.evButton(1, true);
            E.tick();
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            E.evMods(mods());
            E.evMove((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            E.evButton(1, false);
            return 0;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
            E.evMods(mods());
            E.evMove((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            E.evButton(2, true);
            E.tick();
            return 0;
        case WM_RBUTTONUP:
            E.evButton(2, false);
            return 0;
        case WM_MBUTTONDOWN:
            E.evButton(4, true);
            return 0;
        case WM_MBUTTONUP:
            E.evButton(4, false);
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            E.evMods(mods());
            E.evMove((float)p.x, (float)p.y);
            E.evWheel(GET_WHEEL_DELTA_WPARAM(wp) / 120.f, 0);
            return 0;
        }
        case WM_MOUSEHWHEEL:
            E.evWheel(0, GET_WHEEL_DELTA_WPARAM(wp) / 120.f);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) { SetCursor(w->cursors[w->cur] ? w->cursors[w->cur] : w->cursors[0]); return TRUE; }
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
        case WM_KEYDOWN: {
            E.evMods(mods());
            int k = K_NONE;
            switch (wp) {
            case VK_RETURN: k = K_ENTER; break;
            case VK_ESCAPE: k = K_ESC; break;
            case VK_BACK: k = K_BACK; break;
            case VK_DELETE: k = K_DEL; break;
            case VK_LEFT: k = K_LEFT; break;
            case VK_RIGHT: k = K_RIGHT; break;
            case VK_HOME: k = K_HOME; break;
            case VK_END: k = K_END; break;
            case VK_TAB: k = K_TAB; break;
            default: break;
            }
            if (k != K_NONE) { E.evKey(k); return 0; }
            break;
        }
        case WM_KEYUP:
            E.evMods(mods());
            break;
        case WM_CHAR:
            if (wp >= 32) { E.evChar((uint32_t)wp); return 0; }
            return 0;
        default:
            break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

private:
    Editor &E;
    HWND hwnd = nullptr;
    const uint32_t *last = nullptr;
    int lw = 0, lh = 0;
    bool tracking = false;
    HCURSOR cursors[8] = {};
    Cursor cur = CUR_ARROW;
};

PlatformWindow *PlatformWindow::create(Editor &e) { return new Win32Window(e); }

} // namespace gui
} // namespace ts
#endif
