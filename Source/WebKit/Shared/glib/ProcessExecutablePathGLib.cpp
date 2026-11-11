/*
 * Copyright (C) 2010 Apple Inc. All rights reserved.
 * Portions Copyright (c) 2010 Motorola Mobility, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY MOTOROLA INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MOTOROLA INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ProcessExecutablePath.h"

#include <glib.h>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>

#if OS(DARWIN)
#include <wtf/darwin/EngineLibrary.h>
#elif OS(WINDOWS)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace WebKit {

#if ENABLE(DEVELOPER_MODE)
static String getExecutablePath()
{
    auto executablePath = FileSystem::currentExecutablePath();
    if (!executablePath.isNull())
        return FileSystem::parentPath(FileSystem::stringFromFileSystemRepresentation(executablePath));
    return { };
}
#endif

String directoryOfWebKitLibrary()
{
    static NeverDestroyed<String> directory = [] {
#if OS(DARWIN)
        // Shared with WebCore, which finds the bundled runtime libraries by it too.
        return WTF::directoryOfEngineLibrary();
#elif OS(WINDOWS)
        HMODULE module = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&directoryOfWebKitLibrary), &module))
            return String();
        WCHAR path[MAX_PATH];
        auto length = ::GetModuleFileNameW(module, path, std::size(path));
        if (!length || length == std::size(path))
            return String();
        return FileSystem::parentPath(String(path));
#else
        Dl_info info;
        if (!dladdr(reinterpret_cast<const void*>(&directoryOfWebKitLibrary), &info) || !info.dli_fname)
            return String();
        return FileSystem::parentPath(FileSystem::stringFromFileSystemRepresentation(info.dli_fname));
#endif
    }();
    return directory;
}

static String findWebKitProcess(const ASCIILiteral processName)
{
    // An embedder shipping the engine inside its own bundle keeps the helper
    // processes beside the library; the install prefix means nothing there.
    if (auto libraryDirectory = directoryOfWebKitLibrary(); !libraryDirectory.isNull()) {
        String processPath = FileSystem::pathByAppendingComponent(libraryDirectory, processName);
        if (FileSystem::fileExists(processPath))
            return processPath;
    }

#if ENABLE(DEVELOPER_MODE)
    static const char* execDirectory = g_getenv("WEBKIT_EXEC_PATH");
    if (execDirectory) {
        String processPath = FileSystem::pathByAppendingComponent(FileSystem::stringFromFileSystemRepresentation(execDirectory), processName);
        if (FileSystem::fileExists(processPath))
            return processPath;
    }

    static String executablePath = getExecutablePath();
    if (!executablePath.isNull()) {
        String processPath = FileSystem::pathByAppendingComponent(executablePath, processName);
        if (FileSystem::fileExists(processPath))
            return processPath;
    }
#endif

    return FileSystem::pathByAppendingComponent(FileSystem::stringFromFileSystemRepresentation(PKGLIBEXECDIR), processName);
}

String executablePathOfWebProcess()
{
#if PLATFORM(WPE)
    return findWebKitProcess("WPEWebProcess"_s);
#else
    return findWebKitProcess("WebKitWebProcess"_s);
#endif
}

String executablePathOfNetworkProcess()
{
#if PLATFORM(WPE)
    return findWebKitProcess("WPENetworkProcess"_s);
#else
    return findWebKitProcess("WebKitNetworkProcess"_s);
#endif
}

#if ENABLE(GPU_PROCESS)
String executablePathOfGPUProcess()
{
#if PLATFORM(WPE)
    return findWebKitProcess("WPEGPUProcess"_s);
#else
    return findWebKitProcess("WebKitGPUProcess"_s);
#endif
}
#endif

} // namespace WebKit
