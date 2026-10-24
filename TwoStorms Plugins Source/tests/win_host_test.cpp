// The Windows twin of clap_host_test (for running the Windows .clap under Wine or on Windows).
//   win_host_test render <bundle.clap> <in.f64> <out.f64> <srate> <block> [n=v ...]
//   win_host_test gui    <bundle.clap> <out.png> <ms> [click:x,y ...]
#include <clap/clap.h>
#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static std::map<clap_id, uint32_t> g_timers;
static clap_id g_next = 1;
static bool g_cb = false;
static bool h_reg(const clap_host *, uint32_t ms, clap_id *id) { *id = g_next++; g_timers[*id] = ms; return true; }
static bool h_unreg(const clap_host *, clap_id id) { g_timers.erase(id); return true; }
static const clap_host_timer_support s_timer = {h_reg, h_unreg};
static void h_rescan(const clap_host *, clap_param_rescan_flags) {}
static void h_clear(const clap_host *, clap_id, clap_param_clear_flags) {}
static void h_flush(const clap_host *) {}
static const clap_host_params s_params = {h_rescan, h_clear, h_flush};
static void h_dirty(const clap_host *) {}
static const clap_host_state s_state = {h_dirty};
static const void *h_ext(const clap_host *, const char *id)
{
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &s_timer;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &s_params;
    if (!strcmp(id, CLAP_EXT_STATE)) return &s_state;
    return nullptr;
}
static void h_restart(const clap_host *) { printf("[host] restart requested\n"); }
static void h_proc(const clap_host *) {}
static void h_cb(const clap_host *) { g_cb = true; }
static clap_host g_host = {CLAP_VERSION_INIT, nullptr, "ts-win-host", "TwoStorms", "", "1.0", h_ext, h_restart, h_proc, h_cb};

struct EvList { std::vector<clap_event_param_value> v; };
static uint32_t in_size(const clap_input_events *l) { return (uint32_t)((EvList *)l->ctx)->v.size(); }
static const clap_event_header *in_get(const clap_input_events *l, uint32_t i) { return &((EvList *)l->ctx)->v[i].header; }
static bool out_push(const clap_output_events *, const clap_event_header *) { return true; }
static clap_event_param_value mkev(int id, double v)
{
    clap_event_param_value e{};
    e.header.size = sizeof e;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.param_id = (clap_id)id;
    e.value = v;
    e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
    return e;
}

static const clap_plugin *load(const char *path)
{
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return nullptr; }
    const clap_plugin_entry *e = (const clap_plugin_entry *)GetProcAddress(m, "clap_entry");
    if (!e || !e->init(path)) { printf("no entry\n"); return nullptr; }
    const clap_plugin_factory *f = (const clap_plugin_factory *)e->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor *d = f->get_plugin_descriptor(f, 0);
    printf("plugin: %s (%s) %s\n", d->name, d->id, d->version);
    const clap_plugin *p = f->create_plugin(f, &g_host, d->id);
    if (!p || !p->init(p)) return nullptr;
    return p;
}

static void timers(const clap_plugin *p, std::map<clap_id, double> &last)
{
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const clap_plugin_timer_support *ts = (const clap_plugin_timer_support *)p->get_extension(p, CLAP_EXT_TIMER_SUPPORT);
    for (auto &t : std::map<clap_id, uint32_t>(g_timers))
        if (now - last[t.first] >= t.second / 1000.0) { last[t.first] = now; ts->on_timer(p, t.first); }
    if (g_cb) { g_cb = false; p->on_main_thread(p); }
}

static int render(int argc, char **argv)
{
    const clap_plugin *p = load(argv[2]);
    if (!p) return 1;
    double sr = atof(argv[5]);
    int block = atoi(argv[6]);
    EvList pre;
    for (int i = 7; i < argc; ++i) {
        const char *eq = strchr(argv[i], '=');
        if (eq) pre.v.push_back(mkev(atoi(argv[i]), atof(eq + 1)));
    }
    const clap_plugin_params *pp = (const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
    clap_input_events ie{&pre, in_size, in_get};
    clap_output_events oe{nullptr, out_push};
    pp->flush(p, &ie, &oe);
    FILE *f = fopen(argv[3], "rb");
    fseek(f, 0, SEEK_END);
    long frames = ftell(f) / 32;
    fseek(f, 0, SEEK_SET);
    std::vector<double> in(frames * 4), out(frames * 2);
    if (fread(in.data(), 8, in.size(), f) != in.size()) return 1;
    fclose(f);
    p->activate(p, sr, 1, (uint32_t)block);
    p->start_processing(p);
    std::vector<double> b[6];
    for (auto &x : b) x.resize(block);
    for (long pos = 0; pos < frames; pos += block) {
        int n = (int)std::min<long>(block, frames - pos);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 4; ++c) b[c][i] = in[(pos + i) * 4 + c];
        double *i2[2] = {b[0].data(), b[1].data()}, *s2[2] = {b[2].data(), b[3].data()}, *o2[2] = {b[4].data(), b[5].data()};
        clap_audio_buffer ins[2] = {}, outs[1] = {};
        ins[0].channel_count = 2; ins[0].data64 = i2;
        ins[1].channel_count = 2; ins[1].data64 = s2;
        outs[0].channel_count = 2; outs[0].data64 = o2;
        EvList el;
        clap_input_events ie2{&el, in_size, in_get};
        clap_process pr{};
        pr.frames_count = (uint32_t)n;
        pr.audio_inputs = ins; pr.audio_inputs_count = 2;
        pr.audio_outputs = outs; pr.audio_outputs_count = 1;
        pr.in_events = &ie2; pr.out_events = &oe;
        p->process(p, &pr);
        for (int i = 0; i < n; ++i) { out[(pos + i) * 2] = b[4][i]; out[(pos + i) * 2 + 1] = b[5][i]; }
    }
    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    f = fopen(argv[4], "wb");
    fwrite(out.data(), 8, out.size(), f);
    fclose(f);
    printf("rendered %ld frames\n", frames);
    return 0;
}

static int gui(int argc, char **argv)
{
    const clap_plugin *p = load(argv[2]);
    if (!p) return 1;
    const clap_plugin_gui *g = (const clap_plugin_gui *)p->get_extension(p, CLAP_EXT_GUI);
    if (!g || !g->is_api_supported(p, CLAP_WINDOW_API_WIN32, false)) { printf("no win32 gui\n"); return 1; }
    p->activate(p, 48000, 1, 512);
    g->create(p, CLAP_WINDOW_API_WIN32, false);
    uint32_t W, H;
    g->get_size(p, &W, &H);
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "tshost";
    RegisterClassA(&wc);
    RECT rc{0, 0, (LONG)W, (LONG)H};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND top = CreateWindowA("tshost", "TwoStorms test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    clap_window win{CLAP_WINDOW_API_WIN32, {}};
    win.win32 = top;
    g->set_parent(p, &win);
    g->show(p);
    printf("[host] window %ux%u\n", W, H);
    std::map<clap_id, double> last;
    // TS_TONE=<Hz>: a plucked tone through the plug-in as time goes by (on this thread), for windows that show audio
    double tone = getenv("TS_TONE") ? atof(getenv("TS_TONE")) : 0;
    clap_output_events oe{nullptr, out_push};
    if (tone > 0) p->start_processing(p);
    long tpos = 0;
    DWORD tstart = GetTickCount();
    std::vector<double> tb[6];
    for (auto &x : tb) x.resize(512);
    auto feed = [&]() {
        long due = (long)((GetTickCount() - tstart) * 48.0);
        while (tone > 0 && tpos + 512 <= due) {
            for (int i = 0; i < 512; ++i) {
                double t = (double)((tpos + i) % 96000) / 48000;
                double v = 0;
                for (int h = 1; h <= 8; ++h) v += std::exp(-t * (1.2 + 0.5 * h)) / h * std::sin(2 * 3.14159265358979 * tone * h * t);
                tb[0][i] = tb[1][i] = 0.3 * v;
                tb[2][i] = tb[3][i] = 0;
            }
            double *i2[2] = {tb[0].data(), tb[1].data()}, *s2[2] = {tb[2].data(), tb[3].data()}, *o2[2] = {tb[4].data(), tb[5].data()};
            clap_audio_buffer ins[2] = {}, outs[1] = {};
            ins[0].channel_count = 2; ins[0].data64 = i2;
            ins[1].channel_count = 2; ins[1].data64 = s2;
            outs[0].channel_count = 2; outs[0].data64 = o2;
            EvList el;
            clap_input_events ie2{&el, in_size, in_get};
            clap_process pr{};
            pr.frames_count = 512;
            pr.audio_inputs = ins; pr.audio_inputs_count = 2;
            pr.audio_outputs = outs; pr.audio_outputs_count = 1;
            pr.in_events = &ie2; pr.out_events = &oe;
            p->process(p, &pr);
            tpos += 512;
        }
    };
    auto pump = [&](int ms) {
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < (DWORD)ms) {
            MSG m;
            while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
            feed();
            timers(p, last);
            Sleep(5);
        }
    };
    pump(atoi(argv[4]));
    HWND child = GetWindow(top, GW_CHILD);
    for (int i = 5; i < argc; ++i) {
        int x, y;
        if (sscanf(argv[i], "click:%d,%d", &x, &y) == 2) {
            SendMessageA(child, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
            pump(30);
            SendMessageA(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            pump(40);
            SendMessageA(child, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            pump(150);
        }
    }
    // capture the window
    HDC src = GetDC(child);
    HDC mem = CreateCompatibleDC(src);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)W;
    bi.bmiHeader.biHeight = -(LONG)H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(src, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, (int)W, (int)H, src, 0, 0, SRCCOPY);
    std::vector<unsigned char> rgb((size_t)W * H * 3);
    const uint32_t *px = (const uint32_t *)bits;
    for (size_t k = 0; k < (size_t)W * H; ++k) { rgb[k * 3] = (px[k] >> 16) & 255; rgb[k * 3 + 1] = (px[k] >> 8) & 255; rgb[k * 3 + 2] = px[k] & 255; }
    stbi_write_png(argv[3], (int)W, (int)H, 3, rgb.data(), (int)W * 3);
    printf("[host] screenshot %s\n", argv[3]);
    g->hide(p);
    g->destroy(p);
    p->deactivate(p);
    p->destroy(p);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) return 1;
    if (!strcmp(argv[1], "render")) return render(argc, argv);
    if (!strcmp(argv[1], "gui")) return gui(argc, argv);
    return 1;
}
