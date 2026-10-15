// Z3CloudPlay - Check baked glyph silhouettes against stb's cubic-capable rasterizer.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "gfx/font.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

static std::string read(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) return 2;
    const std::string source = read(argv[1]), baked = read(argv[2]);
    stbtt_fontinfo info{};
    hui::gfx::Font font;
    if (source.empty() || !stbtt_InitFont(&info,
            reinterpret_cast<const unsigned char *>(source.data()), 0) || !font.load(baked))
    {
        std::fprintf(stderr, "cannot load source or baked font: %s\n", font.error().c_str());
        return 1;
    }
    hui::gfx::font_format::Header header{};
    std::memcpy(&header, baked.data(), sizeof(header));
    const float scale = stbtt_ScaleForMappingEmToPixels(&info, header.pixel_size);
    int failures = 0, tested = 0;
    double worst = 0.0;
    if (argc == 4)
    {
        const std::string required = argv[3];
        for (std::size_t index = 0; index < required.size();)
        {
            const auto codepoint = hui::gfx::next_codepoint(required, &index);
            if (!font.has_glyph(codepoint))
            {
                std::fprintf(stderr, "required title missing glyph U+%04X\n", codepoint);
                ++failures;
            }
        }
    }
    for (unsigned i = 0; i < header.glyph_count; ++i)
    {
        hui::gfx::font_format::Glyph glyph{};
        std::memcpy(&glyph, baked.data() + sizeof(header) + i * sizeof(glyph), sizeof(glyph));
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char *bitmap = stbtt_GetCodepointBitmap(&info, scale, scale,
            glyph.codepoint, &w, &h, &xoff, &yoff);
        if (bitmap && w > 0 && h > 0 &&
            (glyph.w != w + 2 * header.sdf_range || glyph.h != h + 2 * header.sdf_range ||
             glyph.offset_x != xoff - header.sdf_range || glyph.offset_y != yoff - header.sdf_range))
        {
            std::fprintf(stderr, "U+%04X incorrect atlas bounds or baseline offsets\n", glyph.codepoint);
            ++failures;
        }
        int mismatch = 0, ink_union = 0;
        for (int y = 0; y < glyph.h; ++y)
            for (int x = 0; x < glyph.w; ++x)
            {
                const int bx = x + static_cast<int>(glyph.offset_x) - xoff;
                const int by = y + static_cast<int>(glyph.offset_y) - yoff;
                const int coverage = bitmap && bx >= 0 && bx < w && by >= 0 && by < h ?
                    bitmap[by * w + bx] : 0;
                const bool reference = coverage >= 128;
                const bool actual = font.atlas()[(glyph.y + y) * header.atlas_width + glyph.x + x] >= 128;
                // Ignore partially covered boundary pixels: area coverage is
                // not identical to the SDF's pixel-center classification.
                mismatch += actual != reference && (coverage <= 32 || coverage >= 224);
                ink_union += actual || reference;
            }
        stbtt_FreeBitmap(bitmap, nullptr);
        // Subpixel edge coverage and a center-sampled SDF differ near the edge.
        // Large disagreements indicate missing contours, not antialiasing.
        if (ink_union > 10)
        {
            ++tested;
            const double fraction = static_cast<double>(mismatch) / ink_union;
            worst = std::max(worst, fraction);
            if (fraction > 0.10)
            {
                std::fprintf(stderr, "U+%04X silhouette mismatch: %d / %d (%.1f%%)\n",
                    glyph.codepoint, mismatch, ink_union, fraction * 100.0);
                ++failures;
            }
        }
        if (glyph.codepoint == ' ' && (glyph.w != 0 || glyph.h != 0 || glyph.advance <= 0))
        {
            std::fprintf(stderr, "space must be empty with positive advance\n");
            ++failures;
        }
    }
    if (tested < 80) { std::fprintf(stderr, "not enough glyphs tested\n"); return 1; }
    std::printf("%d glyph silhouettes checked, %d failures, worst mismatch %.2f%%\n",
        tested, failures, worst * 100.0);
    return failures ? 1 : 0;
}
