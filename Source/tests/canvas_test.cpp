#include "../src/gui/canvas.h"
#include "../src/gui/font.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <vector>
#include <cmath>
using namespace ts::gui;
int main(int argc, char** argv) {
  Canvas c; c.resize(640, 360);
  c.clear(Col::hex(0x0b0b0d));
  c.shadow(40, 40, 260, 140, 10, 14, Col(0,0,0,0.6f));
  c.fillRoundRect(40, 40, 260, 140, 10, Paint::vertical(40, Col::hex(0x1d1b1d), 180, Col::hex(0x141315)));
  c.strokeRoundRect(40, 40, 260, 140, 10, 1, Paint(Col::hex(0x2e2b2a)));
  c.fillRoundRect(60.3f, 60.5f, 120, 28, 6, Paint::vertical(60, Col::hex(0xffa040), 88, Col::hex(0xe67a10)));
  c.text(font(F_SEMIBOLD), 13, 76, 79, "Presets  \xE2\x96\xBE Ratio 4.00 : 1", Col::hex(0x1a0d00));
  c.text(font(F_BOLD), 18, 60, 120, "TWOSTORMS COMPRESSOR", Col::hex(0xff9424));
  c.text(font(F_REGULAR), 12, 60, 145, "Threshold -18.0 dB   Attack 10 ms   0123456789", Col::hex(0xf0ebe3));
  c.text(font(F_MEDIUM), 12, 60, 162, "-12.0 dBFS  1-2  +0.00 dB", Col::hex(0xb48cff), true);
  std::vector<Pt> pts; for (int i=0;i<200;i++){ float x=330+i*1.4f; pts.push_back({x, 200-80*std::sin(i*0.06f)*std::exp(-i*0.004f)}); }
  c.strokePolyline(pts.data(), (int)pts.size(), 2.0f, Paint(Col::hex(0xff9424)));
  c.beginPath(); c.moveTo(330,300); for (auto&p:pts) c.lineTo(p.x, p.y+90); c.lineTo(pts.back().x,300); c.fillPath(Paint::vertical(200, Col::hex(0x9b7bff,0.5f), 300, Col::hex(0x9b7bff,0.05f)));
  c.fillCircle(100, 280, 30, Paint(Col::hex(0x3fd17a)));
  c.strokeCircle(180, 280, 30, 3, Paint(Col::hex(0xff9424)));
  c.line(240, 250, 300, 320, 1, Col(1,1,1,1));
  c.fillRect(10.5f, 330.25f, 200.3f, 6.5f, Paint(Col::hex(0x3fd17a)));
  std::vector<unsigned char> rgb(640*360*3);
  for (int i=0;i<640*360;i++){ uint32_t v=c.pixels()[i]; rgb[i*3]=(v>>16)&255; rgb[i*3+1]=(v>>8)&255; rgb[i*3+2]=v&255; }
  stbi_write_png(argv[1], 640, 360, 3, rgb.data(), 640*3);
}
