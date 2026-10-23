// Presets, shared by every TwoStorms plug-in.
// Factory presets come with the plug-in (a menu path and a "p=v p=v" string, as in the JSFX
// suite's presets.py table); a preset starts from every A/B setting's default, so it always
// sounds the same. User presets are the JSFX suite's own files (the helper script's format), so the
// JSFX and CLAP versions of a plug-in share them:
//   <REAPER resource path>/Data/TwoStorms/Presets/TwoStorms <Plug-in>.txt   (inside REAPER)
//   ~/.config/TwoStorms/Presets/... or %APPDATA%\TwoStorms\Presets\...      (other hosts)
// One preset per [name] section: "id=value" lines (slider numbers), and an optional "extra=" line
// of numbers a plug-in adds (the IR Loader's file paths). At most 40 per plug-in, sorted by name.
#pragma once
#include <string>
#include <initializer_list>
#include <utility>
#include <vector>

namespace ts {

struct FactoryPreset {
    std::string path;     // "Live rig/Clean bass (opto)" (submenus split on '/')
    std::string values;   // "50=-24 51=4 ..."
};

struct UserPreset {
    std::string name;
    std::vector<std::pair<int, double>> values;
    std::vector<double> extra;   // the plug-in's extras ("extra=" line)
};

// parse "p=v p=v ..." (the JSFX's pr_str)
std::vector<std::pair<int, double>> parsePresetString(const std::string &s);
// the other way: {{P_THR, -24}, {P_RAT, 4}} -> "50=-24 51=4" (factory presets written with the settings' names)
std::string presetStr(std::initializer_list<std::pair<int, double>> kv);
std::string presetStr(const std::vector<std::pair<int, double>> &kv);

class PresetStore {
public:
    // plugName: the file name ("Compressor"); resourcePath: REAPER's (or empty)
    void setup(const std::string &plugName, const std::string &resourcePath);
    const std::string &dir() const { return dir_; }
    std::string file() const;
    std::vector<UserPreset> load() const;
    static constexpr int MAXN = 40;       // presets per plug-in (as the helper script)
    static std::string cleanName(const std::string &s);
    bool exists(const std::string &name) const;   // case-insensitive, as the helper
    // adds or replaces (by name, ignoring case); false if the file can't be written or 40 are kept
    bool save(const UserPreset &p, bool *full = nullptr);
    bool remove(const std::string &name);
    static std::string defaultDir();

private:
    std::string name_, dir_;
    bool write(const std::vector<UserPreset> &all) const;
};

} // namespace ts
