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

#import "Pasteboard.h"

#import <AppKit/AppKit.h>
#include <cstring>

namespace AgentHost {

G_DECLARE_FINAL_TYPE(AgentHostPasteboard, agent_host_pasteboard, AGENT_HOST, PASTEBOARD, WPEClipboard)

struct _AgentHostPasteboard {
    WPEClipboard parent;
    // The pasteboard's change count when the clipboard last wrote or read it.
    NSInteger changeCount;
};

G_DEFINE_FINAL_TYPE(AgentHostPasteboard, agent_host_pasteboard, WPE_TYPE_CLIPBOARD)

// The pasteboard type the engine's format is kept as; nil for formats only the engine reads (its smart paste
// marker, a page's custom data), which stay the host's.
static NSPasteboardType pasteboardType(const char* format)
{
    if (!strcmp(format, "text/plain") || !strcmp(format, "text/plain;charset=utf-8"))
        return NSPasteboardTypeString;
    if (!strcmp(format, "text/html"))
        return NSPasteboardTypeHTML;
    if (!strcmp(format, "text/uri-list"))
        return NSPasteboardTypeURL;
    if (!strcmp(format, "image/png"))
        return NSPasteboardTypePNG;
    if (!strcmp(format, "image/svg+xml"))
        return @"public.svg-image";
    return nil;
}

static NSString *utf8String(GBytes* bytes)
{
    gsize size;
    const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
    while (size && !data[size - 1])
        size--;
    return [[NSString alloc] initWithBytes:data length:size encoding:NSUTF8StringEncoding];
}

static GBytes* utf8Bytes(NSString *string)
{
    const char* utf8 = string.UTF8String;
    return utf8 ? g_bytes_new(utf8, strlen(utf8)) : nullptr;
}

// What a page copied goes on the pasteboard too, in the types other apps read.
static void writePasteboard(AgentHostPasteboard* self, GPtrArray* formats, WPEClipboardContent* content)
{
    @autoreleasepool {
        NSPasteboard *pasteboard = NSPasteboard.generalPasteboard;
        [pasteboard clearContents];
        NSMutableSet<NSPasteboardType> *written = [NSMutableSet set];
        for (guint i = 0; content && formats && i < formats->len; i++) {
            auto* format = static_cast<const char*>(g_ptr_array_index(formats, i));
            NSPasteboardType type = pasteboardType(format);
            if (!type || [written containsObject:type])
                continue;
            GOutputStream* stream = g_memory_output_stream_new_resizable();
            if (wpe_clipboard_content_serialize(content, format, stream) && g_output_stream_close(stream, nullptr, nullptr)) {
                GBytes* bytes = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(stream));
                if (type == NSPasteboardTypeURL) {
                    // The first address of the list (its other lines are more addresses or comments).
                    NSString *list = utf8String(bytes);
                    for (NSString *line in [list componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
                        if (line.length && ![line hasPrefix:@"#"]) {
                            [pasteboard setString:line forType:NSPasteboardTypeURL];
                            break;
                        }
                    }
                } else if (type == NSPasteboardTypeString || type == NSPasteboardTypeHTML) {
                    if (NSString *string = utf8String(bytes))
                        [pasteboard setString:string forType:type];
                } else {
                    gsize size;
                    const void* data = g_bytes_get_data(bytes, &size);
                    [pasteboard setData:[NSData dataWithBytes:data length:size] forType:type];
                }
                g_bytes_unref(bytes);
                [written addObject:type];
            }
            g_object_unref(stream);
        }
        self->changeCount = pasteboard.changeCount;
    }
}

static void agentHostPasteboardChanged(WPEClipboard* clipboard, GPtrArray* formats, gboolean isLocal, WPEClipboardContent* content)
{
    if (isLocal)
        writePasteboard(AGENT_HOST_PASTEBOARD(clipboard), formats, content);
    WPE_CLIPBOARD_CLASS(agent_host_pasteboard_parent_class)->changed(clipboard, formats, isLocal, content);
}

static GBytes* agentHostPasteboardRead(WPEClipboard*, const char* format)
{
    @autoreleasepool {
        NSPasteboard *pasteboard = NSPasteboard.generalPasteboard;
        NSPasteboardType type = pasteboardType(format);
        if (type == NSPasteboardTypeString || type == NSPasteboardTypeHTML)
            return utf8Bytes([pasteboard stringForType:type]);
        if (type == NSPasteboardTypeURL) {
            NSArray<NSURL *> *urls = [pasteboard readObjectsForClasses:@[NSURL.class] options:nil];
            if (!urls.count)
                return nullptr;
            NSMutableString *list = [NSMutableString string];
            for (NSURL *url in urls)
                [list appendFormat:@"%@\r\n", url.absoluteString];
            return utf8Bytes(list);
        }
        if (type == NSPasteboardTypePNG) {
            NSData *png = [pasteboard dataForType:NSPasteboardTypePNG];
            // Screenshots and images copied in other apps are often TIFF only.
            if (!png) {
                if (NSData *tiff = [pasteboard dataForType:NSPasteboardTypeTIFF])
                    png = [[NSBitmapImageRep imageRepWithData:tiff] representationUsingType:NSBitmapImageFileTypePNG properties:@{ }];
            }
            return png ? g_bytes_new(png.bytes, png.length) : nullptr;
        }
        if (type) {
            NSData *data = [pasteboard dataForType:type];
            return data ? g_bytes_new(data.bytes, data.length) : nullptr;
        }
        return nullptr;
    }
}

static void agent_host_pasteboard_class_init(AgentHostPasteboardClass* pasteboardClass)
{
    WPE_CLIPBOARD_CLASS(pasteboardClass)->changed = agentHostPasteboardChanged;
    WPE_CLIPBOARD_CLASS(pasteboardClass)->read = agentHostPasteboardRead;
}

static void agent_host_pasteboard_init(AgentHostPasteboard* self)
{
    self->changeCount = -1;
}

WPEClipboard* createSystemClipboard(WPEDisplay* display)
{
    auto* clipboard = WPE_CLIPBOARD(g_object_new(agent_host_pasteboard_get_type(), "display", display, nullptr));
    syncSystemClipboard(clipboard);
    return clipboard;
}

void syncSystemClipboard(WPEClipboard* clipboard)
{
    if (!AGENT_HOST_IS_PASTEBOARD(clipboard))
        return;
    auto* self = AGENT_HOST_PASTEBOARD(clipboard);
    @autoreleasepool {
        NSPasteboard *pasteboard = NSPasteboard.generalPasteboard;
        NSInteger changeCount = pasteboard.changeCount;
        if (changeCount == self->changeCount)
            return;
        self->changeCount = changeCount;
        NSArray<NSPasteboardType> *types = pasteboard.types;
        auto has = [types](NSPasteboardType type) {
            return [types containsObject:type];
        };
        GPtrArray* formats = g_ptr_array_new_null_terminated(5, nullptr, TRUE);
        auto add = [formats](const char* format) {
            g_ptr_array_add(formats, const_cast<char*>(g_intern_static_string(format)));
        };
        if (has(NSPasteboardTypeString)) {
            add("text/plain");
            add("text/plain;charset=utf-8");
        }
        if (has(NSPasteboardTypeHTML))
            add("text/html");
        if (has(NSPasteboardTypeURL) || has(NSPasteboardTypeFileURL))
            add("text/uri-list");
        if (has(NSPasteboardTypePNG) || has(NSPasteboardTypeTIFF))
            add("image/png");
        if (has(@"public.svg-image"))
            add("image/svg+xml");
        // Not the clipboard's own: the engine reads it through agentHostPasteboardRead().
        WPE_CLIPBOARD_CLASS(agent_host_pasteboard_parent_class)->changed(clipboard, formats, FALSE, nullptr);
        g_ptr_array_unref(formats);
    }
}

} // namespace AgentHost
