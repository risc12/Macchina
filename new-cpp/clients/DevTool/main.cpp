//
//  main.cpp
//  Macchina — new-cpp/clients/FontSize
//
//  Usage:   FontSize [serial]        (default serial 39195855)
//  Needs NIHardwareAgent RUNNING and a Studio plugged in.
//

#include "core/Controller.hpp"
#include "core/transport-macos/CFMessagePortTransport.hpp"
#include "gfx/Surface.hpp"
#include "studio/StudioController.hpp"
#include "studio/StudioLayout.hpp"

#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

using namespace macchina;

namespace {

float roundToPrecision(float value, float precision) {
  return std::round(value / precision) * precision;
}

// float roundToHalf(float value) {
//   roundToPrecision(value, 0.5);
// }
//
// float roundToQuarter(float value) {
//   roundToPrecision(value, 0.25);
// }

const uint16_t kBlack = gfx::rgb565(0, 0, 0);
const uint16_t kWhite = gfx::rgb565(255, 255, 255);
const uint16_t kOrange = gfx::rgb565(255, 140, 0);
// const uint16_t kGrey   = gfx::rgb565(120, 120, 120);
const uint16_t kDim = gfx::rgb565(50, 50, 50);
const uint16_t kDimmer = gfx::rgb565(20, 20, 20);

const uint16_t kBlue = gfx::rgb565(30, 30, 255);

volatile sig_atomic_t gStopRequested = 0;
void onSigint(int) { gStopRequested = 1; }

template <typename T> T remap(T value, T inMin, T inMax, T outMin, T outMax) {
  return (T)(((float)(value - inMin) / (inMax - inMin)) * (outMax - outMin) +
             outMin);
}

struct RGB {
  int r, g, b;
};

RGB hslToRgb(float h, float s, float l) {
  s /= 100.0f;
  l /= 100.0f;

  float c = (1 - std::abs(2 * l - 1)) * s;
  float x = c * (1 - std::abs(std::fmod(h / 60.0f, 2) - 1));
  float m = l - c / 2;

  float r, g, b;
  if (h < 60) {
    r = c;
    g = x;
    b = 0;
  } else if (h < 120) {
    r = x;
    g = c;
    b = 0;
  } else if (h < 180) {
    r = 0;
    g = c;
    b = x;
  } else if (h < 240) {
    r = 0;
    g = x;
    b = c;
  } else if (h < 300) {
    r = x;
    g = 0;
    b = c;
  } else {
    r = c;
    g = 0;
    b = x;
  }

  return {(int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255)};
}

struct State {
  int lastId = 0;

  float selectedSize = 2;
  float x = 8;
  float y = gfx::Surface::textHeight(2) + 16 + 8;

  float h = 0;  // Hue (0-360)
  float s = 50; // Saturation (0-100)
  float l = 50; // Lightness (0-100)

  RGB padColor[16] = {};
  bool padActive[16] = {};
};

void handleInput(State &st, const InputEvent &e) {
  st.lastId = e.id;

  if (e.type == InputEvent::Type::Pad) {

    int pad = studio::padIndexForInputId(e.id);

    if (pad > -1) {

      // Threshold out sensor noise: some pads (hi pad 4) trickle tiny
      // pressure values while untouched, which a brightness floor would
      // otherwise turn into a permanently lit pad.
      if(e.pressure > 0.5f) {
        st.padActive[pad] = true;
      }

      st.padColor[pad] = hslToRgb(st.h, st.s, st.l);
    }
  }

  if (e.id == (int)studio::kMasterKnobId) {
    st.selectedSize = std::max(0.0f, st.selectedSize + e.delta);
  }

  if (e.id == studio::controlByM2Name("AUTO_WRITE")->buttonId) {
    st.selectedSize = roundToPrecision(st.selectedSize, 0.5);
    st.x = roundToPrecision(st.x, 1);
    st.y = roundToPrecision(st.y, 1);
  }

  if (e.id == studio::controlByM2Name("ENTER")->buttonId) {
    for (int p = 0; p < studio::kPadCount; p++) {
      st.padActive[p] = true;
      st.padColor[p] = hslToRgb(st.h, st.s, st.l);
    }
  }

  if (e.id == studio::controlByM2Name("_KNOB_1")->buttonId) {
    st.h = std::fmod(st.h + (e.delta * 360), 360.0f);
    if (st.h < 0)
      st.h += 360;
  }

  if (e.id == studio::controlByM2Name("_KNOB_2")->buttonId) {
    st.s = std::clamp(st.s + (e.delta * 100), 0.0f, 100.0f);
  }

  if (e.id == studio::controlByM2Name("_KNOB_3")->buttonId) {
    st.l = std::clamp(st.l + (e.delta * 100), 0.0f, 100.0f);
  }

  if (e.id == studio::controlByM2Name("_KNOB_7")->buttonId) {
    st.x = st.x + (e.delta * 100);
  }

  if (e.id == studio::controlByM2Name("_KNOB_8")->buttonId) {
    st.y = st.y + (e.delta * 100);
  }
}

// clang-format off
void drawLeft(gfx::Surface &s, const State &st) {
  s.clear(kBlack);
  
  RGB color = hslToRgb(st.h, st.s, st.l);

  int pt = 8;

  for (float i = 1; i < 9; i = std::round(i + i / 2)) {
    char line[64];
    snprintf(line, 64, "Size %.2f", i);
    s.drawText(8, pt, line, gfx::rgb565(color.r, color.g, color.b), i);

    pt = pt + s.textHeight(i) + 8;
  }

  int x = 0;
  int y = 248;
  int padding = 10;
  int gap = 10;
  int rect_height = 25;

  int rect_width = (s.width - 2 * padding - 3 * gap) / 4;
  int spacing = rect_width + gap;

    // H - Container
  s.fillRect(x + padding + spacing * 0, y, rect_width, rect_height, kDim, kWhite);
  // H - Gradient
  for (int i = 0; i < rect_width - 2; i++) {
    float hue = remap((float)i, 0.0f, (float)(rect_width - 3), 0.0f, 360.0f);
    RGB hueColor = hslToRgb(hue, 100.0f, 50.0f);
    s.fillRect(x + padding + spacing * 0 + i + 1, y + 1, 1, rect_height - 2, gfx::rgb565(hueColor.r, hueColor.g, hueColor.b));
  }

    // S - Container
  s.fillRect(x + padding + spacing * 1, y, rect_width, rect_height, kDim, kWhite);
  // S - Gradient (saturation at current hue)
  for (int i = 0; i < rect_width - 2; i++) {
    float saturation = remap((float)i, 0.0f, (float)(rect_width - 3), 0.0f, 100.0f);
    RGB satColor = hslToRgb(st.h, saturation, 50.0f);
    s.fillRect(x + padding + spacing * 1 + i + 1, y + 1, 1, rect_height - 2, gfx::rgb565(satColor.r, satColor.g, satColor.b));
  }
  // L
  s.fillRect(x + padding + spacing * 2, y, rect_width, rect_height, kDim, kWhite);

  // L - Container
  s.fillRect(x + padding + spacing * 2, y, rect_width, rect_height, kDim, kWhite);
  // L - Gradient (lightness at current hue and saturation)
  for (int i = 0; i < rect_width - 2; i++) {
    float lightness = remap((float)i, 0.0f, (float)(rect_width - 3), 0.0f, 100.0f);
    RGB lightColor = hslToRgb(st.h, st.s, lightness);
    s.fillRect(x + padding + spacing * 2 + i + 1, y + 1, 1, rect_height - 2, gfx::rgb565(lightColor.r, lightColor.g, lightColor.b));
  }

  // Inactive - Show RGB values
  s.fillRect(x + padding + spacing * 3, y, rect_width, rect_height, kDimmer, kDim);
  char rgbText[32];
  snprintf(rgbText, 32, "R:%3d G:%3d B:%3d", color.r, color.g, color.b);
  s.drawText(x + padding + spacing * 3 + 2, y + 5, rgbText, kWhite, 1);

  // Value indicators
  s.fillRect(x + padding + spacing * 0 + (int)remap(st.h, 0.0f, 360.0f, 0.0f, (float)(rect_width - 2)), y, 2, rect_height, kWhite);
  s.fillRect(x + padding + spacing * 1 + (int)remap(st.s, 0.0f, 100.0f, 0.0f, (float)(rect_width - 2)), y, 2, rect_height, kWhite);
  s.fillRect(x + padding + spacing * 2 + (int)remap(st.l, 0.0f, 100.0f, 0.0f, (float)(rect_width - 2)), y, 2, rect_height, kWhite);
}
// clang-format on

void drawRight(gfx::Surface &s, const State &st) {
  s.clear(kBlack);

  s.fillRect(0, 0, s.width, s.textHeight(2) + 16, kDim);

  char line[64];
  snprintf(line, 64, "Last id: %i                  %i    %i", st.lastId,
           s.width, s.height);
  s.drawText(8, 8, line, kBlack, 2);

  snprintf(line, 64, "X: %.2f - Y: %.2f - F %.2F", st.x, st.y, st.selectedSize);

  // Render text above
  if (st.y > 240) {
    s.drawText(st.x + 5, st.y - 5 - s.textHeight(st.selectedSize), line,
               kOrange, st.selectedSize);
    s.fillRect(st.x, st.y, 50, 2, kBlue);
    s.fillRect(st.x, st.y - 20, 2, 20, kBlue);
  } else {
    s.drawText(st.x + 5, st.y + 7, line, kOrange, st.selectedSize);
    s.fillRect(st.x, st.y, 50, 2, kBlue);
    s.fillRect(st.x, st.y, 2, 20, kBlue);
  }

  s.fillRect(st.x, st.y, 1, 1, kWhite);
}

void lightLeds(studio::StudioController &studio, const State &st) {
  studio.setAllLeds(0);

  for (int p = 0; p < studio::kPadCount; p++) {
    if (st.padActive[p]) {
      studio.setPadLed((size_t)p, st.padColor[p].r, st.padColor[p].g,
                       st.padColor[p].b);
    }
  }

  studio.flushLeds();
}

} // namespace

int main(int argc, const char *argv[]) {
  std::string serial = (argc > 1) ? argv[1] : "39195855";

  CFMessagePortTransport transport;
  studio::StudioController studio(transport, serial);
  State st;
  studio.onInput = [&st](const InputEvent &e) { handleInput(st, e); };

  fprintf(stderr, "connecting to Maschine Studio (serial %s)…\n",
          serial.c_str());
  if (!studio.connect()) {
    fprintf(stderr, "connect failed — is NIHardwareAgent running and the "
                    "Studio plugged in?\n");
    return 1;
  }

  signal(SIGINT, onSigint);

  gfx::Surface left(studio.displayWidth(), studio.displayHeight());
  gfx::Surface right(studio.displayWidth(), studio.displayHeight());

  while (!gStopRequested) {
    transport.runFor(1.0 / 60);

    drawLeft(left, st);
    drawRight(right, st);
    studio.drawDisplay(0, left.pixels);
    studio.drawDisplay(1, right.pixels);

    lightLeds(studio, st); // LEDs are cheap — update on every change
  }

  studio.setAllLeds(0);
  studio.flushLeds();
  for (int i = 0; i < studio.displayCount(); i++)
    studio.fillDisplay(i, gfx::rgb565(0, 0, 0));
  fprintf(stderr, "\nblanked, bye.\n");
  return 0;
}
