include(platform/Adwaita.cmake)
include(platform/CoordinatedGraphics.cmake)
include(platform/GStreamer.cmake)
include(platform/ImageDecoders.cmake)
include(platform/Skia.cmake)
include(platform/Soup.cmake)

if (USE_OPENSSL)
    include(platform/OpenSSL.cmake)
    list(APPEND WebCore_LIBRARIES OpenSSL::Crypto)
    # The OpenSSL backend uses APIs deprecated in OpenSSL 3, see bug 245146.
    list(APPEND WebCore_PRIVATE_DEFINITIONS OPENSSL_API_COMPAT=0x10100000L)
else ()
    include(platform/GCrypt.cmake)
endif ()

if (USE_EXTERNAL_HOLEPUNCH)
    include(platform/HolePunch.cmake)
endif ()

list(APPEND WebCore_UNIFIED_SOURCE_LIST_FILES
    "SourcesWPE.txt"

    "platform/SourcesGLib.txt"
)

list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    "${WEBCORE_DIR}/accessibility/atspi"
    "${WEBCORE_DIR}/crypto/openssl"
    "${WEBCORE_DIR}/platform/audio/glib"
    "${WEBCORE_DIR}/platform/glib"
    "${WEBCORE_DIR}/platform/graphics/android"
    "${WEBCORE_DIR}/platform/graphics/egl"
    "${WEBCORE_DIR}/platform/graphics/epoxy"
    "${WEBCORE_DIR}/platform/graphics/gbm"
    "${WEBCORE_DIR}/platform/graphics/gstreamer"
    "${WEBCORE_DIR}/platform/graphics/opengl"
    "${WEBCORE_DIR}/platform/graphics/opentype"
    "${WEBCORE_DIR}/platform/graphics/libwpe"
    "${WEBCORE_DIR}/platform/graphics/wayland"
    "${WEBCORE_DIR}/platform/mock/mediasource"
    "${WEBCORE_DIR}/platform/mediacapabilities"
    "${WEBCORE_DIR}/platform/mediastream/gstreamer"
    "${WEBCORE_DIR}/platform/network/glib"
    "${WEBCORE_DIR}/platform/text/icu"
    "${WEBCORE_DIR}/platform/video-codecs"
    "${WEBCORE_DIR}/platform/wpe"
)

list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
    accessibility/atspi/AccessibilityAtspi.h
    accessibility/atspi/AccessibilityAtspiEnums.h
    accessibility/atspi/AccessibilityObjectAtspi.h
    accessibility/atspi/AccessibilityRootAtspi.h

    platform/glib/ApplicationGLib.h
    platform/glib/SelectionData.h
    platform/glib/SystemSettings.h

    platform/graphics/DMABufBuffer.h
    platform/graphics/DMABufBufferAttributes.h

    platform/graphics/android/BufferFormatAndroid.h
    platform/graphics/android/GraphicsContextGLAndroid.h
    platform/graphics/android/PlatformDisplayAndroid.h

    platform/graphics/egl/PlatformDisplayDefault.h
    platform/graphics/egl/PlatformDisplaySurfaceless.h

    platform/graphics/gbm/GBMVersioning.h
    platform/graphics/gbm/PlatformDisplayGBM.h

    platform/graphics/libwpe/PlatformDisplayLibWPE.h

    platform/text/enchant/TextCheckerEnchant.h
)

set(WebCore_USER_AGENT_SCRIPTS_DEPENDENCIES ${WEBCORE_DIR}/platform/wpe/RenderThemeWPE.cpp)

if (APPLE)
    # On a Mac this port is a Mac as far as a page is concerned: the CSS value keyword
    # generator gets the keywords Safari's DOM has there but this port does not name otherwise.
    set(CSS_VALUE_PLATFORM_DEFINES "WTF_PLATFORM_WPE_DARWIN")
endif ()

if (APPLE)
    # Safari's WebKit has Apple Pay built in, and only its session interfaces are gated by the
    # ApplePayEnabled setting, so window.ApplePayError is visible while no payment can be made.
    # There is no Apple Pay implementation here, so the port generates the bindings of that
    # plain data type alone. The IDL list here is the port's, not the Cocoa list's copy of it.
    list(APPEND WebCore_IDL_FILES
        Modules/applepay/ApplePayError.idl
        Modules/applepay/ApplePayErrorCode.idl
        Modules/applepay/ApplePayErrorContactField.idl
    )
endif ()

list(APPEND WebCore_LIBRARIES
    GLib::Module
    ${UPOWERGLIB_LIBRARIES}
)

if (ENABLE_WPE_LEGACY_API)
    list(APPEND WebCore_LIBRARIES WPE::libwpe)
endif ()

if (APPLE)
    # The software filters use vImage wherever the OS is Darwin (USE(ACCELERATE)).
    list(APPEND WebCore_LIBRARIES "-framework Accelerate")
    # Canvases are encoded by ImageIO, as Safari's are (ImageUtilitiesSkiaDarwin.cpp).
    list(APPEND WebCore_LIBRARIES
        "-framework CoreGraphics"
        "-framework IOSurface"
        "-framework ImageIO"
    )
    # Canvas text is rasterised by Core Text (FontRasterizationSkiaDarwin.cpp).
    list(APPEND WebCore_LIBRARIES "-framework CoreText")
elseif (WIN32)
    # The processes share memory as the Win port's do, and file types come
    # from the registry.
    list(APPEND WebCore_SOURCES
        platform/network/soup/WebKitWindowsTLSDatabase.cpp
        platform/win/MIMETypeRegistryWin.cpp
        platform/win/SharedMemoryWin.cpp
    )
    # TLS servers are verified again as Windows verifies them (CryptoAPI).
    list(APPEND WebCore_LIBRARIES crypt32)
endif ()

list(APPEND WebCore_SYSTEM_INCLUDE_DIRECTORIES
    ${UPOWERGLIB_INCLUDE_DIRS}
)

if (USE_OPENXR)
    list(APPEND WebCore_LIBRARIES ${OPENXR_LIBRARIES})
    list(APPEND WebCore_SYSTEM_INCLUDE_DIRECTORIES ${OPENXR_INCLUDE_DIRS})
endif ()

if (ENABLE_DRAG_SUPPORT)
    list(APPEND WebCore_SOURCES
        platform/skia/DragImageSkia.cpp
    )
endif ()

if (ENABLE_SPELLCHECK)
    list(APPEND WebCore_LIBRARIES Enchant::Enchant)
endif ()

if (USE_ATSPI)
    set(WebCore_AtspiInterfaceFiles
        ${WEBCORE_DIR}/accessibility/atspi/xml/Accessible.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Action.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Application.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Cache.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Collection.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Component.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/DeviceEventController.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/DeviceEventListener.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Document.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/EditableText.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Event.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Hyperlink.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Hypertext.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Image.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Registry.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Selection.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Socket.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/TableCell.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Table.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Text.xml
        ${WEBCORE_DIR}/accessibility/atspi/xml/Value.xml
    )

    add_custom_command(
        OUTPUT ${WebCore_DERIVED_SOURCES_DIR}/AccessibilityAtspiInterfaces.h ${WebCore_DERIVED_SOURCES_DIR}/AccessibilityAtspiInterfaces.c
        DEPENDS ${WebCore_AtspiInterfaceFiles}
        COMMAND gdbus-codegen --interface-prefix=org.a11y.atspi --c-namespace=webkit --pragma-once --interface-info-header --output=${WebCore_DERIVED_SOURCES_DIR}/AccessibilityAtspiInterfaces.h ${WebCore_AtspiInterfaceFiles}
        COMMAND gdbus-codegen --interface-prefix=org.a11y.atspi --c-namespace=webkit --interface-info-body --output=${WebCore_DERIVED_SOURCES_DIR}/AccessibilityAtspiInterfaces.c ${WebCore_AtspiInterfaceFiles}
        VERBATIM
    )

    list(APPEND WebCore_SOURCES
        ${WebCore_DERIVED_SOURCES_DIR}/AccessibilityAtspiInterfaces.c
    )
endif ()

if (USE_GBM)
    list(APPEND WebCore_LIBRARIES GBM::GBM)
elseif (USE_LIBDRM)
    list(APPEND WebCore_LIBRARIES LibDRM::LibDRM)
endif ()

if (USE_LIBHYPHEN)
    list(APPEND WebCore_PRIVATE_LIBRARIES Hyphen::Hyphen)
endif ()

if (ENABLE_GAMEPAD)
    list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
        "${WEBCORE_DIR}/platform/gamepad/libwpe"
    )

    list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
        platform/gamepad/libwpe/GamepadProviderLibWPE.h
    )
endif ()

if (ENABLE_WEB_AUTHN)
    # The U2F response converter walks DER with helpers that live beside the Cocoa crypto code but
    # depend on nothing from it.
    list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
        "${WEBCORE_DIR}/Modules/webauthn/apdu"
        "${WEBCORE_DIR}/crypto/cocoa"
    )

    list(APPEND WebCore_SOURCES
        Modules/webauthn/AuthenticationExtensionsClientInputs.cpp
        Modules/webauthn/AuthenticationExtensionsClientOutputs.cpp

        crypto/cocoa/CommonCryptoDERUtilities.cpp
    )
endif ()

if (USE_VULKAN)
    list(APPEND WebCore_PRIVATE_LIBRARIES volk::volk)
    list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
        "${WEBCORE_DIR}/platform/graphics/vulkan"
    )
    list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
        platform/graphics/vulkan/VulkanHandle.h
        platform/graphics/vulkan/VulkanStructure.h
        platform/graphics/vulkan/VulkanTypes.h
        platform/graphics/vulkan/VulkanUtilities.h
        platform/graphics/vulkan/VulkanVolk.h
    )
endif ()

if (APPLE AND ENABLE_WEBGPU)
    # WebGPU's Metal backend, used in the web process. The canvas context is
    # the Cocoa ports', with the compositor's layer buffers in the place of
    # Core Animation's.
    list(APPEND WebCore_FRAMEWORKS WebGPU)
    list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES ${WebGPU_PRIVATE_FRAMEWORK_HEADERS_DIR})
    list(APPEND WebCore_INTERFACE_INCLUDE_DIRECTORIES ${WebGPU_PRIVATE_FRAMEWORK_HEADERS_DIR})
    list(APPEND WebCore_DEPENDENCIES WebGPU_CopyPrivateHeaders)
    list(APPEND WebCore_INTERFACE_DEPENDENCIES WebGPU_CopyPrivateHeaders)
    list(APPEND WebCore_SOURCES
        Modules/WebGPU/Implementation/WebGPUCompositorIntegrationImplSkia.cpp
        Modules/WebGPU/Implementation/WebGPUIOSurfaceImageSkia.cpp

        html/canvas/GPUCanvasContextCocoa.mm
        html/canvas/GPUDisplayBufferDisplayDelegateCoordinated.cpp
    )
    # The canvas context has no Objective-C in it, and WebCore no Objective-C++
    # prefix header on this port.
    set_source_files_properties(html/canvas/GPUCanvasContextCocoa.mm PROPERTIES
        LANGUAGE CXX
        COMPILE_OPTIONS "-xc++"
    )
endif ()

# This sets the maximum amount of memory that BitmapTexturePool can hold before being more
# aggressive trying to release the unused textures.
# Use a big value as the default size limit (80MB, enough for ten 1920x1080 layers).
# Embedded users will want to set this limit depending on the capabilities of their platform.
list(APPEND WebCore_PRIVATE_DEFINITIONS
     BITMAP_TEXTURE_POOL_MAX_SIZE_IN_MB=80
)
