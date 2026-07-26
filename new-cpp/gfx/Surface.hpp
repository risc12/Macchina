//
//  Surface.hpp
//  Macchina — new-cpp/gfx
//
//  The tiniest host-side rasterizer: an RGB565 pixel surface with rect fills
//  and a built-in 5×7 pixel font (uppercase+digits+punctuation; lowercase
//  maps to uppercase). Pure pixels — no transport, no device, no platform.
//
//  Why this exists: the Studio's screens are dumb framebuffers — no font or
//  character generator; M2 rasterizes everything host-side too (FreeType +
//  its NWL widget kit — see new/docs/rendering-pipeline.md). This is the
//  same idea at 1% of the size; a real font stack (stb_truetype) can slot in
//  later behind the same Surface.
//

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace macchina::gfx {

/// Native little-endian RGB565 from 8-bit components (the byte swap the wire
/// wants happens in the Studio draw path, not here).
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

struct Surface
{
    int                   width, height;
    std::vector<uint16_t> pixels;   // row-major, width*height

    Surface(int w, int h, uint16_t fill = 0) : width(w), height(h), pixels((size_t)w * h, fill) {}

    void clear(uint16_t color);
    void fillRect(int x, int y, int w, int h, uint16_t fillColor);
    void fillRect(int x, int y, int w, int h, uint16_t fillColor, uint16_t outlineColor);


    /// Draw one 5×7 glyph at pixel scale `scale` (each font pixel becomes an
    /// approximately scale×scale block; fractional scale is rounded per cell
    /// boundary so adjacent glyph pixels still tile without gaps/overlap).
    /// Unknown characters render as a hollow box.
    void drawChar(int x, int y, char ch, uint16_t color, float scale = 1);

    /// Left-aligned text, 1 font-pixel spacing between glyphs.
    void drawText(int x, int y, std::string_view text, uint16_t color, float scale = 1);

    /// Width/height in pixels that drawText will cover — for centering.
    static int textWidth(std::string_view text, float scale = 1);
    static int textHeight(float scale = 1);
};

} // namespace macchina::gfx
