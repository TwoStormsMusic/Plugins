// The TwoStorms CLAP entry: built once per plug-in (TS_ONLY_KIND: "TwoStorms Compressor.clap" and so on), and once
// with every plug-in in its factory (TwoStorms.clap, which the tests load). Add a plug-in by adding its kind() here
// and a ts_bundle line to CMakeLists.txt.
#include "core/plugin.h"
#include "plugins/compressor/comp_plugin.h"
#include "plugins/act/act_plugin.h"
#include "plugins/gainstage/gs_plugin.h"
#include "plugins/gate/gate_plugin.h"
#include "plugins/tremolo/tremolo_plugin.h"
#include "plugins/chorus/chorus_plugin.h"
#include "plugins/delay/delay_plugin.h"
#include "plugins/wah/wah_plugin.h"
#include "plugins/defitcher/defitcher_plugin.h"
#include "plugins/saturator/sat_plugin.h"
#include "plugins/eq/eq_plugin.h"
#include "plugins/multiband/mb_plugin.h"
#include "plugins/irloader/ir_plugin.h"
#include "plugins/tuner/tuner_plugin.h"
#include <clap/clap.h>
#include <cstring>

namespace {

#ifdef TS_ONLY_KIND
// a .clap of its own: just this plug-in
const ts::PluginKind &(*const s_kinds[])() = {&TS_ONLY_KIND};
#else
const ts::PluginKind &(*const s_kinds[])() = {
    &ts::comp::kind,
    &ts::actp::kind,
    &ts::gs::kind,
    &ts::gate::kind,
    &ts::trem::kind,
    &ts::chorus::kind,
    &ts::delay::kind,
    &ts::wah::kind,
    &ts::defitcher::kind,
    &ts::sat::kind,
    &ts::peq::kind,
    &ts::mb::kind,
    &ts::irl::kind,
    &ts::tuner::kind,
};
#endif
const uint32_t s_count = sizeof(s_kinds) / sizeof(s_kinds[0]);

uint32_t factoryCount(const clap_plugin_factory *) { return s_count; }

const clap_plugin_descriptor *factoryDesc(const clap_plugin_factory *, uint32_t i)
{
    return i < s_count ? &s_kinds[i]().desc : nullptr;
}

const clap_plugin *factoryCreate(const clap_plugin_factory *, const clap_host *host, const char *id)
{
    if (!clap_version_is_compatible(host->clap_version)) return nullptr;
    for (uint32_t i = 0; i < s_count; ++i) {
        const ts::PluginKind &k = s_kinds[i]();
        if (!strcmp(k.desc.id, id)) {
            ts::Plugin *p = k.create(host);
            return p ? p->clapPlugin() : nullptr;
        }
    }
    return nullptr;
}

const clap_plugin_factory s_factory = {factoryCount, factoryDesc, factoryCreate};

bool entryInit(const char *) { return true; }
void entryDeinit() {}
const void *entryGetFactory(const char *id)
{
    if (!strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &s_factory;
    return nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryGetFactory};

#if defined(__linux__)
// The library's static destructors are registered with __cxa_atexit under this library's handle; GCC's start-up files
// run them when the host unloads the library (dlclose), Zig's don't, and then they would run at the host's exit, after
// the code is gone (a crash on quitting). Run them, and take them off the list, as the library goes.
extern "C" {
extern void *__dso_handle __attribute__((visibility("hidden")));
void __cxa_finalize(void *);
}
__attribute__((destructor)) static void tsFinalize() { __cxa_finalize(&__dso_handle); }
#endif
