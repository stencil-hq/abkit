set(PdfJSExtraFiles
    ${WEBCORE_DIR}/Modules/pdfjs-extras/content-script.js
)

# A Darwin host renders the viewer with these, so they are in the bundle and
# regenerate it when changed; see Source/WebKit/PdfJSGResources.cmake.
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND PdfJSExtraFiles
        ${WEBCORE_DIR}/Modules/pdfjs-extras/cocoa/arrow.down.circle.svg
        ${WEBCORE_DIR}/Modules/pdfjs-extras/cocoa/minus.magnifyingglass.svg
        ${WEBCORE_DIR}/Modules/pdfjs-extras/cocoa/plus.magnifyingglass.svg
        ${WEBCORE_DIR}/Modules/pdfjs-extras/cocoa/style.css
    )
endif ()

if (USE_THEME_ADWAITA)
    list(APPEND PdfJSExtraFiles
        ${WEBCORE_DIR}/Modules/pdfjs-extras/adwaita/style.css
    )
endif ()
