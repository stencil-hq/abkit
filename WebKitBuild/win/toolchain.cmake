# Cross-compiles for x86_64 Windows (MSVC ABI) from macOS with clang-cl and
# lld-link. The Windows SDK and CRT come from WebKitLibraries/windows (see
# Source/cmake/OptionsMSVC.cmake); the GLib stack comes from MSYS2's clang64
# packages, unpacked in sysroot/ beside this file (all C, so their ABI is the
# same under either compiler).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_SYSTEM_VERSION 10.0)

set(LLVM_BIN /opt/homebrew/opt/llvm/bin)
set(CMAKE_C_COMPILER ${LLVM_BIN}/clang-cl CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER ${LLVM_BIN}/clang-cl CACHE FILEPATH "")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_LINKER /opt/homebrew/bin/lld-link CACHE FILEPATH "")
set(CMAKE_AR ${LLVM_BIN}/llvm-lib CACHE FILEPATH "")
set(CMAKE_RANLIB ${LLVM_BIN}/llvm-ranlib CACHE FILEPATH "")
set(CMAKE_RC_COMPILER ${LLVM_BIN}/llvm-rc CACHE FILEPATH "")
set(CMAKE_MT ${LLVM_BIN}/llvm-mt CACHE FILEPATH "")
# The SDK splat carries the release CRT only.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# CMake checks the compilers before the port's options name the SDK.
get_filename_component(WEBKIT_WIN_SDK "${CMAKE_CURRENT_LIST_DIR}/../../WebKitLibraries/windows" ABSOLUTE)
set(WEBKIT_WIN_INCLUDES "-imsvc ${WEBKIT_WIN_SDK}/crt/include -imsvc ${WEBKIT_WIN_SDK}/sdk/include/ucrt -imsvc ${WEBKIT_WIN_SDK}/sdk/include/um -imsvc ${WEBKIT_WIN_SDK}/sdk/include/shared -imsvc ${WEBKIT_WIN_SDK}/sdk/include/winrt")
set(WEBKIT_WIN_LIBPATHS "-libpath:${WEBKIT_WIN_SDK}/crt/lib/x64 -libpath:${WEBKIT_WIN_SDK}/sdk/lib/ucrt/x64 -libpath:${WEBKIT_WIN_SDK}/sdk/lib/um/x64")
set(CMAKE_C_FLAGS_INIT "${WEBKIT_WIN_INCLUDES}")
set(CMAKE_CXX_FLAGS_INIT "${WEBKIT_WIN_INCLUDES}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${WEBKIT_WIN_LIBPATHS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${WEBKIT_WIN_LIBPATHS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${WEBKIT_WIN_LIBPATHS}")

set(WEBKIT_WIN_DEPS "${CMAKE_CURRENT_LIST_DIR}/sysroot/clang64")
set(CMAKE_FIND_ROOT_PATH "${WEBKIT_WIN_DEPS}" "${WEBKIT_WIN_SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# clang-cl takes a path starting with /U (as in /Users) for a macro undefine.
set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_LIST_DIR}/override.cmake")
