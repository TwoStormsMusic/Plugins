// A minimal CLAP host for testing the TwoStorms bundle the way a DAW loads it.
//   clap_host_test info   <bundle.clap>
//   clap_host_test render <bundle.clap> <in.f64> <out.f64> <srate> <block> [n=v ...] [t<frame>:n=v ...] [--f32] [--state]
//   clap_host_test gui    <bundle.clap> <out.png> <ms> [script...]     (needs DISPLAY; script: click:x,y rclick:x,y
//                                                                       drag:x0,y0,x1,y1 wait:ms shot:file.png key:text set:n=v)
//   clap_host_test chain  <bundle.clap>
//   clap_host_test bypasskeep <bundle.clap>  (a bypassed plug-in keeps its slot and ID; a copy of it takes a new one)
//   clap_host_test tunermute <bundle.clap>   (the Tuner's Mute silencing the Gain Stage it listens to)
// TS_PLUGIN=<plugin id> picks the plug-in (default: the first in the bundle).
#include <clap/clap.h>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>
#include "../src/core/shm.h"
#include "../src/core/reaper_bridge.h"
#include "../src/core/clock.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const clap_plugin *g_plug = nullptr;
static std::atomic<bool> g_restart{false}, g_callback{false}, g_flush{false};
static std::map<clap_id, uint32_t> g_timers;
static clap_id g_nextTimer = 1;
static std::map<int, clap_posix_fd_flags_t> g_fds;
static int g_resizeW = 0, g_resizeH = 0;

static bool h_reg_timer(const clap_host *, uint32_t ms, clap_id *id) { *id = g_nextTimer++; g_timers[*id] = ms; return true; }
static bool h_unreg_timer(const clap_host *, clap_id id) { g_timers.erase(id); return true; }
static const clap_host_timer_support s_timer = {h_reg_timer, h_unreg_timer};
static bool h_reg_fd(const clap_host *, int fd, clap_posix_fd_flags_t f) { g_fds[fd] = f; return true; }
static bool h_mod_fd(const clap_host *, int fd, clap_posix_fd_flags_t f) { g_fds[fd] = f; return true; }
static bool h_unreg_fd(const clap_host *, int fd) { g_fds.erase(fd); return true; }
static const clap_host_posix_fd_support s_fd = {h_reg_fd, h_mod_fd, h_unreg_fd};
static void h_rescan(const clap_host *, clap_param_rescan_flags) {}
static void h_clear(const clap_host *, clap_id, clap_param_clear_flags) {}
static void h_req_flush(const clap_host *) { g_flush = true; }
static const clap_host_params s_params = {h_rescan, h_clear, h_req_flush};
static int g_dirty = 0;
static void h_mark_dirty(const clap_host *) { g_dirty++; }
static const clap_host_state s_state = {h_mark_dirty};
static void h_lat_changed(const clap_host *) { printf("[host] latency changed\n"); }
static const clap_host_latency s_latency = {h_lat_changed};
static void h_resize_hints_changed(const clap_host *) {}
static bool h_request_resize(const clap_host *, uint32_t w, uint32_t h) { g_resizeW = (int)w; g_resizeH = (int)h; return true; }
static bool h_request_show(const clap_host *) { return true; }
static bool h_request_hide(const clap_host *) { return true; }
static void h_closed(const clap_host *, bool) {}
static const clap_host_gui s_gui = {h_resize_hints_changed, h_request_resize, h_request_show, h_request_hide, h_closed};

static const void *h_get_ext(const clap_host *, const char *id)
{
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &s_timer;
    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &s_fd;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &s_params;
    if (!strcmp(id, CLAP_EXT_STATE)) return &s_state;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &s_latency;
    if (!strcmp(id, CLAP_EXT_GUI)) return &s_gui;
    return nullptr;
}
static void h_req_restart(const clap_host *) { g_restart = true; printf("[host] restart requested\n"); }
static void h_req_process(const clap_host *) {}
static void h_req_callback(const clap_host *) { g_callback = true; }
static clap_host s_host = {CLAP_VERSION_INIT, nullptr, "ts-test-host", "TwoStorms", "", "1.0", h_get_ext, h_req_restart, h_req_process, h_req_callback};

// ---- events
struct EvList {
    std::vector<clap_event_param_value> v;   // at the block's start
    std::vector<clap_event_midi> *mid = nullptr;   // then MIDI, in time order
};
static uint32_t in_size(const clap_input_events *l)
{
    EvList *e = (EvList *)l->ctx;
    return (uint32_t)(e->v.size() + (e->mid ? e->mid->size() : 0));
}
static const clap_event_header *in_get(const clap_input_events *l, uint32_t i)
{
    EvList *e = (EvList *)l->ctx;
    if (i < e->v.size()) return &e->v[i].header;
    return &(*e->mid)[i - e->v.size()].header;
}
static std::vector<std::pair<int, double>> g_outVals;
static int g_outGestures = 0;
static bool out_push(const clap_output_events *, const clap_event_header *h)
{
    if (h->type == CLAP_EVENT_PARAM_VALUE) {
        const clap_event_param_value *e = (const clap_event_param_value *)h;
        g_outVals.push_back({(int)e->param_id, e->value});
    } else if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
        g_outGestures++;
    }
    return true;
}

static clap_event_param_value mkev(int id, double v)
{
    clap_event_param_value e{};
    e.header.size = sizeof e;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.param_id = (clap_id)id;
    e.value = v;
    e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
    return e;
}

struct Lib {
    void *h = nullptr;
    const clap_plugin_entry *entry = nullptr;
    const clap_plugin_factory *fac = nullptr;
    bool load(const char *path)
    {
        h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return false; }
        entry = (const clap_plugin_entry *)dlsym(h, "clap_entry");
        if (!entry || !entry->init(path)) return false;
        fac = (const clap_plugin_factory *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
        return fac != nullptr;
    }
};

// TS_PLUGIN=<id> picks a plug-in in the bundle (default: the first one)
static const clap_plugin *makePlugin(Lib &L, int index = -1)
{
    if (index < 0) {
        index = 0;
        if (const char *want = getenv("TS_PLUGIN"))
            for (uint32_t i = 0, n = L.fac->get_plugin_count(L.fac); i < n; ++i)
                if (!strcmp(L.fac->get_plugin_descriptor(L.fac, i)->id, want)) index = (int)i;
    }
    const clap_plugin_descriptor *d = L.fac->get_plugin_descriptor(L.fac, index);
    const clap_plugin *p = L.fac->create_plugin(L.fac, &s_host, d->id);
    if (!p || !p->init(p)) return nullptr;
    return p;
}

static void runTimers(const clap_plugin *p, std::map<clap_id, double> &last, double now)
{
    const clap_plugin_timer_support *ts = (const clap_plugin_timer_support *)p->get_extension(p, CLAP_EXT_TIMER_SUPPORT);
    if (!ts) return;
    for (auto &t : std::map<clap_id, uint32_t>(g_timers)) {
        double &l = last[t.first];
        if (now - l >= t.second / 1000.0) { l = now; ts->on_timer(p, t.first); }
    }
    if (g_callback.exchange(false)) p->on_main_thread(p);
}

static double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------- info
static int cmdInfo(Lib &L)
{
    uint32_t n = L.fac->get_plugin_count(L.fac);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_plugin_descriptor *d = L.fac->get_plugin_descriptor(L.fac, i);
        printf("plugin %u: %s  (%s) v%s\n", i, d->name, d->id, d->version);
        const clap_plugin *p = makePlugin(L, (int)i);
        const clap_plugin_params *pp = (const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
        uint32_t np = pp->count(p);
        printf("  %u host parameters\n", np);
        for (uint32_t k = 0; k < np; ++k) {
            clap_param_info pi;
            pp->get_info(p, k, &pi);
            double v;
            pp->get_value(p, pi.id, &v);
            char txt[64];
            pp->value_to_text(p, pi.id, v, txt, sizeof txt);
            printf("   %3u [%s] %-28s %10g .. %-10g = %-14s%s%s\n", pi.id, pi.module, pi.name, pi.min_value, pi.max_value, txt,
                   (pi.flags & CLAP_PARAM_IS_STEPPED) ? " stepped" : "", (pi.flags & CLAP_PARAM_IS_ENUM) ? " enum" : "");
        }
        const clap_plugin_audio_ports *ap = (const clap_plugin_audio_ports *)p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
        for (int dir = 1; dir >= 0; --dir)
            for (uint32_t k = 0; k < ap->count(p, dir); ++k) {
                clap_audio_port_info info;
                ap->get(p, k, dir, &info);
                printf("  %s port %u: %s, %u ch, flags %x\n", dir ? "in" : "out", k, info.name, info.channel_count, info.flags);
            }
        p->destroy(p);
    }
    return 0;
}

// ---------------------------------------------------------------- render
struct ByteStream {
    std::string data;
    size_t pos = 0;
};
static int64_t os_write(const clap_ostream *s, const void *b, uint64_t n) { ((ByteStream *)s->ctx)->data.append((const char *)b, n); return (int64_t)n; }
static int64_t is_read(const clap_istream *s, void *b, uint64_t n)
{
    ByteStream *bs = (ByteStream *)s->ctx;
    uint64_t left = bs->data.size() - bs->pos, k = std::min<uint64_t>(n, left);
    memcpy(b, bs->data.data() + bs->pos, k);
    bs->pos += k;
    return (int64_t)k;
}

static int cmdRender(Lib &L, int argc, char **argv)
{
    const char *inPath = argv[3], *outPath = argv[4];
    double sr = atof(argv[5]);
    int block = atoi(argv[6]);
    bool f32 = false, stateTest = false;
    struct E { long fr; int id; double v; };
    std::vector<E> evs;
    // the transport and MIDI, as tools/jsfx_ref takes them: tempo= play= beat0= ts=N/D m<frame>:a,b,c
    double tempo = 120, beat0 = 0;
    int play = 0, tsn = 4, tsd = 4;
    struct Mev { long frame; uint8_t d[3]; };
    std::vector<Mev> mevs;
    for (int i = 7; i < argc; ++i) {
        if (!strcmp(argv[i], "--f32")) { f32 = true; continue; }
        if (!strcmp(argv[i], "--state")) { stateTest = true; continue; }
        if (!strncmp(argv[i], "tempo=", 6)) { tempo = atof(argv[i] + 6); continue; }
        if (!strncmp(argv[i], "play=", 5)) { play = atoi(argv[i] + 5); continue; }
        if (!strncmp(argv[i], "beat0=", 6)) { beat0 = atof(argv[i] + 6); continue; }
        if (!strncmp(argv[i], "ts=", 3)) { sscanf(argv[i] + 3, "%d/%d", &tsn, &tsd); continue; }
        if (argv[i][0] == 'm' && isdigit((unsigned char)argv[i][1])) {
            Mev e{};
            int b0 = 0, b1 = 0, b2 = 0;
            if (sscanf(argv[i] + 1, "%ld:%d,%d,%d", &e.frame, &b0, &b1, &b2) == 4) {
                e.d[0] = (uint8_t)b0; e.d[1] = (uint8_t)b1; e.d[2] = (uint8_t)b2;
                mevs.push_back(e);
            }
            continue;
        }
        const char *s = argv[i];
        long fr = 0;
        if (*s == 't') { fr = strtol(s + 1, (char **)&s, 10); if (*s == ':') ++s; }
        const char *eq = strchr(s, '=');
        if (eq) evs.push_back({fr, atoi(s), atof(eq + 1)});
    }
    const clap_plugin *p = makePlugin(L);
    // channels: the main port's (NC), plus a stereo sidechain if there is one; the input file holds
    // NC + 2 channels (main, then sidechain), the output file NC
    const clap_plugin_audio_ports *ap = (const clap_plugin_audio_ports *)p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
    clap_audio_port_info pi0{};
    ap->get(p, 0, true, &pi0);
    const int NC = (int)pi0.channel_count, NI = NC + 2;
    const uint32_t nInPorts = ap->count(p, true);
    FILE *f = fopen(inPath, "rb");
    fseek(f, 0, SEEK_END);
    long frames = ftell(f) / (8 * NI);
    fseek(f, 0, SEEK_SET);
    std::vector<double> in(frames * NI);
    if (fread(in.data(), 8, in.size(), f) != in.size()) return 1;
    fclose(f);
    const clap_plugin_params *pp = (const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
    const clap_plugin_state *ps = (const clap_plugin_state *)p->get_extension(p, CLAP_EXT_STATE);
    const clap_plugin_latency *pl = (const clap_plugin_latency *)p->get_extension(p, CLAP_EXT_LATENCY);
    // parameters before activation go through flush
    EvList pre;
    for (auto &e : evs) if (e.fr == 0) pre.v.push_back(mkev(e.id, e.v));
    clap_input_events ie{&pre, in_size, in_get};
    clap_output_events oe{nullptr, out_push};
    pp->flush(p, &ie, &oe);
    if (stateTest) {
        // save, make a second instance, load, and render with that one
        ByteStream bs;
        clap_ostream os{&bs, os_write};
        ps->save(p, &os);
        printf("state: %zu bytes\n", bs.data.size());
        p->destroy(p);
        p = makePlugin(L);
        pp = (const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
        ps = (const clap_plugin_state *)p->get_extension(p, CLAP_EXT_STATE);
        pl = (const clap_plugin_latency *)p->get_extension(p, CLAP_EXT_LATENCY);
        clap_istream is{&bs, is_read};
        if (!ps->load(p, &is)) { printf("state load FAILED\n"); return 1; }
    }
    p->activate(p, sr, 1, (uint32_t)block);
    p->start_processing(p);
    printf("latency %u\n", pl->get(p));
    std::vector<double> b64[20];
    std::vector<float> b32[20];
    for (int c = 0; c < 20; ++c) { b64[c].resize(block); b32[c].resize(block); }
    std::vector<double> out(frames * NC);
    double tproc = 0;
    for (long pos = 0; pos < frames; pos += block) {
        int n = (int)std::min<long>(block, frames - pos);
        EvList el;
        for (auto &e : evs)
            if (e.fr > 0 && e.fr >= pos && e.fr < pos + n) el.v.push_back(mkev(e.id, e.v));
        std::vector<clap_event_midi> mids;
        mids.reserve(mevs.size());
        for (auto &e : mevs)
            if (e.frame >= pos && e.frame < pos + n) {
                clap_event_midi m{};
                m.header.size = sizeof m;
                m.header.type = CLAP_EVENT_MIDI;
                m.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                m.header.time = (uint32_t)(e.frame - pos);
                m.port_index = 0;
                memcpy(m.data, e.d, 3);
                mids.push_back(m);
            }
        el.mid = &mids;
        clap_event_transport tp{};
        tp.header.size = sizeof tp;
        tp.header.type = CLAP_EVENT_TRANSPORT;
        tp.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | (play ? CLAP_TRANSPORT_IS_PLAYING : 0);
        tp.tempo = tempo;
        tp.song_pos_beats = llround((beat0 + (play ? pos / sr * tempo / 60 : 0)) * 2147483648.0);
        tp.tsig_num = (uint16_t)tsn;
        tp.tsig_denom = (uint16_t)tsd;
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < NI; ++c) { b64[c][i] = in[(pos + i) * NI + c]; b32[c][i] = (float)b64[c][i]; }
        // buffers: main in 0..NC, sidechain NC..NC+2, out 10..10+NC
        double *d64in[8], *d64sc[2] = {b64[NC].data(), b64[NC + 1].data()}, *d64out[8];
        float *d32in[8], *d32sc[2] = {b32[NC].data(), b32[NC + 1].data()}, *d32out[8];
        for (int c = 0; c < NC; ++c) {
            d64in[c] = b64[c].data(); d32in[c] = b32[c].data();
            d64out[c] = b64[10 + c].data(); d32out[c] = b32[10 + c].data();
        }
        clap_audio_buffer ins[2] = {}, outs[1] = {};
        ins[0].channel_count = NC;
        ins[1].channel_count = 2;
        outs[0].channel_count = NC;
        if (f32) { ins[0].data32 = d32in; ins[1].data32 = d32sc; outs[0].data32 = d32out; }
        else { ins[0].data64 = d64in; ins[1].data64 = d64sc; outs[0].data64 = d64out; }
        clap_input_events ie2{&el, in_size, in_get};
        clap_process pr{};
        pr.steady_time = pos;
        pr.frames_count = (uint32_t)n;
        pr.audio_inputs = ins;
        pr.audio_inputs_count = nInPorts;
        pr.audio_outputs = outs;
        pr.audio_outputs_count = 1;
        pr.in_events = &ie2;
        pr.out_events = &oe;
        pr.transport = &tp;
        double t0 = nowS();
        p->process(p, &pr);
        tproc += nowS() - t0;
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < NC; ++c) out[(pos + i) * NC + c] = f32 ? b32[10 + c][i] : b64[10 + c][i];
    }
    p->stop_processing(p);
    p->deactivate(p);
    printf("output events: %zu values, %d gestures\n", g_outVals.size(), g_outGestures);
    printf("process time %.2f ms for %.2f s of audio (%.3f%% of real time)\n", tproc * 1000, frames / sr, tproc / (frames / sr) * 100);
    for (auto &v : g_outVals) printf("  out %d = %g\n", v.first, v.second);
    p->destroy(p);
    f = fopen(outPath, "wb");
    fwrite(out.data(), 8, out.size(), f);
    fclose(f);
    return 0;
}

// ---------------------------------------------------------------- gui
static Display *g_dpy;
static Window g_top;

static Window childOf(Window w)
{
    Window root, parent, *kids = nullptr;
    unsigned n = 0;
    XQueryTree(g_dpy, w, &root, &parent, &kids, &n);
    Window c = n ? kids[0] : 0;
    if (kids) XFree(kids);
    return c;
}

static void sendButton(Window w, int x, int y, int button, bool press, unsigned state = 0)
{
    XEvent e{};
    e.type = press ? ButtonPress : ButtonRelease;
    e.xbutton.window = w;
    e.xbutton.x = x;
    e.xbutton.y = y;
    e.xbutton.button = (unsigned)button;
    e.xbutton.state = state;
    e.xbutton.same_screen = True;
    XSendEvent(g_dpy, w, True, press ? ButtonPressMask : ButtonReleaseMask, &e);
    XFlush(g_dpy);
}

static void sendMotion(Window w, int x, int y, unsigned state = 0)
{
    XEvent e{};
    e.type = MotionNotify;
    e.xmotion.window = w;
    e.xmotion.x = x;
    e.xmotion.y = y;
    e.xmotion.state = state;
    e.xmotion.same_screen = True;
    XSendEvent(g_dpy, w, True, PointerMotionMask, &e);
    XFlush(g_dpy);
}

static void sendKey(Window w, KeySym ks)
{
    XEvent e{};
    e.type = KeyPress;
    e.xkey.window = w;
    e.xkey.keycode = XKeysymToKeycode(g_dpy, ks);
    e.xkey.same_screen = True;
    XSendEvent(g_dpy, w, True, KeyPressMask, &e);
    XFlush(g_dpy);
}

static void shot(Window w, int W, int H, const char *path)
{
    XImage *im = XGetImage(g_dpy, w, 0, 0, (unsigned)W, (unsigned)H, AllPlanes, ZPixmap);
    if (!im) { printf("no image\n"); return; }
    std::vector<unsigned char> rgb((size_t)W * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned long px = XGetPixel(im, x, y);
            rgb[(y * W + x) * 3] = (px >> 16) & 255;
            rgb[(y * W + x) * 3 + 1] = (px >> 8) & 255;
            rgb[(y * W + x) * 3 + 2] = px & 255;
        }
    XDestroyImage(im);
    stbi_write_png(path, W, H, 3, rgb.data(), W * 3);
    printf("[host] screenshot %s (%dx%d)\n", path, W, H);
}

static int cmdGui(Lib &L, int argc, char **argv)
{
    const char *png = argv[3];
    int ms = atoi(argv[4]);
    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) { fprintf(stderr, "no display\n"); return 1; }
    const clap_plugin *p = makePlugin(L);
    const clap_plugin_gui *g = (const clap_plugin_gui *)p->get_extension(p, CLAP_EXT_GUI);
    const clap_plugin_params *pp = (const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
    if (!g || !g->is_api_supported(p, CLAP_WINDOW_API_X11, false)) { fprintf(stderr, "no x11 gui\n"); return 1; }
    // a sidechain-free test signal from a file, looped, on an audio thread
    std::vector<double> sig;
    // TS_SIG=<file>: another signal (4 channels, f64); TS_MAIN_SILENT=1: the windowed instance gets silence (the
    // TS_EXTRA ones still get the signal: a Tuner on a track of its own, listening to them)
    const char *sigPath = getenv("TS_SIG") ? getenv("TS_SIG") : "tests/sig/mix.f64";
    const bool mainSilent = getenv("TS_MAIN_SILENT") != nullptr;
    if (FILE *f = fopen(sigPath, "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f) / 8;
        fseek(f, 0, SEEK_SET);
        sig.resize(n);
        if (fread(sig.data(), 8, n, f) != (size_t)n) sig.clear();
        fclose(f);
    }
    double sr = 48000;
    int block = 512;
    p->activate(p, sr, 1, (uint32_t)block);
    p->start_processing(p);
    // TS_EXTRA=<id>,<id>,...: more instances, made before the windowed one is shown and processed alongside it
    // (with TS_TEST_CHAIN they share its chain, in the order made: the windowed one first)
    std::vector<const clap_plugin *> extras;
    if (const char *ex = getenv("TS_EXTRA")) {
        std::string all = ex;
        size_t q = 0;
        while (q < all.size()) {
            size_t e = all.find(',', q);
            if (e == std::string::npos) e = all.size();
            std::string id = all.substr(q, e - q);
            q = e + 1;
            for (uint32_t i = 0, n = L.fac->get_plugin_count(L.fac); i < n; ++i)
                if (id == L.fac->get_plugin_descriptor(L.fac, i)->id) {
                    const clap_plugin *x = makePlugin(L, (int)i);
                    if (x) { x->activate(x, sr, 1, (uint32_t)block); x->start_processing(x); extras.push_back(x); }
                }
        }
    }
    std::vector<std::map<clap_id, double>> lastX(extras.size());
    std::atomic<double> extraPeak{0};
    std::mutex evm;
    std::vector<clap_event_param_value> pending, pendingX;   // for the windowed instance, for the first extra
    std::atomic<bool> run{true};
    std::thread audio([&] {
        std::vector<double> b[6];
        for (auto &x : b) x.resize(block);
        long pos = 0, frames = (long)sig.size() / 4;
        auto t0 = std::chrono::steady_clock::now();
        long done = 0;
        while (run) {
            for (int i = 0; i < block; ++i) {
                long k = frames ? (pos + i) % frames : 0;
                for (int c = 0; c < 4; ++c) b[c][i] = frames ? sig[k * 4 + c] : 0;
            }
            pos += block;
            double *in[2] = {b[0].data(), b[1].data()}, *sc[2] = {b[2].data(), b[3].data()}, *out[2] = {b[4].data(), b[5].data()};
            clap_audio_buffer ins[2] = {}, outs[1] = {};
            ins[0].channel_count = 2; ins[0].data64 = in;
            ins[1].channel_count = 2; ins[1].data64 = sc;
            outs[0].channel_count = 2; outs[0].data64 = out;
            EvList el;
            { std::lock_guard<std::mutex> l(evm); el.v.swap(pending); }
            clap_input_events ie{&el, in_size, in_get};
            clap_output_events oe{nullptr, out_push};
            clap_process pr{};
            pr.frames_count = (uint32_t)block;
            pr.audio_inputs = ins; pr.audio_inputs_count = 2;
            pr.audio_outputs = outs; pr.audio_outputs_count = 1;
            pr.in_events = &ie; pr.out_events = &oe;
            std::vector<double> keep[2];
            if (mainSilent) {
                keep[0] = b[0]; keep[1] = b[1];
                std::fill(b[0].begin(), b[0].end(), 0.0);
                std::fill(b[1].begin(), b[1].end(), 0.0);
            }
            p->process(p, &pr);
            if (mainSilent) { b[0] = keep[0]; b[1] = keep[1]; }
            // the extras' output level (the Tuner's Mute reaches them)
            extraPeak = 0;
            for (size_t xi = 0; xi < extras.size(); ++xi) {
                const clap_plugin *x = extras[xi];
                EvList none;
                if (xi == 0) { std::lock_guard<std::mutex> l(evm); none.v.swap(pendingX); }
                clap_input_events ie3{&none, in_size, in_get};
                pr.in_events = &ie3;
                x->process(x, &pr);
                for (int i = 0; i < block; ++i) extraPeak = std::max(extraPeak.load(), std::fabs(b[4][i]));
            }
            done += block;
            std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(done / sr * 1e6)));
        }
    });
    g->create(p, CLAP_WINDOW_API_X11, false);
    uint32_t W, H;
    g->get_size(p, &W, &H);
    g_top = XCreateSimpleWindow(g_dpy, DefaultRootWindow(g_dpy), 0, 0, W, H, 0, 0, 0);
    XMapWindow(g_dpy, g_top);
    XSync(g_dpy, False);
    clap_window win{CLAP_WINDOW_API_X11, {}};
    win.x11 = g_top;
    g->set_parent(p, &win);
    g->show(p);
    printf("[host] window %ux%u\n", W, H);
    std::map<clap_id, double> last;
    auto pumpFor = [&](int msd) {
        double t0 = nowS();
        while (nowS() - t0 < msd / 1000.0) {
            std::vector<pollfd> pf;
            for (auto &f : g_fds) pf.push_back({f.first, POLLIN, 0});
            poll(pf.data(), pf.size(), 5);
            const clap_plugin_posix_fd_support *fs = (const clap_plugin_posix_fd_support *)p->get_extension(p, CLAP_EXT_POSIX_FD_SUPPORT);
            for (auto &x : pf)
                if (x.revents & POLLIN) fs->on_fd(p, x.fd, CLAP_POSIX_FD_READ);
            runTimers(p, last, nowS());
            for (size_t k = 0; k < extras.size(); ++k) runTimers(extras[k], lastX[k], nowS());
            if (g_resizeW) {
                g->set_size(p, (uint32_t)g_resizeW, (uint32_t)g_resizeH);
                XResizeWindow(g_dpy, g_top, (unsigned)g_resizeW, (unsigned)g_resizeH);
                W = (uint32_t)g_resizeW; H = (uint32_t)g_resizeH;
                printf("[host] resized to %ux%u\n", W, H);
                g_resizeW = 0;
            }
            while (XPending(g_dpy)) { XEvent e; XNextEvent(g_dpy, &e); }
            if (g_flush.exchange(false)) {}
        }
    };
    pumpFor(ms);
    Window child = childOf(g_top);
    for (int i = 5; i < argc; ++i) {
        std::string a = argv[i];
        int x0, y0, x1, y1;
        if (sscanf(a.c_str(), "click:%d,%d", &x0, &y0) == 2) {
            sendMotion(child, x0, y0); pumpFor(30);
            sendButton(child, x0, y0, 1, true); pumpFor(40);
            sendButton(child, x0, y0, 1, false); pumpFor(60);
        } else if (sscanf(a.c_str(), "shiftclick:%d,%d", &x0, &y0) == 2) {
            sendMotion(child, x0, y0, ShiftMask); pumpFor(30);
            sendButton(child, x0, y0, 1, true, ShiftMask); pumpFor(40);
            sendButton(child, x0, y0, 1, false, ShiftMask); pumpFor(60);
        } else if (sscanf(a.c_str(), "dclick:%d,%d", &x0, &y0) == 2) {
            sendMotion(child, x0, y0); pumpFor(30);
            sendButton(child, x0, y0, 1, true); pumpFor(30);
            sendButton(child, x0, y0, 1, false); pumpFor(30);
            sendButton(child, x0, y0, 1, true); pumpFor(30);
            sendButton(child, x0, y0, 1, false); pumpFor(60);
        } else if (sscanf(a.c_str(), "rclick:%d,%d", &x0, &y0) == 2) {
            sendMotion(child, x0, y0); pumpFor(30);
            sendButton(child, x0, y0, 3, true); pumpFor(40);
            sendButton(child, x0, y0, 3, false); pumpFor(60);
        } else if (sscanf(a.c_str(), "move:%d,%d", &x0, &y0) == 2) {
            sendMotion(child, x0, y0); pumpFor(60);
        } else if (sscanf(a.c_str(), "drag:%d,%d,%d,%d", &x0, &y0, &x1, &y1) == 4) {
            sendMotion(child, x0, y0); pumpFor(30);
            sendButton(child, x0, y0, 1, true); pumpFor(40);
            for (int k = 1; k <= 10; ++k) { sendMotion(child, x0 + (x1 - x0) * k / 10, y0 + (y1 - y0) * k / 10, Button1Mask); pumpFor(20); }
            sendButton(child, x1, y1, 1, false); pumpFor(60);
        } else if (sscanf(a.c_str(), "wheel:%d,%d,%d", &x0, &y0, &x1) == 3) {
            sendMotion(child, x0, y0); pumpFor(20);
            for (int k = 0; k < std::abs(x1); ++k) { sendButton(child, x0, y0, x1 > 0 ? 4 : 5, true); pumpFor(20); }
        } else if (a.rfind("wait:", 0) == 0) {
            pumpFor(atoi(a.c_str() + 5));
        } else if (a.rfind("shot:", 0) == 0) {
            pumpFor(80);
            shot(g_top, (int)W, (int)H, a.c_str() + 5);
        } else if (a.rfind("key:", 0) == 0) {
            for (const char *s = a.c_str() + 4; *s; ++s) {
                KeySym ks = *s == '\n' ? XK_Return : (*s == '\b' ? XK_BackSpace : (KeySym)(unsigned char)*s);
                if (*s == '-') ks = XK_minus;
                if (*s == '.') ks = XK_period;
                if (*s == ' ') ks = XK_space;
                sendKey(child, ks);
                pumpFor(20);
            }
        } else if (a == "enter") {
            sendKey(child, XK_Return); pumpFor(60);
        } else if (a == "esc") {
            sendKey(child, XK_Escape); pumpFor(60);
        } else if (a.rfind("set:", 0) == 0) {
            int id;
            double v;
            if (sscanf(a.c_str() + 4, "%d=%lf", &id, &v) == 2) { std::lock_guard<std::mutex> l(evm); pending.push_back(mkev(id, v)); }
            pumpFor(100);
        } else if (sscanf(a.c_str(), "resize:%d,%d", &x0, &y0) == 2) {
            // the user drags the window's edge: the plug-in may adjust, then gets the size
            uint32_t w = (uint32_t)x0, h = (uint32_t)y0;
            g->adjust_size(p, &w, &h);
            g->set_size(p, w, h);
            XResizeWindow(g_dpy, g_top, w, h);
            W = w; H = h;
            printf("[host] resized to %ux%u\n", W, H);
            pumpFor(150);
        } else if (a.rfind("linkid:", 0) == 0 && !extras.empty()) {
            // set a parameter of the windowed instance to an extra's instance ID (36): linkid:<param>[,<extra index>]
            int pid = 0, xi = 0;
            sscanf(a.c_str() + 7, "%d,%d", &pid, &xi);
            xi = std::max(0, std::min((int)extras.size() - 1, xi));
            const clap_plugin_params *xp = (const clap_plugin_params *)extras[xi]->get_extension(extras[xi], CLAP_EXT_PARAMS);
            double id = 0;
            if (xp) xp->get_value(extras[xi], 36, &id);
            { std::lock_guard<std::mutex> l(evm); pending.push_back(mkev(pid, id)); }
            printf("[host] param %d = the extra's ID %.0f\n", pid, id);
            pumpFor(100);
        } else if (a.rfind("xset:", 0) == 0) {
            // a parameter of the first extra
            int id;
            double v;
            if (sscanf(a.c_str() + 5, "%d=%lf", &id, &v) == 2) { std::lock_guard<std::mutex> l(evm); pendingX.push_back(mkev(id, v)); }
            pumpFor(100);
        } else if (a.rfind("xlinkid:", 0) == 0) {
            // set a parameter of the first extra to the windowed instance's ID (36)
            double id = 0;
            pp->get_value(p, 36, &id);
            int pid = atoi(a.c_str() + 8);
            { std::lock_guard<std::mutex> l(evm); pendingX.push_back(mkev(pid, id)); }
            printf("[host] extra's param %d = this one's ID %.0f\n", pid, id);
            pumpFor(100);
        } else if (a == "extrapeak") {
            printf("[host] extra output peak %.4f\n", extraPeak.load());
        } else if (a.rfind("print:", 0) == 0) {
            int id = atoi(a.c_str() + 6);
            double v;
            pp->get_value(p, (clap_id)id, &v);
            char t[64];
            pp->value_to_text(p, (clap_id)id, v, t, sizeof t);
            printf("[host] param %d = %g (%s)\n", id, v, t);
        }
    }
    shot(g_top, (int)W, (int)H, png);
    run = false;
    audio.join();
    g->hide(p);
    g->destroy(p);
    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    printf("[host] out events %zu, gestures %d, dirty %d\n", g_outVals.size(), g_outGestures, g_dirty);
    XDestroyWindow(g_dpy, g_top);
    XCloseDisplay(g_dpy);
    return 0;
}


// ---------------------------------------------------------------- chain: two instances in one chain
// A -> B on the test chain (TS_TEST_CHAIN): B's Auto in Match mode, then a drive link from A to B.
// the Gate keying from an earlier plug-in's output through shared memory: a Gate (key = the Act's output) next to an
// Act fed the same audio must gate like a Gate keyed from its own input (up to the key's float precision)
static int cmdKey(Lib &L, const char *sigPath)
{
    setenv("TS_TEST_CHAIN", "key-test", 1);
    auto byId = [&](const char *id) -> const clap_plugin * {
        for (uint32_t i = 0, n = L.fac->get_plugin_count(L.fac); i < n; ++i)
            if (!strcmp(L.fac->get_plugin_descriptor(L.fac, i)->id, id)) return makePlugin(L, (int)i);
        return nullptr;
    };
    const clap_plugin *A = byId("com.twostorms.act"), *B = byId("com.twostorms.gate"), *C = byId("com.twostorms.gate");
    if (!A || !B || !C) return 1;
    const clap_plugin *ps[3] = {A, B, C};
    double sr = 48000;
    int block = 512;
    for (auto p : ps) { p->activate(p, sr, 1, (uint32_t)block); p->start_processing(p); }
    std::vector<double> sig;
    FILE *f = fopen(sigPath, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f) / 8;
    fseek(f, 0, SEEK_SET);
    sig.resize(n);
    if (fread(sig.data(), 8, n, f) != (size_t)n) return 1;
    fclose(f);
    long frames = n / 4;
    std::map<clap_id, double> last[3];
    double fake = 0;
    auto timers = [&](double dt) { fake += dt; for (int k = 0; k < 3; ++k) runTimers(ps[k], last[k], fake); };
    for (int i = 0; i < 40; ++i) timers(0.06);   // placements known (every instance in one chain, in order made)
    // the Act's ID, then: B keys from it, C from its own input; both gate at -20 dB
    const clap_plugin_params *pa = (const clap_plugin_params *)A->get_extension(A, CLAP_EXT_PARAMS);
    double aid = 0;
    auto setp = [&](const clap_plugin *p, std::vector<std::pair<int, double>> v) {
        EvList el;
        for (auto &x : v) el.v.push_back(mkev(x.first, x.second));
        clap_input_events ie{&el, in_size, in_get};
        clap_output_events oe{nullptr, out_push};
        ((const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS))->flush(p, &ie, &oe);
    };
    setp(B, {{50, -20}, {51, 10}});
    setp(C, {{57, 0}, {50, -20}, {51, 10}});
    std::vector<double> bufs[8];
    for (auto &x : bufs) x.resize(block);
    std::vector<double> ob, oc;
    for (long pos = 0; pos + block <= frames; pos += block) {
        // the Act has its ID once it has processed: then B keys from it
        if (pos == block * 4) {
            pa->get_value(A, 36, &aid);
            printf("act id %.0f\n", aid);
            setp(B, {{57, 2}, {58, aid}});
        }
        for (int k = 0; k < 3; ++k) {
            const clap_plugin *p = ps[k];
            for (int i = 0; i < block; ++i)
                for (int c = 0; c < 4; ++c) bufs[c][i] = sig[(pos + i) * 4 + c];
            double *in[8], *out[8];
            for (int c = 0; c < 8; ++c) { in[c] = bufs[c < 2 ? c : 2 + (c % 2)].data(); }
            double *sc[2] = {bufs[2].data(), bufs[3].data()};
            out[0] = bufs[4].data(); out[1] = bufs[5].data();
            for (int c = 2; c < 8; ++c) out[c] = bufs[6 + (c % 2)].data();
            clap_audio_buffer ins[2] = {}, outs[1] = {};
            uint32_t nc = k == 0 ? 8 : 2;
            ins[0].channel_count = nc; ins[0].data64 = in;
            ins[1].channel_count = 2; ins[1].data64 = sc;
            outs[0].channel_count = nc; outs[0].data64 = out;
            EvList el;
            clap_input_events ie{&el, in_size, in_get};
            clap_output_events oe{nullptr, out_push};
            clap_process pr{};
            pr.frames_count = (uint32_t)block;
            pr.audio_inputs = ins; pr.audio_inputs_count = k == 0 ? 1 : 2;
            pr.audio_outputs = outs; pr.audio_outputs_count = 1;
            pr.in_events = &ie; pr.out_events = &oe;
            p->process(p, &pr);
            if (k == 1) for (int i = 0; i < block; ++i) ob.push_back(bufs[4][i]);
            if (k == 2) for (int i = 0; i < block; ++i) oc.push_back(bufs[4][i]);
        }
        timers((double)block / sr);
    }
    // after the first second (the key starts flowing a block or two in; the gate state settles by the next hit)
    double md = 0, pk = 0;
    for (size_t i = (size_t)sr; i < ob.size(); ++i) { md = std::max(md, std::fabs(ob[i] - oc[i])); pk = std::max(pk, std::fabs(oc[i])); }
    double eb = 0, ec = 0;
    for (size_t i = 0; i < ob.size(); ++i) { eb += ob[i] * ob[i]; ec += oc[i] * oc[i]; }
    printf("keyed from the Act vs from its own input: max diff %.3g (peak %.3f), energy %.4f vs %.4f\n", md, pk, eb, ec);
    for (auto p : ps) { p->stop_processing(p); p->deactivate(p); p->destroy(p); }
    return md < 1e-6 ? 0 : 1;
}

// the Tuner's Mute reaching the plug-ins it listens to: two Gain Stages (A, C) and a Tuner (B) listening to both with
// Mute on: A and C go silent within their 10 ms fade (B too), and play again a quarter second after B lets go; with
// "Mute all tracks heard" off, Mute silences only B
static int cmdTunerMute(Lib &L)
{
    auto byId = [&](const char *id) -> const clap_plugin * {
        for (uint32_t i = 0, n = L.fac->get_plugin_count(L.fac); i < n; ++i)
            if (!strcmp(L.fac->get_plugin_descriptor(L.fac, i)->id, id)) return makePlugin(L, (int)i);
        return nullptr;
    };
    const clap_plugin *A = byId("com.twostorms.gainstage"), *C = byId("com.twostorms.gainstage"), *B = byId("com.twostorms.tuner");
    if (!A || !B || !C) return 1;
    const clap_plugin *ps[3] = {A, C, B};
    double sr = 48000;
    int block = 256;
    for (auto p : ps) { p->activate(p, sr, 1, (uint32_t)block); p->start_processing(p); }
    auto setp = [&](const clap_plugin *p, std::vector<std::pair<int, double>> v) {
        EvList el;
        for (auto &x : v) el.v.push_back(mkev(x.first, x.second));
        clap_input_events ie{&el, in_size, in_get};
        clap_output_events oe{nullptr, out_push};
        ((const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS))->flush(p, &ie, &oe);
    };
    std::vector<double> bufs[12];
    for (auto &x : bufs) x.resize(block);
    long pos = 0;
    // n blocks of a 220 Hz tone through all three; the peaks of their outputs over the last `tail` blocks
    auto run = [&](int nb, int tail, double *pk) {
        for (int k = 0; k < 3; ++k) pk[k] = 0;
        for (int b = 0; b < nb; ++b, pos += block) {
            for (int k = 0; k < 3; ++k) {
                const clap_plugin *p = ps[k];
                for (int i = 0; i < block; ++i) bufs[0][i] = bufs[1][i] = 0.5 * std::sin(2 * M_PI * 220 * (pos + i) / sr);
                double *in[8], *out[8];
                for (int c = 0; c < 8; ++c) { in[c] = bufs[c < 2 ? c : 2].data(); out[c] = bufs[4 + c].data(); }
                std::fill(bufs[2].begin(), bufs[2].end(), 0.0);
                clap_audio_buffer ins[1] = {}, outs[1] = {};
                uint32_t nc = k < 2 ? 8 : 2;
                ins[0].channel_count = nc; ins[0].data64 = in;
                outs[0].channel_count = nc; outs[0].data64 = out;
                EvList el;
                clap_input_events ie{&el, in_size, in_get};
                clap_output_events oe{nullptr, out_push};
                clap_process pr{};
                pr.frames_count = (uint32_t)block;
                pr.audio_inputs = ins; pr.audio_inputs_count = 1;
                pr.audio_outputs = outs; pr.audio_outputs_count = 1;
                pr.in_events = &ie; pr.out_events = &oe;
                p->process(p, &pr);
                if (b >= nb - tail)
                    for (int i = 0; i < block; ++i) pk[k] = std::max(pk[k], std::max(std::fabs(bufs[4][i]), std::fabs(bufs[5][i])));
            }
        }
    };
    double p0[3], p1[3], p2[3], p3[3];
    run(10, 5, p0);
    double aid = 0, cid = 0;
    ((const clap_plugin_params *)A->get_extension(A, CLAP_EXT_PARAMS))->get_value(A, 36, &aid);
    ((const clap_plugin_params *)C->get_extension(C, CLAP_EXT_PARAMS))->get_value(C, 36, &cid);
    // B listens to both (70, 71: its first two sources), not to its own track (50), Mute on (53)
    setp(B, {{50, 0}, {70, aid}, {71, cid}, {53, 1}});
    run(40, 30, p1);   // past the first scan, the block and the 10 ms fade
    setp(B, {{61, 0}});   // Mute all tracks heard: off
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    run(40, 20, p2);
    setp(B, {{53, 0}, {61, 1}});
    run(40, 20, p3);
    printf("gain stages and tuner: before %.3f %.3f %.3f; Mute %.6f %.6f %.6f; Mute, only its own output %.3f %.3f %.6f; "
           "after %.3f %.3f %.3f\n",
           p0[0], p0[1], p0[2], p1[0], p1[1], p1[2], p2[0], p2[1], p2[2], p3[0], p3[1], p3[2]);
    for (auto p : ps) { p->stop_processing(p); p->deactivate(p); p->destroy(p); }
    bool ok = p0[0] > 0.4 && p0[1] > 0.4 && p0[2] > 0.4 && p1[0] < 1e-9 && p1[1] < 1e-9 && p1[2] < 1e-9 && p2[0] > 0.4 && p2[1] > 0.4 &&
              p2[2] < 1e-9 && p3[0] > 0.4 && p3[1] > 0.4 && p3[2] > 0.4;
    return ok ? 0 : 1;
}

// a bypassed plug-in keeps its slot and its ID: A runs, then is bypassed (no audio, its main thread still ticks) for
// three seconds while B starts and C, a copy of A (same ID), starts too. Nobody may take A's slot, C must take a new
// ID, and A, playing again, keeps its own.
static int cmdBypassKeep(Lib &L)
{
    auto byId = [&](const char *id) -> const clap_plugin * {
        for (uint32_t i = 0, n = L.fac->get_plugin_count(L.fac); i < n; ++i)
            if (!strcmp(L.fac->get_plugin_descriptor(L.fac, i)->id, id)) return makePlugin(L, (int)i);
        return nullptr;
    };
    const clap_plugin *A = byId("com.twostorms.act"), *B = byId("com.twostorms.compressor"), *C = byId("com.twostorms.act");
    if (!A || !B || !C) return 1;
    double sr = 48000;
    int block = 256;
    for (auto p : {A, B, C}) { p->activate(p, sr, 1, (uint32_t)block); p->start_processing(p); }
    std::vector<double> bufs[16];
    for (auto &x : bufs) x.resize(block);
    auto proc = [&](const clap_plugin *p, int nch) {
        double *in[8], *out[8];
        for (int c = 0; c < 8; ++c) { in[c] = bufs[c].data(); out[c] = bufs[8 + c].data(); }
        clap_audio_buffer ins[1] = {}, outs[1] = {};
        ins[0].channel_count = (uint32_t)nch; ins[0].data64 = in;
        outs[0].channel_count = (uint32_t)nch; outs[0].data64 = out;
        EvList el;
        clap_input_events ie{&el, in_size, in_get};
        clap_output_events oe{nullptr, out_push};
        clap_process pr{};
        pr.frames_count = (uint32_t)block;
        pr.audio_inputs = ins; pr.audio_inputs_count = 1;
        pr.audio_outputs = outs; pr.audio_outputs_count = 1;
        pr.in_events = &ie; pr.out_events = &oe;
        p->process(p, &pr);
    };
    auto pid = [&](const clap_plugin *p) {
        double v = 0;
        ((const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS))->get_value(p, 36, &v);
        return (int64_t)v;
    };
    auto slotOf = [&](int64_t id) {
        for (int s = 0; s < ts::shm::NSLOT; ++s)
            if (ts::shm::slot(s).owner.load() == id) return s;
        return -1;
    };
    std::map<clap_id, double> la, lb, lc;
    // A alone for half a second
    for (int i = 0; i < 100; ++i) { proc(A, 8); runTimers(A, la, nowS()); std::this_thread::sleep_for(std::chrono::microseconds(5000)); }
    int64_t aid = pid(A);
    int as = slotOf(aid);
    // C: a copy of A (its ID too)
    {
        EvList el;
        el.v.push_back(mkev(36, (double)aid));
        clap_input_events ie{&el, in_size, in_get};
        clap_output_events oe{nullptr, out_push};
        ((const clap_plugin_params *)C->get_extension(C, CLAP_EXT_PARAMS))->flush(C, &ie, &oe);
    }
    // A bypassed for 3 s: only its main thread; B and C play
    double t0 = nowS();
    while (nowS() - t0 < 3.0) {
        runTimers(A, la, nowS());
        proc(B, 2); runTimers(B, lb, nowS());
        proc(C, 8); runTimers(C, lc, nowS());
        std::this_thread::sleep_for(std::chrono::microseconds(5000));
    }
    int64_t bid = pid(B), cid = pid(C);
    bool kept = ts::shm::slot(as).owner.load() == aid;
    // A plays again
    for (int i = 0; i < 100; ++i) { proc(A, 8); runTimers(A, la, nowS()); proc(B, 2); proc(C, 8); std::this_thread::sleep_for(std::chrono::microseconds(5000)); }
    int64_t aid2 = pid(A);
    printf("A %lld in slot %d (kept while bypassed: %s, ID after: %lld); B %lld in slot %d; the copy C %lld in slot %d\n", (long long)aid, as,
           kept ? "yes" : "NO", (long long)aid2, (long long)bid, slotOf(bid), (long long)cid, slotOf(cid));
    bool ok = kept && aid2 == aid && cid != aid && slotOf(bid) != as && slotOf(cid) != as && slotOf(aid) == as;
    for (auto p : {A, B, C}) { p->stop_processing(p); p->deactivate(p); p->destroy(p); }
    return ok ? 0 : 1;
}

static int cmdChain(Lib &L)
{
    setenv("TS_TEST_CHAIN", "chain-test", 1);
    const clap_plugin *A = makePlugin(L), *B = makePlugin(L);
    const clap_plugin *ps[2] = {A, B};
    double sr = 48000;
    int block = 512;
    for (auto p : ps) { p->activate(p, sr, 1, (uint32_t)block); p->start_processing(p); }
    std::vector<double> sig;
    FILE *f = fopen("tests/sig/mix.f64", "rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f) / 8;
    fseek(f, 0, SEEK_SET);
    sig.resize(n);
    if (fread(sig.data(), 8, n, f) != (size_t)n) return 1;
    fclose(f);
    long frames = n / 4, pos = 0;
    std::vector<double> b[8];
    for (auto &x : b) x.resize(block);
    std::map<clap_id, double> lastA, lastB;
    std::vector<std::pair<int, double>> evA, evB;
    double ea = 0, eb = 0;
    long en = 0;
    // run `secs` of audio (A then B, block by block), the main-thread timers as if in real time
    auto run = [&](double secs, bool measure) {
        double t = 0;
        ea = eb = 0;
        en = 0;
        static double fake = 0;
        while (t < secs) {
            for (int i = 0; i < block; ++i) {
                long k = (pos + i) % frames;
                for (int c = 0; c < 4; ++c) b[c][i] = sig[k * 4 + c];
            }
            pos += block;
            for (int stage = 0; stage < 2; ++stage) {
                const clap_plugin *p = ps[stage];
                double *in[2] = {stage == 0 ? b[0].data() : b[4].data(), stage == 0 ? b[1].data() : b[5].data()};
                double *sc[2] = {b[2].data(), b[3].data()};
                double *out[2] = {stage == 0 ? b[4].data() : b[6].data(), stage == 0 ? b[5].data() : b[7].data()};
                clap_audio_buffer ins[2] = {}, outs[1] = {};
                ins[0].channel_count = 2; ins[0].data64 = in;
                ins[1].channel_count = 2; ins[1].data64 = sc;
                outs[0].channel_count = 2; outs[0].data64 = out;
                EvList el;
                for (auto &e : stage == 0 ? evA : evB) el.v.push_back(mkev(e.first, e.second));
                (stage == 0 ? evA : evB).clear();
                clap_input_events ie{&el, in_size, in_get};
                clap_output_events oe{nullptr, out_push};
                clap_process pr{};
                pr.frames_count = (uint32_t)block;
                pr.audio_inputs = ins; pr.audio_inputs_count = 2;
                pr.audio_outputs = outs; pr.audio_outputs_count = 1;
                pr.in_events = &ie; pr.out_events = &oe;
                p->process(p, &pr);
            }
            if (measure)
                for (int i = 0; i < block; ++i) { ea += b[4][i] * b[4][i] + b[5][i] * b[5][i]; eb += b[6][i] * b[6][i] + b[7][i] * b[7][i]; en++; }
            t += block / sr;
            fake += block / sr;
            runTimers(A, lastA, fake);
            runTimers(B, lastB, fake);
            std::this_thread::sleep_for(std::chrono::microseconds((long)(block / sr * 1e6)));
        }
    };
    auto gain = [&](const clap_plugin *p) {
        const clap_plugin_params *pp = (const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
        double v;
        pp->get_value(p, 10, &v);
        return v;
    };
    evA.push_back({10, -6});
    evB.push_back({50, -30});   // B compresses hard
    run(1.5, false);
    // the two slots of the test chain, by chain order
    int64_t key = ts::ReaperBridge::hashKey("chain-test");
    int sa = -1, sb = -1;
    for (int s = 0; s < (int)ts::shm::get()->h.maxslot.load(); ++s) {
        ts::shm::Slot &sl = ts::shm::slot(s);
        if (sl.owner.load() >= 100000 && sl.track.load() == key && ts::now_sec() - sl.heart.load() < 1) {
            if (sl.order.load() == 0) sa = s;
            if (sl.order.load() == 1) sb = s;
        }
    }
    printf("slots: A %d, B %d\n", sa, sb);
    if (sa < 0 || sb < 0) return 1;
    int64_t idA = ts::shm::slot(sa).owner.load(), idB = ts::shm::slot(sb).owner.load();
    run(1.0, true);
    printf("before Match: A out %.2f dB, B out %.2f dB, B gain %.2f\n", 10 * log10(ea / en), 10 * log10(eb / en), gain(B));
    // B: Auto in Match mode, RMS, 2 s
    ts::shm::send(sb, ts::shm::C_SET, 40, 1, 0);   // P_AMODE = Match
    ts::shm::send(sb, ts::shm::C_SET, 3, 1, 0);    // P_MEAS = RMS
    ts::shm::send(sb, ts::shm::C_SET, 5, 2, 0);    // P_TIME = 2 s
    run(0.3, false);
    ts::shm::send(sb, ts::shm::C_AUTO, 1, 0, 0);
    run(3.5, false);
    printf("Match result %lld (2 = matched), B gain now %.2f\n", (long long)ts::shm::slot(sb).result.load(), gain(B));
    run(2.0, true);
    printf("after Match: A out %.2f dB, B out %.2f dB (difference %.2f dB)\n", 10 * log10(ea / en), 10 * log10(eb / en),
           10 * log10(eb / ea));
    // drive link: A drives B (B moves the other way when A's gain moves)
    ts::shm::send(sa, ts::shm::C_SET, 34, (double)idB, 0);   // A's P_DRIVE = B
    run(0.5, false);
    double gB0 = gain(B);
    evA.push_back({10, -3});   // A up by 3 dB
    run(0.5, false);
    printf("drive link: A -6 -> %.2f, B %.2f -> %.2f (moved %.2f, expect -3)\n", gain(A), gB0, gain(B), gain(B) - gB0);
    (void)idA;
    for (auto p : ps) { p->stop_processing(p); p->deactivate(p); p->destroy(p); }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: see the top of the file\n"); return 1; }
    Lib L;
    if (!L.load(argv[2])) return 1;
    int r = 1;
    if (!strcmp(argv[1], "info")) r = cmdInfo(L);
    else if (!strcmp(argv[1], "render")) r = cmdRender(L, argc, argv);
    else if (!strcmp(argv[1], "gui")) r = cmdGui(L, argc, argv);
    else if (!strcmp(argv[1], "chain")) r = cmdChain(L);
    else if (!strcmp(argv[1], "tunermute")) r = cmdTunerMute(L);
    else if (!strcmp(argv[1], "bypasskeep")) r = cmdBypassKeep(L);
    else if (!strcmp(argv[1], "key")) r = cmdKey(L, argc > 3 ? argv[3] : "tests/sig/hits.f64");
    L.entry->deinit();
    return r;
}
