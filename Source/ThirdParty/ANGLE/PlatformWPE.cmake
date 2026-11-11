if (is_apple)
    # On an Apple OS ANGLE is the EGL itself, drawing with Metal as it does
    # for the Cocoa port; none of the system-EGL sources below apply.
    include(PlatformCocoa.cmake)
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
    # state (missing or garbage pixels) or crashed in StateManager11.
    list(APPEND ANGLE_DEFINITIONS
        ANGLE_ENABLE_SHARE_CONTEXT_LOCK=1
        ANGLE_FORCE_CONTEXT_CHECK_EVERY_CALL=1
    )
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
