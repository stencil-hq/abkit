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
#include "GLShaderCache.h"

#if USE(ANGLE) && USE(GLIB)

#if ENABLE(WEBGL)
#include "ANGLEHeaders.h"
#else
// This port's EGL is ANGLE's (only WebGL needs ANGLE beside a system EGL).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif
#include <atomic>
#include <cstring>
#include <mutex>
#include <wtf/FileSystem.h>
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ProcessID.h>
#include <wtf/SHA1.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace WebCore::GLShaderCache {

// The most the cache keeps on disk, and what an overfull one is pruned to when a process starts using it.
static constexpr uint64_t maximumSize = 128 * 1024 * 1024;
static constexpr uint64_t prunedSize = 96 * 1024 * 1024;

static std::atomic<bool> s_persistent { false };

static const String& directory()
{
    static NeverDestroyed<String> directory = [] {
#if PLATFORM(GTK)
        auto program = "webkitgtk"_s;
#else
        auto program = "wpe"_s;
#endif
        auto path = FileSystem::pathByAppendingComponents(FileSystem::userCacheDirectory(), std::initializer_list<StringView> { program, "shader-cache"_s });
        if (!FileSystem::makeAllDirectories(path))
            return String();
        return path;
    }();
    return directory.get();
}

static String pathForKey(const void* key, EGLsizeiANDROID keySize)
{
    SHA1 sha1;
    sha1.addBytes(unsafeMakeSpan(static_cast<const uint8_t*>(key), static_cast<size_t>(keySize)));
    SHA1::Digest digest;
    sha1.computeHash(digest);
    StringBuilder name;
    for (auto byte : digest)
        name.append(hex(byte, 2, Lowercase));
    return FileSystem::pathByAppendingComponent(directory(), name.toString());
}

// ANGLE asks with no buffer (or one too small) for the size first, then again with a buffer of that size.
static EGLsizeiANDROID get(const void* key, EGLsizeiANDROID keySize, void* value, EGLsizeiANDROID valueSize)
{
    if (directory().isNull())
        return 0;
    auto contents = FileSystem::readEntireFile(pathForKey(key, keySize));
    if (!contents || contents->isEmpty())
        return 0;
    auto size = static_cast<EGLsizeiANDROID>(contents->size());
    if (value && valueSize >= size)
        memcpySpan(unsafeMakeSpan(static_cast<uint8_t*>(value), contents->size()), contents->span());
    return size;
}

// Written beside its place and moved there, so that another process reads either nothing or all of it.
static void set(const void* key, EGLsizeiANDROID keySize, const void* value, EGLsizeiANDROID valueSize)
{
    if (!s_persistent.load(std::memory_order_relaxed) || directory().isNull() || valueSize <= 0)
        return;
    auto path = pathForKey(key, keySize);
    auto partial = makeString(path, ".part"_s, getCurrentProcessID());
    auto bytes = unsafeMakeSpan(static_cast<const uint8_t*>(value), static_cast<size_t>(valueSize));
    if (FileSystem::overwriteEntireFile(partial, bytes) != bytes.size() || !FileSystem::moveFile(partial, path))
        FileSystem::deleteFile(partial);
}

// Removes the oldest entries of an overfull cache, and what interrupted writes left behind.
static void prune()
{
    struct Entry {
        String path;
        uint64_t size;
        WallTime modified;
    };
    Vector<Entry> entries;
    uint64_t total = 0;
    for (auto& name : FileSystem::listDirectory(directory())) {
        auto path = FileSystem::pathByAppendingComponent(directory(), name);
        if (name.contains(".part"_s)) {
            auto modified = FileSystem::fileModificationTime(path);
            if (modified && WallTime::now() - *modified > 1_min)
                FileSystem::deleteFile(path);
            continue;
        }
        auto size = FileSystem::fileSize(path);
        auto modified = FileSystem::fileModificationTime(path);
        if (!size || !modified)
            continue;
        total += *size;
        entries.append({ WTF::move(path), *size, *modified });
    }
    if (total <= maximumSize)
        return;
    std::ranges::sort(entries, { }, &Entry::modified);
    for (auto& entry : entries) {
        if (total <= prunedSize)
            break;
        if (FileSystem::deleteFile(entry.path))
            total -= entry.size;
    }
}

void attach(EGLDisplay display)
{
#if ENABLE(WEBGL)
    const char* extensions = EGL_QueryString(display, EGL_EXTENSIONS);
    auto setBlobCacheFuncs = EGL_SetBlobCacheFuncsANDROID;
#else
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    auto setBlobCacheFuncs = reinterpret_cast<PFNEGLSETBLOBCACHEFUNCSANDROIDPROC>(eglGetProcAddress("eglSetBlobCacheFuncsANDROID"));
#endif
    if (!extensions || !strstr(extensions, "EGL_ANDROID_blob_cache") || !setBlobCacheFuncs || directory().isNull())
        return;
    static std::once_flag pruneOnce;
    std::call_once(pruneOnce, prune);
    setBlobCacheFuncs(display, set, get);
}

void setPersistent(bool persistent)
{
    s_persistent.store(persistent, std::memory_order_relaxed);
}

} // namespace WebCore::GLShaderCache

#endif // USE(ANGLE) && USE(GLIB)
