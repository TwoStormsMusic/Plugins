// The window side of the built-in Act (the JSFX suite's g_* functions): titles, finales, Auto's
// state, drive links, names. Main thread only. An instance is addressed by s: -1 = this one, or a
// slot number for another TwoStorms plug-in (how the Gain Stage will use it).
#pragma once
#include "act.h"
#include "eel.h"
#include "shm.h"
#include <string>
#include <vector>

namespace ts {

class Plugin;

class ActView {
public:
    explicit ActView(Plugin &p);
    void refresh();   // once per frame: time, my slot, finales

    double now = 0;
    int myslot = -1;
    int64_t myid = 0;

    int ds(int s) const { return s < 0 ? myslot : s; }
    double pv(int s, int p) const;
    int nch(int s) const;
    bool used(int s, int c) const;
    bool linked(int s, int k) const;
    // gain groups: a linked pair is one group (one fader), an unlinked used channel its own;
    // ga = first channel, gn = 1 or 2; returns how many
    int groups(int s, int *ga, int *gn) const;
    int findId(int64_t id, int hint = -1) const;
    bool live(int s) const;
    bool named(int d) const;
    std::string trackName(int d) const;
    std::string prevName(int d) const;
    static const char *subName(int sk);
    bool dupKind(int d) const;
    std::string pointTitle(int d) const;
    std::string titleOfId(int64_t id) const;
    bool sameChain(int a, int b) const;
    int fxPos(int d) const;

    // finales: in each chain, the Acts set to Always, or else its last Act set to Auto
    std::vector<int> FIN, FKEY;
    void finCompute();
    int finOfChain(int d) const;
    bool finTail(int d) const;
    int fin() const { return myslot >= 0 && myslot < (int)FIN.size() ? FIN[myslot] : 0; }
    std::string finaleStatus(bool *warn) const;

    // Auto
    bool autoBusy(int s) const;
    int autoMode(int s) const;
    int refState(int s) const;          // 0 fine, 1 no reference, 2 the reference is after this one
    double aimTick(int s) const;
    void autoClick(int s);
    const char *autoLabel(int s) const;
    std::string autoTimeText(int s, bool *busy, bool *nosig) const;
    int prevSlot(int d) const;
    std::vector<int> chainPoints(int d) const;
    std::string valueText(int s, bool *ok) const;   // Match / Reference: the level and the name
    std::string srcText(int s, int a, bool compact) const;

    // drive link
    void setDrive(int s, int64_t idb);
    std::string driveLabel(int s) const;
    bool driveIsParam(int s) const { return iround(pv(s, P_DRIVE)) == DRIVE_PARAM; }

    // meters
    double meterRange() const { return act_range_db(pv(-1, P_RANGE)); }

private:
    Plugin &P;
    int mx() const;
};

} // namespace ts
