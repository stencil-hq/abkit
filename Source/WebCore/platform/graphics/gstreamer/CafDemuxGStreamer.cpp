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

#include "config.h"
#include "CafDemuxGStreamer.h"

#if ENABLE(VIDEO) && USE(GSTREAMER) && OS(DARWIN)

#include "GStreamerCommon.h"
#include <CoreAudioTypes/CoreAudioTypes.h>
#include <bit>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

GST_DEBUG_CATEGORY_STATIC(webkitCafDemuxDebug);
#define GST_CAT_DEFAULT webkitCafDemuxDebug

// The chunk and codec identifiers are four characters; they are read in the argument order
// GST_MAKE_FOURCC expects, so GST_FOURCC_ARGS prints them as they appear in the file.
#define CAF_FOURCC(a, b, c, d) GST_MAKE_FOURCC(a, b, c, d)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
G_DEFINE_TYPE(WebKitCafDemux, webkit_caf_demux, GST_TYPE_BASE_PARSE);
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

static GstStaticPadTemplate cafDemuxSinkTemplate = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-caf"));

static GstStaticPadTemplate cafDemuxSrcTemplate = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw; "
        "audio/x-alac; "
        "audio/mpeg, mpegversion = (int) 4, stream-format = (string) raw; "
        "audio/x-opus"));

static void webkit_caf_demux_reset(WebKitCafDemux* self)
{
    self->state = WEBKIT_CAF_DEMUX_STATE_SIGNATURE;
    self->haveDesc = FALSE;
    self->formatId = 0;
    self->formatFlags = 0;
    self->bytesPerPacket = 0;
    self->framesPerPacket = 0;
    self->channels = 0;
    self->bitsPerChannel = 0;
    self->channelLayoutTag = 0;
    self->haveChannelLayout = FALSE;
    self->rate = 0;
    gst_buffer_replace(&self->cookie, nullptr);
    self->dataStart = 0;
    self->dataSize = -1;
    self->validFrames = 0;
    self->haveValidFrames = FALSE;
    self->streamBytes = 0;
}

// Maps the desc chunk's LinearPCM flags onto a raw sample format. The flags are CoreAudio's:
// float, big endian, signed integer, packed, aligned high, non-interleaved, from bit 0 up.
static gboolean webkit_caf_demux_lpcm_format(guint32 flags, guint32 bits, const char** format)
{
    if (flags & 0x20)
        return FALSE;

    gboolean bigEndian = flags & 0x2;
    if (flags & 0x1) {
        switch (bits) {
        case 32: *format = bigEndian ? "F32BE" : "F32LE"; return TRUE;
        case 64: *format = bigEndian ? "F64BE" : "F64LE"; return TRUE;
        default: return FALSE;
        }
    }

    switch (bits) {
    case 8: *format = "S8"; return TRUE;
    case 16: *format = bigEndian ? "S16BE" : "S16LE"; return TRUE;
    case 24: *format = bigEndian ? "S24BE" : "S24LE"; return TRUE;
    case 32: *format = bigEndian ? "S32BE" : "S32LE"; return TRUE;
    default: return FALSE;
    }
}

static GstCaps* webkit_caf_demux_opus_caps(WebKitCafDemux* self, guint32 channels)
{
    // Opus in CAF carries the OpusHead as its magic cookie; its channel mapping is what the Opus
    // decoder negotiates on, since the raw packets do not repeat it.
    gint mappingFamily = 0;
    gint streamCount = 0;
    gint coupledCount = 0;
    if (self->cookie) {
        WebCore::GstMappedBuffer map(self->cookie, GST_MAP_READ);
        if (map) {
            auto bytes = map.span<uint8_t>();
            if (bytes.size() >= 19 && equalSpans(bytes.first(8), byteCast<uint8_t>("OpusHead"_span))) {
                // OpusHead's fields are little-endian, as in Ogg Opus.
                mappingFamily = bytes[18];
                if (mappingFamily != 0 && bytes.size() >= 21) {
                    streamCount = bytes[19];
                    coupledCount = bytes[20];
                }
            }
        }
    }

    if (mappingFamily == 0)
        return gst_caps_new_simple("audio/x-opus", "channel-mapping-family", G_TYPE_INT, 0, nullptr);

    return gst_caps_new_simple("audio/x-opus",
        "channel-mapping-family", G_TYPE_INT, mappingFamily,
        "channels", G_TYPE_INT, (gint)channels,
        "stream-count", G_TYPE_INT, streamCount,
        "coupled-count", G_TYPE_INT, coupledCount, nullptr);
}

static GstCaps* webkit_caf_demux_src_caps(WebKitCafDemux* self)
{
    guint32 channels = self->channels;
    if (!channels && self->haveChannelLayout)
        channels = AudioChannelLayoutTag_GetNumberOfChannels((AudioChannelLayoutTag)self->channelLayoutTag);

    if (!channels)
        return nullptr;

    if (self->formatId == CAF_FOURCC('l', 'p', 'c', 'm')) {
        if (!self->rate)
            return nullptr;
        const char* format = nullptr;
        if (!webkit_caf_demux_lpcm_format(self->formatFlags, self->bitsPerChannel, &format))
            return nullptr;
        return gst_caps_new_simple("audio/x-raw",
            "format", G_TYPE_STRING, format,
            "layout", G_TYPE_STRING, "interleaved",
            "rate", G_TYPE_INT, (gint)self->rate,
            "channels", G_TYPE_INT, (gint)channels, nullptr);
    }

    if (self->formatId == CAF_FOURCC('a', 'l', 'a', 'c')) {
        GstCaps* caps = gst_caps_new_simple("audio/x-alac",
            "channels", G_TYPE_INT, (gint)channels, nullptr);
        if (self->rate)
            gst_caps_set_simple(caps, "rate", G_TYPE_INT, (gint)self->rate, nullptr);
        if (self->cookie)
            gst_caps_set_simple(caps, "codec_data", GST_TYPE_BUFFER, self->cookie, nullptr);
        return caps;
    }

    if (self->formatId == CAF_FOURCC('a', 'a', 'c', ' ')) {
        GstCaps* caps = gst_caps_new_simple("audio/mpeg",
            "mpegversion", G_TYPE_INT, 4,
            "stream-format", G_TYPE_STRING, "raw",
            "channels", G_TYPE_INT, (gint)channels, nullptr);
        if (self->rate)
            gst_caps_set_simple(caps, "rate", G_TYPE_INT, (gint)self->rate, nullptr);
        if (self->cookie)
            gst_caps_set_simple(caps, "codec_data", GST_TYPE_BUFFER, self->cookie, nullptr);
        return caps;
    }

    if (self->formatId == CAF_FOURCC('o', 'p', 'u', 's'))
        return webkit_caf_demux_opus_caps(self, channels);

    return nullptr;
}

static void webkit_caf_demux_update_duration(WebKitCafDemux* self, GstBaseParse* parse)
{
    if (!self->rate)
        return;

    GstClockTime duration = GST_CLOCK_TIME_NONE;
    if (self->haveValidFrames && self->validFrames)
        duration = gst_util_uint64_scale(self->validFrames, GST_SECOND, self->rate);
    else if (self->dataSize > 0 && self->bytesPerPacket && self->framesPerPacket) {
        guint64 packets = (guint64)self->dataSize / self->bytesPerPacket;
        duration = gst_util_uint64_scale(packets, (guint64)self->framesPerPacket * GST_SECOND, self->rate);
    }

    if (GST_CLOCK_TIME_IS_VALID(duration) && duration)
        gst_base_parse_set_duration(parse, GST_FORMAT_TIME, duration, 0);
}

static GstFlowReturn webkit_caf_demux_handle_frame(GstBaseParse* parse, GstBaseParseFrame* frame, gint* skipsize)
{
    WebKitCafDemux* self = WEBKIT_CAF_DEMUX(parse);
    GstBuffer* buffer = frame->buffer;
    *skipsize = 0;

    if (self->state == WEBKIT_CAF_DEMUX_STATE_DATA) {
        if (!self->bytesPerPacket || !self->framesPerPacket || !self->rate) {
            GST_ELEMENT_ERROR(parse, STREAM, FORMAT, ("CAF stream has no packet framing"), (nullptr));
            return GST_FLOW_ERROR;
        }

        gsize amount = gst_buffer_get_size(buffer);
        gsize consumed = amount - (amount % self->bytesPerPacket);
        if (!consumed) {
            if (GST_BASE_PARSE_DRAINING(parse)) {
                *skipsize = (gint)amount;
                return GST_FLOW_OK;
            }
            gst_base_parse_set_min_frame_size(parse, self->bytesPerPacket);
            return GST_FLOW_OK;
        }

        guint64 offset = GST_BUFFER_OFFSET(buffer);
        if (offset == GST_BUFFER_OFFSET_NONE)
            offset = self->dataStart + self->streamBytes;
        guint64 dataBytes = offset > self->dataStart ? offset - self->dataStart : 0;
        self->streamBytes = dataBytes + consumed;

        guint64 packets = dataBytes / self->bytesPerPacket;
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(packets, (guint64)self->framesPerPacket * GST_SECOND, self->rate);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(consumed / self->bytesPerPacket, (guint64)self->framesPerPacket * GST_SECOND, self->rate);
        GST_BUFFER_OFFSET(buffer) = offset;
        GST_BUFFER_OFFSET_END(buffer) = offset + consumed;

        return gst_base_parse_finish_frame(parse, frame, (gint)consumed);
    }

    WebCore::GstMappedBuffer map(buffer, GST_MAP_READ);
    if (!map)
        return GST_FLOW_ERROR;
    auto bytes = map.span<uint8_t>();

    GstFlowReturn flow = GST_FLOW_OK;
    // Assigned below; declared here, uninitialised, because the gotos to `done` jump past them.
    guint32 chunkType;
    guint64 chunkSize;
    std::span<const guint8> payload;
    guint64 total;

    if (self->state == WEBKIT_CAF_DEMUX_STATE_SIGNATURE) {
        if (bytes.size() < 8) {
            gst_base_parse_set_min_frame_size(parse, 8);
            goto done;
        }
        if (!equalSpans(bytes.first(4), byteCast<uint8_t>("caff"_span))) {
            *skipsize = 1;
            goto done;
        }
        self->state = WEBKIT_CAF_DEMUX_STATE_CHUNKS;
        *skipsize = 8;
        goto done;
    }

    // CHUNKS: one chunk header (fourcc and a big-endian 64-bit size) at a time.
    if (bytes.size() < 12) {
        gst_base_parse_set_min_frame_size(parse, 12);
        goto done;
    }

    chunkType = GST_READ_UINT32_LE(bytes.data());
    chunkSize = GST_READ_UINT64_BE(bytes.subspan(4).data());
    payload = bytes.subspan(12);
    // A chunk whose size is the all-ones marker reaches to the end of the stream (the data chunk
    // of a streamed file), which baseparse's skip cannot express.
    total = chunkSize > (guint64)G_MAXINT - 12 ? G_MAXUINT64 : 12 + chunkSize;

    if (chunkType == CAF_FOURCC('d', 'e', 's', 'c')) {
        if (chunkSize < 32) {
            GST_ELEMENT_ERROR(parse, STREAM, DEMUX, ("CAF desc chunk is too short"), (nullptr));
            flow = GST_FLOW_ERROR;
            goto done;
        }
        if (bytes.size() < 44) {
            gst_base_parse_set_min_frame_size(parse, 44);
            goto done;
        }

        guint64 rateBits = GST_READ_UINT64_BE(payload.data());
        gdouble sampleRate = std::bit_cast<gdouble>(rateBits);
        self->rate = (guint32)(sampleRate + 0.5);
        self->formatId = GST_READ_UINT32_LE(payload.subspan(8).data());
        self->formatFlags = GST_READ_UINT32_BE(payload.subspan(12).data());
        self->bytesPerPacket = GST_READ_UINT32_BE(payload.subspan(16).data());
        self->framesPerPacket = GST_READ_UINT32_BE(payload.subspan(20).data());
        self->channels = GST_READ_UINT32_BE(payload.subspan(24).data());
        self->bitsPerChannel = GST_READ_UINT32_BE(payload.subspan(28).data());
        self->haveDesc = TRUE;
    } else if (chunkType == CAF_FOURCC('p', 'a', 'k', 't')) {
        if (chunkSize >= 16) {
            if (bytes.size() < 28) {
                gst_base_parse_set_min_frame_size(parse, 28);
                goto done;
            }
            self->validFrames = GST_READ_UINT64_BE(payload.data());
            self->haveValidFrames = TRUE;
        }
    } else if (chunkType == CAF_FOURCC('c', 'h', 'a', 'n')) {
        if (chunkSize >= 12) {
            if (bytes.size() < 24) {
                gst_base_parse_set_min_frame_size(parse, 24);
                goto done;
            }
            self->channelLayoutTag = GST_READ_UINT32_BE(payload.data());
            self->haveChannelLayout = TRUE;
        }
    } else if (chunkType == CAF_FOURCC('k', 'u', 'k', 'i')) {
        if (chunkSize > 4 * 1024 * 1024) {
            GST_ELEMENT_ERROR(parse, STREAM, DEMUX, ("CAF magic cookie is too large"), (nullptr));
            flow = GST_FLOW_ERROR;
            goto done;
        }
        if (bytes.size() < total) {
            gst_base_parse_set_min_frame_size(parse, (guint)total);
            goto done;
        }
        gst_buffer_replace(&self->cookie, nullptr);
        self->cookie = gst_buffer_copy_region(buffer, GST_BUFFER_COPY_MEMORY, 12, chunkSize);
    } else if (chunkType == CAF_FOURCC('d', 'a', 't', 'a')) {
        if (!self->haveDesc) {
            GST_ELEMENT_ERROR(parse, STREAM, DEMUX, ("CAF data chunk before desc chunk"), (nullptr));
            flow = GST_FLOW_ERROR;
            goto done;
        }
        GstCaps* caps = webkit_caf_demux_src_caps(self);
        if (!caps) {
            auto printableCharacter = [](guint32 value) {
                auto character = value & 0xff;
                return character >= 0x20 && character <= 0x7e ? static_cast<char>(character) : '.';
            };
            GST_ELEMENT_ERROR(parse, STREAM, FORMAT, ("Unsupported CAF codec '%" GST_FOURCC_FORMAT "'", printableCharacter(self->formatId), printableCharacter(self->formatId >> 8), printableCharacter(self->formatId >> 16), printableCharacter(self->formatId >> 24)), (nullptr));
            flow = GST_FLOW_ERROR;
            goto done;
        }

        // The source caps and the duration are only known here: every chunk that describes the
        // codec sits before data.
        gst_pad_set_caps(GST_BASE_PARSE_SRC_PAD(parse), caps);
        gst_caps_unref(caps);

        self->dataStart = GST_BUFFER_OFFSET(buffer) + 12;
        self->dataSize = (chunkSize == G_MAXUINT64 || !chunkSize) ? -1 : (gint64)chunkSize;
        self->streamBytes = 0;
        self->state = WEBKIT_CAF_DEMUX_STATE_DATA;
        webkit_caf_demux_update_duration(self, parse);
        gst_base_parse_set_min_frame_size(parse, self->bytesPerPacket ? self->bytesPerPacket : 1);

        *skipsize = 12;
        goto done;
    }

    // Every other chunk (info, free, uuid, ...) and the ones just handled is passed over whole;
    // baseparse remembers a skip that reaches past the buffered data.
    if (total > (guint64)G_MAXINT) {
        GST_ELEMENT_ERROR(parse, STREAM, DEMUX, ("CAF chunk is too large"), (nullptr));
        flow = GST_FLOW_ERROR;
        goto done;
    }
    *skipsize = (gint)total;

done:
    return flow;
}

static gboolean webkit_caf_demux_convert(GstBaseParse* parse, GstFormat srcFormat, gint64 srcValue, GstFormat destFormat, gint64* destValue)
{
    WebKitCafDemux* self = WEBKIT_CAF_DEMUX(parse);

    if (srcValue < 0) {
        *destValue = -1;
        return TRUE;
    }

    if (self->rate && self->framesPerPacket && self->bytesPerPacket) {
        if (srcFormat == GST_FORMAT_BYTES && destFormat == GST_FORMAT_TIME) {
            gint64 bytes = srcValue - (gint64)self->dataStart;
            if (bytes < 0)
                bytes = 0;
            guint64 packets = (guint64)bytes / self->bytesPerPacket;
            *destValue = (gint64)gst_util_uint64_scale(packets, (guint64)self->framesPerPacket * GST_SECOND, self->rate);
            return TRUE;
        }
        if (srcFormat == GST_FORMAT_TIME && destFormat == GST_FORMAT_BYTES) {
            guint64 frames = gst_util_uint64_scale(srcValue, self->rate, GST_SECOND);
            guint64 packets = frames / self->framesPerPacket;
            *destValue = (gint64)(self->dataStart + packets * self->bytesPerPacket);
            return TRUE;
        }
    }

    if (self->rate) {
        if (srcFormat == GST_FORMAT_DEFAULT && destFormat == GST_FORMAT_TIME) {
            *destValue = (gint64)gst_util_uint64_scale(srcValue, GST_SECOND, self->rate);
            return TRUE;
        }
        if (srcFormat == GST_FORMAT_TIME && destFormat == GST_FORMAT_DEFAULT) {
            *destValue = (gint64)gst_util_uint64_scale(srcValue, self->rate, GST_SECOND);
            return TRUE;
        }
    }

    return GST_BASE_PARSE_CLASS(webkit_caf_demux_parent_class)->convert(parse, srcFormat, srcValue, destFormat, destValue);
}

static gboolean webkit_caf_demux_start(GstBaseParse* parse)
{
    WebKitCafDemux* self = WEBKIT_CAF_DEMUX(parse);
    webkit_caf_demux_reset(self);
    gst_base_parse_set_min_frame_size(parse, 8);
    gst_base_parse_set_syncable(parse, FALSE);
    return TRUE;
}

static gboolean webkit_caf_demux_stop(GstBaseParse* parse)
{
    webkit_caf_demux_reset(WEBKIT_CAF_DEMUX(parse));
    return TRUE;
}

static gboolean webkit_caf_demux_set_sink_caps(GstBaseParse* parse, GstCaps* caps)
{
    // Only audio/x-caf reaches this element (its sink template says so), and the source caps are
    // built from the desc chunk once it has been read, not from these.
    (void)parse;
    (void)caps;
    return TRUE;
}

static void webkit_caf_demux_finalize(GObject* object)
{
    WebKitCafDemux* self = WEBKIT_CAF_DEMUX(object);
    gst_buffer_replace(&self->cookie, nullptr);
    G_OBJECT_CLASS(webkit_caf_demux_parent_class)->finalize(object);
}

static void webkit_caf_demux_class_init(WebKitCafDemuxClass* klass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(klass);
    GstElementClass* elementClass = GST_ELEMENT_CLASS(klass);
    GstBaseParseClass* parseClass = GST_BASE_PARSE_CLASS(klass);

    GST_DEBUG_CATEGORY_INIT(webkitCafDemuxDebug, "webkitcafdemux", 0, "CAF demuxer");

    objectClass->finalize = webkit_caf_demux_finalize;

    parseClass->start = GST_DEBUG_FUNCPTR(webkit_caf_demux_start);
    parseClass->stop = GST_DEBUG_FUNCPTR(webkit_caf_demux_stop);
    parseClass->set_sink_caps = GST_DEBUG_FUNCPTR(webkit_caf_demux_set_sink_caps);
    parseClass->handle_frame = GST_DEBUG_FUNCPTR(webkit_caf_demux_handle_frame);
    parseClass->convert = GST_DEBUG_FUNCPTR(webkit_caf_demux_convert);

    gst_element_class_add_static_pad_template(elementClass, &cafDemuxSrcTemplate);
    gst_element_class_add_static_pad_template(elementClass, &cafDemuxSinkTemplate);

    gst_element_class_set_static_metadata(elementClass,
        "CAF audio demuxer", "Codec/Demuxer/Audio",
        "Demuxes the Core Audio Format container", "WebKit");
}

static void webkit_caf_demux_init(WebKitCafDemux* self)
{
    webkit_caf_demux_reset(self);
    GST_PAD_SET_ACCEPT_INTERSECT(GST_BASE_PARSE_SINK_PAD(self));
    GST_PAD_SET_ACCEPT_TEMPLATE(GST_BASE_PARSE_SINK_PAD(self));
}

#endif // ENABLE(VIDEO) && USE(GSTREAMER) && OS(DARWIN)
