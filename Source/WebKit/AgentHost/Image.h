/*
 * Copyright (C) 2026 Stencil
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

// Stills of a page (the pixels of one frame) and the images made of them:
// an area cropped and scaled to a size, encoded as PNG or JPEG.

#include <cstdint>
#include <string>
#include <vector>

namespace AgentHost {

struct Still {
    // Premultiplied BGRA, `stride` bytes a row.
    std::vector<uint8_t> pixels;
    unsigned width { 0 };
    unsigned height { 0 };
    size_t stride { 0 };
    // Pixels of the still per CSS pixel of the page.
    double scale { 1 };
};

struct Area {
    double x { 0 };
    double y { 0 };
    double width { 0 };
    double height { 0 };
};

enum class ImageFormat { PNG, JPEG };

// The most pixels an image may have (a 16384 px square).
constexpr double maxImagePixels = 16384.0 * 16384.0;

// The CSS pixel `area` of a still as a `width` x `height` image in `format`
// (`quality` 1 to 100, for JPEG): box filtered down, bilinear up, transparent
// (white in a JPEG) past the still. Returns false and says why on failure.
bool encodeImage(const Still&, const Area&, unsigned width, unsigned height, ImageFormat, int quality, std::vector<uint8_t>& encoded, std::string& error);

} // namespace AgentHost
