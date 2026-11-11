# Cross-compiles for Windows (MSVC ABI) on x64 or arm64 from a Linux or macOS
# host with clang-cl and lld-link. The Windows SDK and CRT come from
# WebKitLibraries/windows (see Source/cmake/OptionsMSVC.cmake); the GLib stack
# comes from MSYS2's clang64 (x64) or clangarm64 (arm64) packages, unpacked in
# sysroot/ beside this file by fetch-deps.py (all C, so their ABI is the same
# under either compiler).
#
# WEBKIT_WIN_ARCH (x64 or arm64) names the target and WEBKIT_LLVM_BIN the
# directory of the LLVM tools (clang-cl, llvm-lib, llvm-rc, llvm-mt, and
# lld-link there or on PATH), each a cache variable or else an environment
# variable; without WEBKIT_LLVM_BIN the tools are looked for on PATH.
if (NOT WEBKIT_WIN_ARCH)
    set(WEBKIT_WIN_ARCH "$ENV{WEBKIT_WIN_ARCH}")
endif ()
if (NOT WEBKIT_LLVM_BIN)
    set(WEBKIT_LLVM_BIN "$ENV{WEBKIT_LLVM_BIN}")
endif ()
# try_compile projects read this file again, without the cache.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES WEBKIT_WIN_ARCH WEBKIT_LLVM_BIN)

if (WEBKIT_WIN_ARCH STREQUAL "x64")
    set(CMAKE_SYSTEM_PROCESSOR AMD64)
    set(WEBKIT_WIN_TRIPLE x86_64-pc-windows-msvc)
    set(WEBKIT_WIN_DEPS_ENVIRONMENT clang64)
elseif (WEBKIT_WIN_ARCH STREQUAL "arm64")
    set(CMAKE_SYSTEM_PROCESSOR ARM64)
    set(WEBKIT_WIN_TRIPLE aarch64-pc-windows-msvc)
    set(WEBKIT_WIN_DEPS_ENVIRONMENT clangarm64)
else ()
    message(FATAL_ERROR "WEBKIT_WIN_ARCH is x64 or arm64, not '${WEBKIT_WIN_ARCH}'")
endif ()
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)

if (WEBKIT_LLVM_BIN)
    set(WEBKIT_LLVM_HINTS HINTS "${WEBKIT_LLVM_BIN}" NO_DEFAULT_PATH)
endif ()
find_program(WEBKIT_CLANG_CL clang-cl ${WEBKIT_LLVM_HINTS} REQUIRED)
find_program(WEBKIT_LLVM_LIB llvm-lib ${WEBKIT_LLVM_HINTS} REQUIRED)
find_program(WEBKIT_LLVM_RANLIB llvm-ranlib ${WEBKIT_LLVM_HINTS} REQUIRED)
find_program(WEBKIT_LLVM_RC llvm-rc ${WEBKIT_LLVM_HINTS} REQUIRED)
find_program(WEBKIT_LLVM_MT llvm-mt ${WEBKIT_LLVM_HINTS} REQUIRED)
# Homebrew keeps lld in a formula of its own, so the linker may be on PATH only.
find_program(WEBKIT_LLD_LINK lld-link HINTS "${WEBKIT_LLVM_BIN}" REQUIRED)

set(CMAKE_C_COMPILER ${WEBKIT_CLANG_CL} CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER ${WEBKIT_CLANG_CL} CACHE FILEPATH "")
set(CMAKE_C_COMPILER_TARGET ${WEBKIT_WIN_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${WEBKIT_WIN_TRIPLE})
set(CMAKE_LINKER ${WEBKIT_LLD_LINK} CACHE FILEPATH "")
set(CMAKE_AR ${WEBKIT_LLVM_LIB} CACHE FILEPATH "")
set(CMAKE_RANLIB ${WEBKIT_LLVM_RANLIB} CACHE FILEPATH "")
set(CMAKE_RC_COMPILER ${WEBKIT_LLVM_RC} CACHE FILEPATH "")
set(CMAKE_MT ${WEBKIT_LLVM_MT} CACHE FILEPATH "")
# The "hybrid CRT": the C++ runtime (vcruntime, the STL) linked into every
# module, so that the engine needs no Visual C++ redistributable, and the UCRT
# from Windows (ucrtbase.dll) instead of its static copy, so that every module,
# the MSYS2 libraries' included, allocates and frees on one heap. Release
# only: the SDK splat carries the release CRT only.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(WEBKIT_WIN_HYBRID_CRT "/NODEFAULTLIB:libucrt.lib ucrt.lib")
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# CMake checks the compilers before the port's options name the SDK.
get_filename_component(WEBKIT_WIN_SDK "${CMAKE_CURRENT_LIST_DIR}/../../WebKitLibraries/windows" ABSOLUTE)
set(WEBKIT_WIN_INCLUDES "-imsvc ${WEBKIT_WIN_SDK}/crt/include -imsvc ${WEBKIT_WIN_SDK}/sdk/include/ucrt -imsvc ${WEBKIT_WIN_SDK}/sdk/include/um -imsvc ${WEBKIT_WIN_SDK}/sdk/include/shared -imsvc ${WEBKIT_WIN_SDK}/sdk/include/winrt")
set(WEBKIT_WIN_LIBPATHS "-libpath:${WEBKIT_WIN_SDK}/crt/lib/${WEBKIT_WIN_ARCH} -libpath:${WEBKIT_WIN_SDK}/sdk/lib/ucrt/${WEBKIT_WIN_ARCH} -libpath:${WEBKIT_WIN_SDK}/sdk/lib/um/${WEBKIT_WIN_ARCH}")
# Precompiled headers without their inputs' timestamps: the same headers make
# the same PCH in every build directory, so that ccache finds what was compiled
# against it in another one (ninja rebuilds a PCH whose headers change).
set(CMAKE_C_FLAGS_INIT "${WEBKIT_WIN_INCLUDES} -Xclang -fno-pch-timestamp")
set(CMAKE_CXX_FLAGS_INIT "${WEBKIT_WIN_INCLUDES} -Xclang -fno-pch-timestamp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${WEBKIT_WIN_LIBPATHS} ${WEBKIT_WIN_HYBRID_CRT}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${WEBKIT_WIN_LIBPATHS} ${WEBKIT_WIN_HYBRID_CRT}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${WEBKIT_WIN_LIBPATHS} ${WEBKIT_WIN_HYBRID_CRT}")

set(WEBKIT_WIN_DEPS "${CMAKE_CURRENT_LIST_DIR}/sysroot/${WEBKIT_WIN_DEPS_ENVIRONMENT}")
set(CMAKE_FIND_ROOT_PATH "${WEBKIT_WIN_DEPS}" "${WEBKIT_WIN_SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# clang-cl takes an absolute path starting with a flag's name (/Users..., /work...)
# for that flag.
set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_LIST_DIR}/override.cmake")
