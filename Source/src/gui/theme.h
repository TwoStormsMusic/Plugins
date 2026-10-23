// The TwoStorms look: black, orange, purple as the third color, green as the fourth.
#pragma once
#include "canvas.h"

namespace ts {
namespace gui {
namespace th {

inline Col bg() { return Col::hex(0x09090b); }          // window
inline Col bg2() { return Col::hex(0x0e0e10); }         // window, lower end of its gradient
inline Col panel() { return Col::hex(0x141416); }       // cards
inline Col panel2() { return Col::hex(0x101012); }
inline Col raised() { return Col::hex(0x1d1c1f); }      // buttons
inline Col raised2() { return Col::hex(0x171619); }
inline Col hover() { return Col::hex(0x26242a); }
inline Col well() { return Col::hex(0x08080a); }        // insets: bars, value boxes, displays
inline Col border() { return Col::hex(0x28262a); }
inline Col borderHi() { return Col::hex(0x3a3640); }
inline Col grid() { return Col::hex(0x19181c); }
inline Col gridHi() { return Col::hex(0x26242a); }
inline Col text() { return Col::hex(0xf2ede6); }
inline Col dim() { return Col::hex(0xa69e96); }
inline Col faint() { return Col::hex(0x6a645f); }
inline Col ghost() { return Col::hex(0x3e3a38); }
inline Col orange() { return Col::hex(0xff9424); }
inline Col orangeHi() { return Col::hex(0xffb35c); }
inline Col orangeLo() { return Col::hex(0xd96a0b); }
inline Col orangeInk() { return Col::hex(0x1c0e02); }   // text on orange
inline Col purple() { return Col::hex(0xa78bff); }
inline Col purpleHi() { return Col::hex(0xc8b5ff); }
inline Col purpleLo() { return Col::hex(0x6f4fe0); }
inline Col green() { return Col::hex(0x3ddc84); }
inline Col greenLo() { return Col::hex(0x1f9c57); }
inline Col amber() { return Col::hex(0xffbf47); }
inline Col red() { return Col::hex(0xff4f5e); }
inline Col redLo() { return Col::hex(0xb3242f); }

} // namespace th
} // namespace gui
} // namespace ts
