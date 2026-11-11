# ABHost runs the engine as a headless browser for a parent process, which drives it over the
# host's standard streams. It is an embedder like any other: it uses the installed API only.

set(ABHost_SOURCES
    Block.cpp
    BlockCapture.cpp
    BlockInput.cpp
    BlockNetwork.cpp
    BlockPage.cpp
    BlockPresent.cpp
    BlockScripts.cpp
    BlockSettings.cpp
    Display.cpp
    Host.cpp
    Image.cpp
    Json.cpp
    Keys.cpp
    Platform.cpp
    Present.cpp
    Transport.cpp
    main.cpp
)

set(ABHost_PRIVATE_INCLUDE_DIRECTORIES
    ${CMAKE_BINARY_DIR}
    ${DERIVED_SOURCES_WEBKIT_DIR}
    ${FORWARDING_HEADERS_WPE_DIR}
    ${FORWARDING_HEADERS_WPE_JSC_DIR}
    ${JavaScriptCoreGLib_DERIVED_SOURCES_DIR}
    ${JavaScriptCoreGLib_FRAMEWORK_HEADERS_DIR}
    ${WPEPlatform_DERIVED_SOURCES_DIR}
    ${WEBKIT_DIR}/WPEPlatform
)

set(ABHost_PRIVATE_LIBRARIES
    JPEG::JPEG
    PNG::PNG
    WebKit
)

if (APPLE)
    # Frames go to the client as IOSurfaces, over Mach messages.
    list(APPEND ABHost_PRIVATE_LIBRARIES "-framework IOSurface")
    # The Mac's appearance is read from the global preferences.
    list(APPEND ABHost_PRIVATE_LIBRARIES "-framework CoreFoundation")
endif ()

WEBKIT_EXECUTABLE_DECLARE(ABHost)
WEBKIT_EXECUTABLE(ABHost)

if (APPLE)
    # ABView puts the host's presented blocks in windows. It is a client of the host like any
    # other, in a process of its own: it links nothing of the engine.
    add_executable(ABView View/ABView.m)
    target_compile_options(ABView PRIVATE -fobjc-arc)
    target_link_libraries(ABView PRIVATE
        "-framework AppKit"
        "-framework Carbon"
        "-framework IOSurface"
        "-framework QuartzCore"
    )
endif ()

# The host loads its web process extension from "extensions" beside its own executable.
add_library(ABHostExtension MODULE Extension/AgentHostExtension.cpp)
target_include_directories(ABHostExtension PRIVATE
    ${CMAKE_BINARY_DIR}
    ${DERIVED_SOURCES_WEBKIT_DIR}
    ${FORWARDING_HEADERS_WPE_DIR}
    ${FORWARDING_HEADERS_WPE_JSC_DIR}
    ${JavaScriptCoreGLib_DERIVED_SOURCES_DIR}
    ${JavaScriptCoreGLib_FRAMEWORK_HEADERS_DIR}
)
target_link_libraries(ABHostExtension PRIVATE WebKit)
set_target_properties(ABHostExtension PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/extensions"
)
