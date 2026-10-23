#pragma once
#include "../../core/params.h"
#include "../../core/presets.h"

namespace ts {
namespace comp {

const ParamTable &table();
const std::vector<FactoryPreset> &factoryPresets();
// a value as the window shows it (cfmt in the JSFX)
void fmt(int p, double v, char *buf, int n);

} // namespace comp
} // namespace ts
