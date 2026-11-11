# Copyright (C) 2026 Stencil
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# libgioboringssl.so: a GIO TLS backend on BoringSSL whose clients send Safari's ClientHello and
# whose trust decisions are the Security framework's on an Apple OS, and elsewhere those of the
# system's CA bundle.
#
# The connection, certificate and BIO code is glib-networking's OpenSSL backend, compiled from a
# pinned, patched source tree (WebKitBuild/darwin/build-deps.sh prepares it and passes its path);
# the files in this directory are what the backend adds. BoringSSL is linked statically and nothing
# but the module's entry points is exported, so its symbols never meet those of the OpenSSL
# libcrypto the engine links.

cmake_minimum_required(VERSION 3.20)
project(GIOBoringSSL C)

set(GLIB_NETWORKING_SOURCE_DIR "" CACHE PATH "glib-networking source tree with glib-networking-boringssl.patch applied")
set(BORINGSSL_ROOT "" CACHE PATH "BoringSSL install prefix (include/, lib/libssl.a, lib/libcrypto.a)")
foreach (required GLIB_NETWORKING_SOURCE_DIR BORINGSSL_ROOT)
    if (NOT EXISTS "${${required}}")
        message(FATAL_ERROR "${required} must name an existing directory")
    endif ()
endforeach ()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.73.3 gmodule-2.0)
find_package(ZLIB REQUIRED)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/config.h" "#define GETTEXT_PACKAGE \"glib-networking\"\n#define ENABLE_DEBUG_LOGS 0\n")

set(tls_dir "${GLIB_NETWORKING_SOURCE_DIR}/tls")
add_library(gioboringssl MODULE
    GIOBoringSSLModule.c
    SafariClientHello.c

    ${tls_dir}/base/gtlsconnection-base.c
    ${tls_dir}/base/gtlsinputstream.c
    ${tls_dir}/base/gtlslog.c
    ${tls_dir}/base/gtlsoutputstream.c
    ${tls_dir}/base/gtlssessioncache.c
    ${tls_dir}/base/gtlsutils.c

    ${tls_dir}/openssl/gtlsbackend-openssl.c
    ${tls_dir}/openssl/gtlsbio.c
    ${tls_dir}/openssl/gtlscertificate-openssl.c
    ${tls_dir}/openssl/gtlsclientconnection-openssl.c
    ${tls_dir}/openssl/gtlsconnection-openssl.c
    ${tls_dir}/openssl/gtlsdatabase-openssl.c
    ${tls_dir}/openssl/gtlsfiledatabase-openssl.c
    ${tls_dir}/openssl/gtlsserverconnection-openssl.c
)
if (APPLE)
    target_sources(gioboringssl PRIVATE GTlsDatabaseSecTrust.c)
endif ()

set_target_properties(gioboringssl PROPERTIES
    PREFIX "lib"
    SUFFIX ".so"
    C_STANDARD 99
    C_EXTENSIONS ON
    C_VISIBILITY_PRESET hidden
)

target_compile_definitions(gioboringssl PRIVATE
    HAVE_CONFIG_H
    G_LOG_DOMAIN="GLib-Net"
    G_LOG_USE_STRUCTURED
    G_DISABLE_DEPRECATED
    GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_70
    # What glib-networking-boringssl.patch does as Safari on an Apple OS, the Linux overlay
    # (WebKitBuild/linux/patches/glib-networking-boringssl-linux.patch) does wherever this is set.
    GIO_BORINGSSL_SAFARI
)

target_include_directories(gioboringssl PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${GLIB_NETWORKING_SOURCE_DIR}"
    "${tls_dir}/base"
    "${tls_dir}/openssl"
    "${BORINGSSL_ROOT}/include"
)

target_compile_options(gioboringssl PRIVATE -Werror=implicit-function-declaration)

# BoringSSL is C++ inside; -load_hidden (--exclude-libs for ELF) keeps every symbol of the two
# archives out of the module's export table whatever visibility they were compiled with.
if (APPLE)
    target_link_libraries(gioboringssl PRIVATE
        PkgConfig::GIO
        ZLIB::ZLIB
        "-Wl,-load_hidden,${BORINGSSL_ROOT}/lib/libssl.a"
        "-Wl,-load_hidden,${BORINGSSL_ROOT}/lib/libcrypto.a"
        c++
        "-framework Security"
        "-framework CoreFoundation"
    )
else ()
    find_package(Threads REQUIRED)
    target_link_libraries(gioboringssl PRIVATE
        PkgConfig::GIO
        ZLIB::ZLIB
        "${BORINGSSL_ROOT}/lib/libssl.a"
        "${BORINGSSL_ROOT}/lib/libcrypto.a"
        "-Wl,--exclude-libs,libssl.a:libcrypto.a"
        "-Wl,--no-undefined"
        stdc++
        Threads::Threads
    )
endif ()

install(TARGETS gioboringssl LIBRARY DESTINATION lib/gio/modules)
