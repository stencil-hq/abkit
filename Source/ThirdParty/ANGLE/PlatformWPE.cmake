if (is_apple)
    # On an Apple OS ANGLE is the EGL itself, drawing with Metal as it does
    # for the Cocoa port; none of the system-EGL sources below apply.
    include(PlatformCocoa.cmake)
    # Unlike the Cocoa port, WPE uses one share group from several threads at
    # once: Skia's painting workers, the compositor and the main thread, and
    # every WebGL context hands its drawing buffer's texture to the
    # compositor through it. Shared objects and their name tables are only
    # synchronized with the share-context lock, here one mutex per share
    # group as in ANGLE's own build (BUILD.gn).
    list(APPEND ANGLE_DEFINITIONS
        ANGLE_ENABLE_SHARE_CONTEXT_LOCK=1
        ANGLE_ENABLE_CONTEXT_MUTEX=1
    )
    # The Cocoa port links the Quartz umbrella framework on a Mac. Its PDF and Quick Look parts load
    # the system's WebKit into every process, which then holds two copies of the WebRTC classes.
    # QuartzCore is the part ANGLE uses.
    if (QUARTZ_LIBRARY MATCHES "Quartz\\.framework")
        find_library(QUARTZCORE_LIBRARY QuartzCore)
        list(REMOVE_ITEM ANGLEGLESv2_LIBRARIES ${QUARTZ_LIBRARY})
        list(APPEND ANGLEGLESv2_LIBRARIES ${QUARTZCORE_LIBRARY})
    endif ()

    # The Metal backend draws its blits, clears and mipmaps with an internal shader library.
    # Compiled from source as each web process starts, it costs that process up to two seconds
    # whenever the system's Metal compiler cache misses (a new engine build, an evicted cache),
    # and the page's first load waits behind it. It is compiled here instead and loaded as a
    # binary (ANGLE_METAL_HAS_PREBUILT_INTERNAL_SHADERS, as in ANGLE's own build, BUILD.gn),
    # with the Metal toolchain of a developer directory: ANGLE_METAL_DEVELOPER_DIR, else the
    # first of DEVELOPER_DIR, xcode-select's and /Applications/Xcode.app that has one. Since
    # Xcode 26 the toolchain is a component of its own: `xcodebuild -downloadComponent
    # MetalToolchain`.
    set(ANGLE_METAL_DEVELOPER_DIR "" CACHE PATH "Developer directory whose Metal toolchain compiles ANGLE's internal shaders")
    execute_process(COMMAND xcode-select -p OUTPUT_VARIABLE _angle_selected_developer_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(_angle_metal_developer_dir "")
    foreach (_angle_candidate IN ITEMS "${ANGLE_METAL_DEVELOPER_DIR}" "$ENV{DEVELOPER_DIR}" "${_angle_selected_developer_dir}" "/Applications/Xcode.app/Contents/Developer")
        if (NOT _angle_candidate OR NOT IS_DIRECTORY "${_angle_candidate}")
            continue ()
        endif ()
        execute_process(COMMAND ${CMAKE_COMMAND} -E env DEVELOPER_DIR=${_angle_candidate} xcrun -sdk macosx -f metal RESULT_VARIABLE _angle_metal_found OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND ${CMAKE_COMMAND} -E env DEVELOPER_DIR=${_angle_candidate} xcrun -sdk macosx -f metallib RESULT_VARIABLE _angle_metallib_found OUTPUT_QUIET ERROR_QUIET)
        if (_angle_metal_found EQUAL 0 AND _angle_metallib_found EQUAL 0)
            set(_angle_metal_developer_dir "${_angle_candidate}")
            break ()
        endif ()
    endforeach ()
    if (NOT _angle_metal_developer_dir)
        message(FATAL_ERROR "ANGLE's internal Metal shaders need the Metal toolchain (metal, metallib): install Xcode and run `xcodebuild -downloadComponent MetalToolchain`, or set ANGLE_METAL_DEVELOPER_DIR to a developer directory that has it.")
    endif ()
    message(STATUS "ANGLE's internal Metal shaders: Metal toolchain of ${_angle_metal_developer_dir}")

    set(_angle_metal_dir "${CMAKE_CURRENT_BINARY_DIR}/metal")
    set(_angle_metal_source "${CMAKE_CURRENT_SOURCE_DIR}/src/libANGLE/renderer/metal/shaders/mtl_internal_shaders_autogen.metal")
    set(_angle_metal_air "${_angle_metal_dir}/mtl_internal_shaders_autogen.air")
    set(_angle_metal_lib "${_angle_metal_dir}/mtl_internal_shaders_autogen.metallib")
    set(_angle_metal_header "${_angle_metal_dir}/mtl_internal_shaders_metallib.h")
    set(_angle_xcrun ${CMAKE_COMMAND} -E env DEVELOPER_DIR=${_angle_metal_developer_dir} xcrun -sdk macosx)
    add_custom_command(
        OUTPUT ${_angle_metal_air}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_angle_metal_dir}
        COMMAND ${_angle_xcrun} metal -c ${_angle_metal_source} -o ${_angle_metal_air} --std=macos-metal2.1 -mmacosx-version-min=10.14
        DEPENDS ${_angle_metal_source}
        COMMENT "Compiling ANGLE's internal Metal shaders"
        VERBATIM)
    add_custom_command(
        OUTPUT ${_angle_metal_lib}
        COMMAND ${_angle_xcrun} metallib ${_angle_metal_air} -o ${_angle_metal_lib}
        DEPENDS ${_angle_metal_air}
        VERBATIM)
    add_custom_command(
        OUTPUT ${_angle_metal_header}
        COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/libANGLE/renderer/metal/shaders/embed_in_header.py
            --source ${_angle_metal_lib} --variable-name gDefaultMetallib --header ${_angle_metal_header}
        DEPENDS ${_angle_metal_lib} ${CMAKE_CURRENT_SOURCE_DIR}/src/libANGLE/renderer/metal/shaders/embed_in_header.py
        VERBATIM)
    list(APPEND ANGLE_SOURCES ${_angle_metal_header})
    list(APPEND ANGLE_PRIVATE_INCLUDE_DIRECTORIES ${_angle_metal_dir})
    list(APPEND ANGLE_DEFINITIONS ANGLE_METAL_HAS_PREBUILT_INTERNAL_SHADERS)
    return ()
endif ()

if (is_win)
    # On Windows ANGLE is the EGL itself too, drawing with Direct3D 11 as it
    # does for the Win port.
    include(PlatformWin.cmake)
    # Unlike the Win port, WPE draws from several threads at once (Skia's
    # painting workers, the compositor, the main thread), each with its own
    # context. Direct3D 11 contexts share one device context and one state
    # cache per display, so every call must hold the global lock and treat a
    # call on another context than the last as a context switch; without
    # both, tiles repainted after the first frame drew with another thread's
    # state (missing or garbage pixels) or crashed in StateManager11. EGL
    # calls hold the global lock too (global_state.h): eglSwapBuffers on the
    # compositor thread presented while the painting workers drew, and
    # Intel's threaded driver crashed in its Present. The lock is recursive,
    # as some EGL entry points hold it already.
    list(APPEND ANGLE_DEFINITIONS
        ANGLE_ENABLE_SHARE_CONTEXT_LOCK=1
        ANGLE_FORCE_CONTEXT_CHECK_EVERY_CALL=1
        ANGLE_ENABLE_GLOBAL_MUTEX_RECURSION=1
    )
    # The web process presents into its embedder's window: with a flip-model
    # swap chain at most one frame ahead (see NativeWindow11Win32.cpp).
    list(APPEND ANGLE_DEFINITIONS ANGLE_HWND_FLIP_MODEL_SWAP_CHAIN=1)
    return ()
endif ()

if (is_android)
    list(APPEND ANGLE_DEFINITIONS ANGLE_PLATFORM_ANDROID)
else ()
    list(APPEND ANGLE_DEFINITIONS ANGLE_PLATFORM_LINUX EGL_NO_PLATFORM_SPECIFIC_TYPES)
endif ()
include(linux.cmake)

if (USE_OPENGL)
    # Enable GLSL compiler output.
    list(APPEND ANGLE_DEFINITIONS ANGLE_ENABLE_GLSL)
endif ()

if (USE_ANGLE_EGL OR ENABLE_WEBGL)
    list(APPEND ANGLE_SOURCES
        ${gl_backend_sources}

        ${angle_system_utils_sources_linux}
        ${angle_system_utils_sources_posix}

        ${angle_dma_buf_sources}

        ${libangle_gl_egl_dl_sources}
        ${libangle_gl_egl_sources}
        ${libangle_gl_sources}

        ${libangle_gpu_info_util_sources}
        ${libangle_gpu_info_util_linux_sources}
    )

    list(APPEND ANGLE_DEFINITIONS
        ANGLE_ENABLE_OPENGL
        USE_SYSTEM_EGL
    )

    list(APPEND ANGLEGLESv2_LIBRARIES
        ${CMAKE_DL_LIBS}
        Threads::Threads
    )
endif ()
