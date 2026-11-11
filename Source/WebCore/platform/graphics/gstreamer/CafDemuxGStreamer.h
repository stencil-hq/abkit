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

#if ENABLE(VIDEO) && USE(GSTREAMER) && OS(DARWIN)

#include <gst/base/gstbaseparse.h>
#include <gst/gst.h>

G_BEGIN_DECLS

#define WEBKIT_TYPE_CAF_DEMUX            (webkit_caf_demux_get_type())
#define WEBKIT_CAF_DEMUX(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), WEBKIT_TYPE_CAF_DEMUX, WebKitCafDemux))
#define WEBKIT_CAF_DEMUX_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), WEBKIT_TYPE_CAF_DEMUX, WebKitCafDemuxClass))
#define WEBKIT_IS_CAF_DEMUX(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), WEBKIT_TYPE_CAF_DEMUX))
#define WEBKIT_IS_CAF_DEMUX_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), WEBKIT_TYPE_CAF_DEMUX))
#define WEBKIT_CAF_DEMUX_CAST(obj)       ((WebKitCafDemux*)(obj))

// A Mac plays CAF through AVFoundation, and the GStreamer a page's media comes from has a
// typefinder for the container but no demuxer for it.
typedef enum {
    WEBKIT_CAF_DEMUX_STATE_SIGNATURE,
    WEBKIT_CAF_DEMUX_STATE_CHUNKS,
    WEBKIT_CAF_DEMUX_STATE_DATA,
} WebKitCafDemuxState;

typedef struct _WebKitCafDemux WebKitCafDemux;
typedef struct _WebKitCafDemuxClass WebKitCafDemuxClass;

struct _WebKitCafDemux {
    GstBaseParse parent;

    WebKitCafDemuxState state;
    gboolean haveDesc;
    guint32 formatId;
    guint32 formatFlags;
    guint32 bytesPerPacket;
    guint32 framesPerPacket;
    guint32 channels;
    guint32 bitsPerChannel;
    guint32 channelLayoutTag;
    gboolean haveChannelLayout;
    guint32 rate;
    GstBuffer* cookie;
    guint64 dataStart;
    gint64 dataSize;
    guint64 validFrames;
    gboolean haveValidFrames;
    guint64 streamBytes;
};

struct _WebKitCafDemuxClass {
    GstBaseParseClass parentClass;
};

// The element's GType, registered on Darwin as "webkitcafdemux".
GType webkit_caf_demux_get_type(void);

G_END_DECLS

#endif // ENABLE(VIDEO) && USE(GSTREAMER) && OS(DARWIN)
