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

#include "Block.h"

#include "Display.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace AgentHost {

// Images of the page. A block renders no frames (CompositionSuppressed): a
// capture lifts that, takes a frame, and puts it back. Which frame: ending
// the suppression makes the engine render one, but it may first render a
// composition that was left waiting when the suppression began, of what the
// page's layers held then. So the capture lets two animation frames pass
// after the suppression ended, which takes a rendering update of the page as
// it is now and the frame made of it (the page's next update waits for that
// frame to be shown), and takes the last frame it got by then. The whole
// document is such a frame with the view at the document's size for that
// moment (layout that depends on the viewport's height sees the taller one).
// The still is cropped, scaled and encoded off the main loop.

// How long a capture waits for the page to settle, and then for its frame.
static constexpr unsigned settlePatience = 500;
static constexpr unsigned framePatience = 3000;
// The engine renders at most this many pixels a side.
static constexpr double maxFrameSide = 16384;
static const char settleScript[] = "await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));";
// The page's document size (CSS px), as `width,height`.
static const char extentScript[] = "const d = document.documentElement, b = document.body; "
    "return [Math.max(d ? d.scrollWidth : 0, b ? b.scrollWidth : 0, innerWidth), Math.max(d ? d.scrollHeight : 0, b ? b.scrollHeight : 0, innerHeight)].join();";

struct Block::CaptureRequest {
    Reply reply;
    std::optional<Area> rect;
    bool full { false };
    double scale { 1 };
    ImageFormat format { ImageFormat::PNG };
    int quality { 80 };

    // The size (CSS px) of the view the frame is of.
    unsigned viewWidth { 0 };
    unsigned viewHeight { 0 };
    // The last frame of that size rendered since the suppression ended, and whether the page has settled since.
    std::optional<Still> latest;
    bool settled { false };
    guint timer { 0 };
};

void Block::capture(const Json& operation, const Reply& reply)
{
    auto request = std::make_shared<CaptureRequest>();
    request->reply = reply;
    if (auto rect = operation.get("rect"); rect && !rect.isNull()) {
        Area area;
        for (auto [name, value] : { std::pair { "x", &area.x }, std::pair { "y", &area.y }, std::pair { "width", &area.width }, std::pair { "height", &area.height } }) {
            auto number = rect.numberMember(name);
            if (!number || !std::isfinite(*number)) {
                reply->invalid(std::string("`") + name + "` must be a number");
                return;
            }
            *value = *number;
        }
        request->rect = area;
    }
    if (auto full = operation.get("full"); full && !full.isNull()) {
        if (!full.isBoolean()) {
            reply->invalid("`full` must be true or false");
            return;
        }
        request->full = full.toBoolean();
    }
    if (auto scale = operation.get("scale"); scale && !scale.isNull()) {
        if (!scale.isNumber()) {
            reply->invalid("`scale` must be a number");
            return;
        }
        request->scale = scale.toNumber();
    }
    if (auto format = operation.get("format"); format && !format.isNull()) {
        auto name = format.isString() ? format.toString() : std::string();
        if (name == "jpeg" || name == "jpg")
            request->format = ImageFormat::JPEG;
        else if (name != "png") {
            reply->invalid("capture: unknown format " + format.serialize() + " (png, jpeg)");
            return;
        }
    }
    if (auto quality = operation.get("quality"); quality && !quality.isNull()) {
        if (!quality.isNumber()) {
            reply->invalid("`quality` must be a number");
            return;
        }
        request->quality = static_cast<int>(std::clamp(std::round(quality.toNumber()), 1.0, 100.0));
    }
    if (!(std::isfinite(request->scale) && request->scale > 0)) {
        reply->failed("capture: the scale must be positive, not " + Json::number(request->scale).serialize());
        return;
    }
    reply->setUnansweredMessage("capture: the page is gone");
    m_captures.push_back(std::move(request));
    if (!m_capture)
        startNextCapture();
}

void Block::startNextCapture()
{
    if (m_capture || m_captures.empty() || m_closed)
        return;
    m_capture = std::move(m_captures.front());
    m_captures.pop_front();
    auto request = m_capture;
    request->viewWidth = m_width;
    request->viewHeight = m_height;
    // One wait for the whole capture: a page held by a dialog neither measures itself nor draws.
    request->timer = runAfter(2 * settlePatience + framePatience, [weakThis = weak_from_this(), request] {
        request->timer = 0;
        auto block = weakThis.lock();
        if (block && !block->m_closed && block->m_capture == request)
            block->finishCapture(request, std::nullopt, "capture: the page drew no frame");
    });

    if (!request->full) {
        grabStill(request);
        return;
    }
    // The document's size, as its page measures it once it has laid out.
    run(std::string(settleScript) + extentScript, "[]", World::Isolated, [weakThis = weak_from_this(), request](std::optional<std::string>&& result, GError* error) {
        auto block = weakThis.lock();
        if (!block || block->m_closed || block->m_capture != request)
            return;
        double width = 0, height = 0;
        if (error || !result || sscanf(result->c_str(), "%lf,%lf", &width, &height) != 2) {
            block->finishCapture(request, std::nullopt, error ? error->message : "capture: the page did not give its size");
            return;
        }
        width = std::max(std::round(width), static_cast<double>(request->viewWidth));
        height = std::max(std::round(height), static_cast<double>(request->viewHeight));
        double scale = wpe_view_get_scale(block->m_wpeView);
        if (width * scale > maxFrameSide || height * scale > maxFrameSide) {
            GChars message(g_strdup_printf("capture: WPE WebKit renders at most %.0f px a side; the document is %.0f×%.0f CSS px at %g px each", maxFrameSide, width, height, scale));
            block->finishCapture(request, std::nullopt, message.get());
            return;
        }
        request->viewWidth = static_cast<unsigned>(width);
        request->viewHeight = static_cast<unsigned>(height);
        block->grabStill(request);
    });
}

void Block::grabStill(const std::shared_ptr<CaptureRequest>& request)
{
    m_frameWaiter = [this, request](WPEBuffer* buffer) {
        double scale = wpe_view_get_scale(m_wpeView);
        int width = wpe_buffer_get_width(buffer);
        int height = wpe_buffer_get_height(buffer);
        // A frame of another size is of the view before it took the size the capture gave it.
        if (std::abs(width - request->viewWidth * scale) > 1 || std::abs(height - request->viewHeight * scale) > 1)
            return;
        GError* error = nullptr;
        // The bytes stay the buffer's.
        auto* bytes = wpe_buffer_import_to_pixels(buffer, &error);
        if (!bytes) {
            auto message = "capture: " + takeError(error, "the frame has no pixels");
            finishCapture(request, std::nullopt, message.c_str());
            return;
        }
        gsize size = 0;
        auto* data = static_cast<const uint8_t*>(g_bytes_get_data(bytes, &size));
        Still still;
        still.width = static_cast<unsigned>(width);
        still.height = static_cast<unsigned>(height);
        still.stride = height > 0 ? size / static_cast<size_t>(height) : 0;
        still.scale = static_cast<double>(width) / request->viewWidth;
        if (still.stride < static_cast<size_t>(width) * 4) {
            finishCapture(request, std::nullopt, "capture: the frame's pixels are not 32 bits each");
            return;
        }
        still.pixels.assign(data, data + size);
        if (request->settled)
            finishCapture(request, std::move(still), nullptr);
        else
            request->latest = std::move(still);
    };
    updateCompositionSuppressed();
    if (request->viewWidth != m_width || request->viewHeight != m_height)
        resizeView(m_wpeView, static_cast<int>(request->viewWidth), static_cast<int>(request->viewHeight));
    // A presented page renders only when it changes: what it shows now is its last frame, which the client still
    // holds (so it is not drawn into).
    else if (m_presentation && m_presentation->latest)
        m_frameWaiter(m_presentation->latest.get());
    // A page held by a dialog does not settle: the capture then takes what was rendered, or the next frame.
    runPatiently(settleScript, settlePatience, [weakThis = weak_from_this(), request] {
        auto block = weakThis.lock();
        if (!block || block->m_closed || block->m_capture != request)
            return;
        request->settled = true;
        if (auto still = std::exchange(request->latest, std::nullopt))
            block->finishCapture(request, std::move(still), nullptr);
    });
}

void Block::cancelCaptureTimer(CaptureRequest& request)
{
    if (auto timer = std::exchange(request.timer, 0))
        g_source_remove(timer);
}

bool Block::frameRendered(WPEBuffer* buffer)
{
    AGENT_HOST_DEBUG("block %" G_GUINT64_FORMAT ": frame %dx%d%s", m_identifier, wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer), m_frameWaiter || m_presentation ? "" : " (dropped)");
    // A frame nobody waits for (one more rendered before the suppression took hold again) is dropped.
    if (auto waiter = m_frameWaiter)
        waiter(buffer);
    // A capture's waiter may have finished it and stopped the presentation of a closing block: ask again.
    return !m_closed && m_presentation && presentFrame(buffer);
}

void Block::finishCapture(const std::shared_ptr<CaptureRequest>& request, std::optional<Still>&& still, const char* error)
{
    if (m_capture != request)
        return;
    // `error` may be the waiter's own, which goes with it: keep the text.
    std::optional<std::string> message;
    if (error)
        message = error;
    m_capture = nullptr;
    m_frameWaiter = nullptr;
    cancelCaptureTimer(*request);
    updateCompositionSuppressed();
    // Back to the block's size, which `viewport` may have changed meanwhile.
    if (wpe_view_get_width(m_wpeView) != static_cast<int>(m_width) || wpe_view_get_height(m_wpeView) != static_cast<int>(m_height))
        resizeView(m_wpeView, static_cast<int>(m_width), static_cast<int>(m_height));

    auto reply = request->reply;
    if (message || !still)
        reply->failed(message.value_or("capture: the page drew no frame"));
    else {
        Area area = request->rect && !request->full ? *request->rect : Area { 0, 0, still->width / still->scale, still->height / still->scale };
        double width = std::round(area.width * request->scale);
        double height = std::round(area.height * request->scale);
        if (!(width >= 1 && height >= 1 && width * height <= maxImagePixels)) {
            GChars text(g_strdup_printf("capture: a %.0f×%.0f px image is out of range", width, height));
            reply->failed(text.get());
        } else {
            std::thread([reply, area, width = static_cast<unsigned>(width), height = static_cast<unsigned>(height), format = request->format, quality = request->quality, still = std::move(*still)]() mutable {
                std::vector<uint8_t> encoded;
                std::string error;
                std::string answer;
                if (encodeImage(still, area, width, height, format, quality, encoded, error)) {
                    GChars base64(g_base64_encode(encoded.data(), encoded.size()));
                    answer.reserve(strlen(base64.get()) + 96);
                    answer.append("{\"data\":\"").append(base64.get()).append("\",\"mime\":\"").append(format == ImageFormat::JPEG ? "image/jpeg" : "image/png");
                    answer.append("\",\"width\":").append(std::to_string(width)).append(",\"height\":").append(std::to_string(height)).append("}");
                }
                // The answer is given, and let go of, on the main loop.
                runOnMainLoop([reply = std::move(reply), answer = std::move(answer), error = std::move(error)] {
                    if (answer.empty())
                        reply->failed(error);
                    else
                        reply->ok(answer);
                });
            }).detach();
        }
    }
    startNextCapture();
}

} // namespace AgentHost
