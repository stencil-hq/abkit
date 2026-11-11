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
#include "BundledRuntime.h"

#if PLATFORM(WPE) && OS(DARWIN)

#include <glib.h>
#include <mutex>
#include <wtf/FileSystem.h>
#include <wtf/darwin/EngineLibrary.h>
#include <wtf/text/ASCIILiteral.h>

namespace WebCore {

static void setEnvironmentIfUnset(const char* name, const String& value)
{
    if (!g_getenv(name))
        g_setenv(name, FileSystem::fileSystemRepresentation(value).legacyCStringPointer(), TRUE);
}

void useBundledRuntimePathsIfPresent()
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        auto libraryDirectory = WTF::directoryOfEngineLibrary();
        if (libraryDirectory.isNull())
            return;
        auto libDirectory = FileSystem::pathByAppendingComponent(libraryDirectory, "lib"_s);

        // GStreamer looks for its plugins, for the scanner that reads them and for its
        // registry cache under the prefix it was built in, none of which a package has.
        auto pluginsDirectory = FileSystem::pathByAppendingComponent(libDirectory, "gstreamer-1.0"_s);
        if (g_file_test(FileSystem::fileSystemRepresentation(pluginsDirectory).legacyCStringPointer(), G_FILE_TEST_IS_DIR)) {
            setEnvironmentIfUnset("GST_PLUGIN_SYSTEM_PATH_1_0", pluginsDirectory);

            auto scanner = FileSystem::pathByAppendingComponent(pluginsDirectory, "gst-plugin-scanner"_s);
            if (FileSystem::fileExists(scanner))
                setEnvironmentIfUnset("GST_PLUGIN_SCANNER_1_0", scanner);

            // A registry written for the packaged plugins is a cache, so it belongs beside
            // the user's other caches rather than in the read-only package.
            if (!g_getenv("GST_REGISTRY_1_0") && !g_getenv("GST_REGISTRY")) {
                auto cacheDirectory = FileSystem::stringFromFileSystemRepresentation(g_get_user_cache_dir());
                auto registry = FileSystem::pathByAppendingComponent(FileSystem::pathByAppendingComponent(cacheDirectory, "gstreamer-1.0"_s), "registry.bin"_s);
                setEnvironmentIfUnset("GST_REGISTRY_1_0", registry);
            }
        }

        // fontconfig reads its configuration, and the conf.d directory beside it, from the
        // prefix it was built in.
        auto fontsDirectory = FileSystem::pathByAppendingComponent(libDirectory, "fonts"_s);
        auto fontsConfig = FileSystem::pathByAppendingComponent(fontsDirectory, "fonts.conf"_s);
        if (FileSystem::fileExists(fontsConfig)) {
            setEnvironmentIfUnset("FONTCONFIG_FILE", fontsConfig);
            setEnvironmentIfUnset("FONTCONFIG_PATH", fontsDirectory);
        }
    });
}

} // namespace WebCore

#endif // PLATFORM(WPE) && OS(DARWIN)
