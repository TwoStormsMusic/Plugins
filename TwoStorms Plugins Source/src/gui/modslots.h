// The eight modulation slots the Delay and the Saturator share (parameters from a base, 170 or 190, eight per slot: type, target,
// target setting, amount, rate / attack, release, shape / source / CC, tempo sync): the window's rows.
#pragma once
#include "../core/eel.h"
#include "../core/plugin.h"
#include "ui.h"
#include <atomic>

namespace ts {
namespace gui {

constexpr int MOD_B0 = 170, MOD_N = 8, MOD_STRIDE = 8;
enum { MS_TYPE = 0, MS_TARGET, MS_PARAM, MS_AMT, MS_RATE, MS_RATE2, MS_OPT, MS_SYNC };
inline int modParam(int s, int k) { return MOD_B0 + s * MOD_STRIDE + k; }
inline int modParamB(int b, int s, int k) { return b + s * MOD_STRIDE + k; }

struct ModSlotLabels {
    const char *const *targets; int nTargets;   // the target menu (0 = Global)
    const char *const *globals; int nGlobals;   // what a Global slot can move
    const char *const *params; int nParams;     // what a band / line slot can move
    const char *const *sources;                 // the follower's three sources
    int base = MOD_B0;                          // the first slot's first parameter
};

// the column titles at y and the eight rows below (dy apart), across [x, x + w); mv: each slot's value now
void modSlots(Ui &ui, Plugin &P, float x, float y, float w, float dy, const ModSlotLabels &L, const std::atomic<float> *mv, bool live);
// how many slots are on
int modSlotsOn(const Plugin &P, int base = MOD_B0);

} // namespace gui
} // namespace ts
