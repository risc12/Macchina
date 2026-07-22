//
//  main.cpp
//  Macchina — new-cpp/clients/HelloScreen
//
//  The smallest possible text-on-glass proof: rasterize "HELLO WORLD" into an
//  RGB565 surface host-side (gfx/Surface — the device has no font engine, see
//  new/docs/rendering-pipeline.md) and push it through the normal draw path.
//  Draws once and exits; the image stays on the panels.
//
//  Usage:   HelloScreen [serial]        (default serial 39195855)
//  Needs NIHardwareAgent RUNNING and a Studio plugged in.
//

#include "core/transport-macos/CFMessagePortTransport.hpp"
#include "gfx/Surface.hpp"
#include "studio/StudioController.hpp"

#include <cstdio>
#include <string>

using namespace macchina;

namespace {

// Centered text helper.
void centerText(gfx::Surface & s, int y, std::string_view text, uint16_t color, int scale)
{
    s.drawText((s.width - gfx::Surface::textWidth(text, scale)) / 2, y, text, color, scale);
}

} // namespace

int main(int argc, const char * argv[])
{
    std::string serial = (argc > 1) ? argv[1] : "39195855";

    CFMessagePortTransport   transport;
    studio::StudioController studio(transport, serial);

    fprintf(stderr, "connecting to Maschine Studio (serial %s)…\n", serial.c_str());
    if (!studio.connect())
    {
        fprintf(stderr, "connect failed — is NIHardwareAgent running and the Studio plugged in?\n");
        return 1;
    }

    const uint16_t kBlack  = gfx::rgb565(0, 0, 0);
    const uint16_t kWhite  = gfx::rgb565(255, 255, 255);
    const uint16_t kOrange = gfx::rgb565(255, 140, 0);     // Maschine accent
    const uint16_t kGrey   = gfx::rgb565(110, 110, 110);

    // Left display: the hello.
    gfx::Surface left(studio.displayWidth(), studio.displayHeight(), kBlack);
    centerText(left, 80, "HELLO WORLD", kWhite, 6);
    left.fillRect(60, 150, left.width - 120, 3, kOrange);   // underline flourish
    centerText(left, 170, "RENDERED HOST-SIDE BY...", kGrey, 2);
    studio.drawDisplay(0, left.pixels);

    // Right display: prove coordinates, colour, and the font in one glance.
    gfx::Surface right(studio.displayWidth(), studio.displayHeight(), kBlack);
    centerText(right, 40, "480 X 272", kOrange, 4);
    centerText(right, 90, "RGB565 BIG-ENDIAN ON THE WIRE", kGrey, 2);
    centerText(right, 130, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", kWhite, 2);
    centerText(right, 155, "0123456789 .,:!?-+=/()<>*#%", kWhite, 2);
    right.fillRect(0, 250, right.width, 22, kOrange);
    studio.drawDisplay(1, right.pixels);

    fprintf(stderr, "drawn — the text stays on the panels. Bye.\n");
    return 0;
}
