# WPEAgentHost runs the engine as a headless browser for a parent process, which drives it over the
# host's standard streams. It is an embedder like any other: it uses the installed API only.

set(WPEAgentHost_SOURCES
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

set(WPEAgentHost_PRIVATE_INCLUDE_DIRECTORIES
    ${CMAKE_BINARY_DIR}
    ${DERIVED_SOURCES_WEBKIT_DIR}
    ${FORWARDING_HEADERS_WPE_DIR}
    ${FORWARDING_HEADERS_WPE_JSC_DIR}
    ${JavaScriptCoreGLib_DERIVED_SOURCES_DIR}
    ${JavaScriptCoreGLib_FRAMEWORK_HEADERS_DIR}
    ${WPEPlatform_DERIVED_SOURCES_DIR}
    ${WEBKIT_DIR}/WPEPlatform
)

set(WPEAgentHost_PRIVATE_LIBRARIES
    JPEG::JPEG
    PNG::PNG
    WebKit
)

if (APPLE)
    # Frames go to the client as IOSurfaces, over Mach messages.
    list(APPEND WPEAgentHost_PRIVATE_LIBRARIES "-framework IOSurface")
    # The Mac's appearance is read from the global preferences.
    list(APPEND WPEAgentHost_PRIVATE_LIBRARIES "-framework CoreFoundation")
endif ()

WEBKIT_EXECUTABLE_DECLARE(WPEAgentHost)
WEBKIT_EXECUTABLE(WPEAgentHost)

if (APPLE)
    # WPEAgentView puts the host's presented blocks in windows. It is a client of the host like any
    # other, in a process of its own: it links nothing of the engine.
    add_executable(WPEAgentView View/WPEAgentView.m)
    target_compile_options(WPEAgentView PRIVATE -fobjc-arc)
    target_link_libraries(WPEAgentView PRIVATE
        "-framework AppKit"
        "-framework Carbon"
        "-framework IOSurface"
        "-framework QuartzCore"
    )
endif ()

# The host loads its web process extension from "extensions" beside its own executable.
add_library(WPEAgentHostExtension MODULE Extension/AgentHostExtension.cpp)
target_include_directories(WPEAgentHostExtension PRIVATE
    ${CMAKE_BINARY_DIR}
    ${DERIVED_SOURCES_WEBKIT_DIR}
    ${FORWARDING_HEADERS_WPE_DIR}
    ${FORWARDING_HEADERS_WPE_JSC_DIR}
    ${JavaScriptCoreGLib_DERIVED_SOURCES_DIR}
    ${JavaScriptCoreGLib_FRAMEWORK_HEADERS_DIR}
)
target_link_libraries(WPEAgentHostExtension PRIVATE WebKit)
set_target_properties(WPEAgentHostExtension PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/extensions"
)
