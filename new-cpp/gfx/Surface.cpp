//
//  Surface.cpp
//  Macchina — new-cpp/gfx
//

#include "gfx/Surface.hpp"

#include <algorithm>
#include <cmath>

namespace macchina::gfx {

namespace {

// 5×7 font, one row per byte, bit 4 = leftmost column. Glyphs are hand-drawn
// (binary literals ARE the glyph — squint and you can proofread them).
struct Glyph
{
    char    ch;
    uint8_t rows[7];
};

// clang-format off
constexpr Glyph kFont[] = {
    { ' ', { 0b00000,0b00000,0b00000,0b00000,0b00000,0b00000,0b00000 } },
    { 'A', { 0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001 } },
    { 'B', { 0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110 } },
    { 'C', { 0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110 } },
    { 'D', { 0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110 } },
    { 'E', { 0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111 } },
    { 'F', { 0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000 } },
    { 'G', { 0b01110,0b10001,0b10000,0b10111,0b10001,0b10001,0b01111 } },
    { 'H', { 0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001 } },
    { 'I', { 0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110 } },
    { 'J', { 0b00111,0b00010,0b00010,0b00010,0b00010,0b10010,0b01100 } },
    { 'K', { 0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001 } },
    { 'L', { 0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111 } },
    { 'M', { 0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001 } },
    { 'N', { 0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001 } },
    { 'O', { 0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110 } },
    { 'P', { 0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000 } },
    { 'Q', { 0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101 } },
    { 'R', { 0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001 } },
    { 'S', { 0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110 } },
    { 'T', { 0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100 } },
    { 'U', { 0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110 } },
    { 'V', { 0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100 } },
    { 'W', { 0b10001,0b10001,0b10001,0b10101,0b10101,0b10101,0b01010 } },
    { 'X', { 0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001 } },
    { 'Y', { 0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100 } },
    { 'Z', { 0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111 } },
    { '0', { 0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110 } },
    { '1', { 0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110 } },
    { '2', { 0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111 } },
    { '3', { 0b11110,0b00001,0b00001,0b01110,0b00001,0b00001,0b11110 } },
    { '4', { 0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010 } },
    { '5', { 0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110 } },
    { '6', { 0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110 } },
    { '7', { 0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000 } },
    { '8', { 0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110 } },
    { '9', { 0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100 } },
    { '.', { 0b00000,0b00000,0b00000,0b00000,0b00000,0b01100,0b01100 } },
    { ',', { 0b00000,0b00000,0b00000,0b00000,0b01100,0b00100,0b01000 } },
    { ':', { 0b00000,0b01100,0b01100,0b00000,0b01100,0b01100,0b00000 } },
    { '!', { 0b00100,0b00100,0b00100,0b00100,0b00100,0b00000,0b00100 } },
    { '?', { 0b01110,0b10001,0b00001,0b00110,0b00100,0b00000,0b00100 } },
    { '-', { 0b00000,0b00000,0b00000,0b11111,0b00000,0b00000,0b00000 } },
    { '+', { 0b00000,0b00100,0b00100,0b11111,0b00100,0b00100,0b00000 } },
    { '=', { 0b00000,0b00000,0b11111,0b00000,0b11111,0b00000,0b00000 } },
    { '/', { 0b00001,0b00010,0b00010,0b00100,0b01000,0b01000,0b10000 } },
    { '(', { 0b00010,0b00100,0b01000,0b01000,0b01000,0b00100,0b00010 } },
    { ')', { 0b01000,0b00100,0b00010,0b00010,0b00010,0b00100,0b01000 } },
    { ']', { 0b01110,0b00010,0b00010,0b00010,0b00010,0b00010,0b01110 } },
    { '[', { 0b01110,0b01000,0b01000,0b01000,0b01000,0b01000,0b01110 } },
    { '<', { 0b00010,0b00100,0b01000,0b10000,0b01000,0b00100,0b00010 } },
    { '>', { 0b01000,0b00100,0b00010,0b00001,0b00010,0b00100,0b01000 } },
    { '_', { 0b00000,0b00000,0b00000,0b00000,0b00000,0b00000,0b11111 } },
    { '\'',{ 0b00100,0b00100,0b01000,0b00000,0b00000,0b00000,0b00000 } },
    { '"', { 0b01010,0b01010,0b00000,0b00000,0b00000,0b00000,0b00000 } },
    { '*', { 0b00000,0b01010,0b00100,0b11111,0b00100,0b01010,0b00000 } },
    { '#', { 0b01010,0b01010,0b11111,0b01010,0b11111,0b01010,0b01010 } },
    { '%', { 0b11001,0b11010,0b00010,0b00100,0b01000,0b01011,0b10011 } },
};
// Unknown characters: hollow box.
constexpr uint8_t kBoxGlyph[7] = { 0b11111,0b10001,0b10001,0b10101,0b10001,0b10001,0b11111 };
// clang-format on

const uint8_t * glyphRows(char ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    for (const auto & g : kFont)
        if (g.ch == ch)
            return g.rows;
    return kBoxGlyph;
}

constexpr int kGlyphW = 5, kGlyphH = 7, kSpacing = 1;

} // namespace


void Surface::clear(uint16_t color)
{
    std::fill(pixels.begin(), pixels.end(), color);
}

void Surface::fillRect(int x, int y, int w, int h, uint16_t color)
{
    int x0 = std::max(x, 0), y0 = std::max(y, 0);
    int x1 = std::min(x + w, width), y1 = std::min(y + h, height);
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            pixels[(size_t)yy * width + xx] = color;
}

void Surface::fillRect(int x, int y, int w, int h, uint16_t fillColor, uint16_t outlineColor)
{
    int x0 = std::max(x, 0), y0 = std::max(y, 0);
    int x1 = std::min(x + w, width), y1 = std::min(y + h, height);
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            if(xx == x || yy == y || xx == x + w - 1 || yy == y + h - 1 ) {
                pixels[(size_t)yy * width + xx] = outlineColor;
            } else {
                pixels[(size_t)yy * width + xx] = fillColor;
            }
}

namespace {

// Cell boundary along one axis, rounded independently per index so that
// consecutive cells (i, i+1) always share an edge — no gaps or overlaps even
// when `scale` is fractional (e.g. 1.5).
int cellEdge(int origin, int i, float scale)
{
    return origin + (int)std::lround(i * (double)scale);
}

} // namespace

void Surface::drawChar(int x, int y, char ch, uint16_t color, float scale)
{
    const uint8_t * rows = glyphRows(ch);
    for (int r = 0; r < kGlyphH; r++)
        for (int c = 0; c < kGlyphW; c++)
            if (rows[r] & (1 << (kGlyphW - 1 - c)))
            {
                int x0 = cellEdge(x, c, scale), x1 = cellEdge(x, c + 1, scale);
                int y0 = cellEdge(y, r, scale), y1 = cellEdge(y, r + 1, scale);
                fillRect(x0, y0, x1 - x0, y1 - y0, color);
            }
}

void Surface::drawText(int x, int y, std::string_view text, uint16_t color, float scale)
{
    double xf = x;
    for (char ch : text)
    {
        drawChar((int)std::lround(xf), y, ch, color, scale);
        xf += (kGlyphW + kSpacing) * (double)scale;
    }
}

int Surface::textWidth(std::string_view text, float scale)
{
    if (text.empty())
        return 0;
    // Mirrors drawText/drawChar's actual rounding steps (cursor rounded per
    // character, glyph width rounded via cellEdge) rather than a closed-form
    // guess — a single round of the whole-string formula can be off by a
    // pixel from what's actually drawn, since round(a)+round(b) != round(a+b).
    double advance    = (kGlyphW + kSpacing) * (double)scale;
    int    lastOrigin = (int)std::lround((double)(text.size() - 1) * advance);
    return lastOrigin + (int)std::lround(kGlyphW * (double)scale);
}

int Surface::textHeight(float scale)
{
    return (int)std::lround(kGlyphH * (double)scale);
}

} // namespace macchina::gfx
