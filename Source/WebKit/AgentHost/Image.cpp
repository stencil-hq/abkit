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

#include "Image.h"

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jpeglib.h>
#include <png.h>

namespace AgentHost {

struct Tap {
    unsigned index;
    float weight;
};

// What each of `count` output samples along one axis is made of: the samples
// of the still (of `size` along that axis) under it, from `start` on, `step`
// still samples per output sample.
static std::vector<std::vector<Tap>> tapsAlong(double start, double step, unsigned count, unsigned size)
{
    std::vector<std::vector<Tap>> taps(count);
    for (unsigned i = 0; i < count; i++) {
        double from = start + step * i;
        double to = from + step;
        if (to <= 0 || from >= size || !size)
            continue;
        auto& sample = taps[i];
        if (step <= 1) {
            // Enlarging (or copying): between the two nearest samples, staying inside the still.
            double center = (from + to) / 2 - 0.5;
            double floor = std::floor(center);
            float fraction = static_cast<float>(center - floor);
            auto clamp = [size](double index) {
                return static_cast<unsigned>(std::clamp(index, 0.0, static_cast<double>(size - 1)));
            };
            if (fraction < 1)
                sample.push_back({ clamp(floor), 1 - fraction });
            if (fraction > 0)
                sample.push_back({ clamp(floor + 1), fraction });
            continue;
        }
        // Reducing: every sample under the output sample, by how much of it lies under.
        double first = std::max(0.0, std::floor(from));
        double last = std::min(static_cast<double>(size), std::ceil(to));
        for (double index = first; index < last; index++) {
            double covered = std::min(to, index + 1) - std::max(from, index);
            if (covered > 0)
                sample.push_back({ static_cast<unsigned>(index), static_cast<float>(covered / step) });
        }
    }
    return taps;
}

// The area as premultiplied RGBA, 8 bits a channel.
static std::vector<uint8_t> resample(const Still& still, const Area& area, unsigned width, unsigned height)
{
    auto columns = tapsAlong(area.x * still.scale, area.width * still.scale / width, width, still.width);
    auto rows = tapsAlong(area.y * still.scale, area.height * still.scale / height, height, still.height);
    std::vector<uint8_t> image(static_cast<size_t>(width) * height * 4);
    std::vector<float> sums(static_cast<size_t>(width) * 4);
    for (unsigned y = 0; y < height; y++) {
        std::fill(sums.begin(), sums.end(), 0.f);
        for (auto& row : rows[y]) {
            const uint8_t* source = still.pixels.data() + still.stride * row.index;
            for (unsigned x = 0; x < width; x++) {
                float* sum = sums.data() + static_cast<size_t>(x) * 4;
                for (auto& column : columns[x]) {
                    const uint8_t* pixel = source + static_cast<size_t>(column.index) * 4;
                    float weight = row.weight * column.weight;
                    sum[0] += pixel[2] * weight;
                    sum[1] += pixel[1] * weight;
                    sum[2] += pixel[0] * weight;
                    sum[3] += pixel[3] * weight;
                }
            }
        }
        uint8_t* target = image.data() + static_cast<size_t>(y) * width * 4;
        for (size_t i = 0; i < sums.size(); i++)
            target[i] = static_cast<uint8_t>(std::clamp(sums[i] + 0.5f, 0.f, 255.f));
    }
    return image;
}

static bool encodePNG(std::vector<uint8_t>& pixels, unsigned width, unsigned height, std::vector<uint8_t>& encoded, std::string& error)
{
    // PNG holds colour that is not premultiplied.
    for (size_t i = 0; i < pixels.size(); i += 4) {
        unsigned alpha = pixels[i + 3];
        if (alpha == 255 || !alpha)
            continue;
        for (unsigned channel = 0; channel < 3; channel++)
            pixels[i + channel] = static_cast<uint8_t>(std::min(255u, (pixels[i + channel] * 255u + alpha / 2) / alpha));
    }

    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;
    // Room for the largest file the image can make, so that it is compressed once.
    png_alloc_size_t size = PNG_IMAGE_PNG_SIZE_MAX(image);
    encoded.resize(size);
    if (!png_image_write_to_memory(&image, encoded.data(), &size, 0, pixels.data(), 0, nullptr)) {
        error = std::string("capture: ") + image.message;
        png_image_free(&image);
        return false;
    }
    encoded.resize(size);
    return true;
}

struct JPEGErrorManager {
    jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

static void jpegFailed(j_common_ptr info)
{
    auto* manager = reinterpret_cast<JPEGErrorManager*>(info->err);
    (*info->err->format_message)(info, manager->message);
    longjmp(manager->jump, 1);
}

// Nothing with a destructor lives in this function: libjpeg reports errors by jumping out of it.
static bool compressJPEG(const uint8_t* rgb, unsigned width, unsigned height, int quality, unsigned char** data, unsigned long* size, char* message)
{
    jpeg_compress_struct info;
    JPEGErrorManager manager;
    info.err = jpeg_std_error(&manager.base);
    manager.base.error_exit = jpegFailed;
    if (setjmp(manager.jump)) {
        memcpy(message, manager.message, JMSG_LENGTH_MAX);
        jpeg_destroy_compress(&info);
        return false;
    }
    jpeg_create_compress(&info);
    jpeg_mem_dest(&info, data, size);
    info.image_width = width;
    info.image_height = height;
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, quality, TRUE);
    jpeg_start_compress(&info, TRUE);
    while (info.next_scanline < info.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(rgb + static_cast<size_t>(info.next_scanline) * width * 3);
        jpeg_write_scanlines(&info, &row, 1);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);
    return true;
}

static bool encodeJPEG(const std::vector<uint8_t>& pixels, unsigned width, unsigned height, int quality, std::vector<uint8_t>& encoded, std::string& error)
{
    // JPEG has no transparency: the image is laid over white.
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (size_t i = 0, o = 0; i < pixels.size(); i += 4, o += 3) {
        unsigned under = 255 - pixels[i + 3];
        for (unsigned channel = 0; channel < 3; channel++)
            rgb[o + channel] = static_cast<uint8_t>(std::min(255u, pixels[i + channel] + under));
    }
    unsigned char* data = nullptr;
    unsigned long size = 0;
    char message[JMSG_LENGTH_MAX] = "";
    bool compressed = compressJPEG(rgb.data(), width, height, quality, &data, &size, message);
    if (compressed)
        encoded.assign(data, data + size);
    else
        error = std::string("capture: ") + message;
    free(data);
    return compressed;
}

bool encodeImage(const Still& still, const Area& area, unsigned width, unsigned height, ImageFormat format, int quality, std::vector<uint8_t>& encoded, std::string& error)
{
    if (!width || !height || !still.width || !still.height || still.pixels.size() < still.stride * still.height) {
        error = "capture: there is no image to encode";
        return false;
    }
    auto pixels = resample(still, area, width, height);
    if (format == ImageFormat::JPEG)
        return encodeJPEG(pixels, width, height, quality, encoded, error);
    return encodePNG(pixels, width, height, encoded, error);
}

} // namespace AgentHost
