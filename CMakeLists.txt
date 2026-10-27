cmake_minimum_required(VERSION 3.21)
project(XPreview VERSION 1.0.0 LANGUAGES C CXX)
# SPDX-License-Identifier: AGPL-3.0-or-later

set(APP_DISPLAY_NAME "XPreview+")
set(APP_COPYRIGHT "Copyright (C) 2026 Sanjeeva Wijeyesakere")
set(APP_SHORT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
set(APP_SOURCE_URL "https://github.com/swijeyes/XPreview-Plus" CACHE STRING
    "Public source repository (shown in About, required by the AGPL)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# --- Hardening -------------------------------------------------------------
option(XPREVIEW_SANITIZE "Build with AddressSanitizer + UBSan (for fuzzing/testing)" OFF)
if(MSVC)
    add_compile_options(/guard:cf /sdl /GS)
    add_link_options(/guard:cf /DYNAMICBASE /NXCOMPAT /CETCOMPAT)
else()
    add_compile_options(-fstack-protector-strong -Wformat -Wformat-security -Werror=format-security)
    if(MINGW)
        # PE hardening: ASLR (incl. high-entropy 64-bit), DEP, stack cookies.
        add_link_options(-Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -fstack-protector-strong)
    elseif(NOT APPLE)
        add_compile_options(-fstack-clash-protection -fPIE)
        add_link_options(-pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
    endif()
    if(NOT XPREVIEW_SANITIZE AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_definitions(_FORTIFY_SOURCE=2)
    endif()
endif()
if(XPREVIEW_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg PrintSupport Concurrent Test)
# miniz: system package if present, else the vendored single-file release
# (used by the Windows cross build; see scripts/fetch-windows-deps.sh).
find_package(miniz CONFIG QUIET)
if(NOT miniz_FOUND)
    set(MINIZ_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/windows/miniz" CACHE PATH "miniz amalgamated sources")
    add_library(miniz_vendored STATIC "${MINIZ_SOURCE_DIR}/miniz.c")
    target_include_directories(miniz_vendored PUBLIC "${MINIZ_SOURCE_DIR}")
    add_library(miniz::miniz ALIAS miniz_vendored)
endif()

# PDFium: prebuilt binaries (https://github.com/bblanchon/pdfium-binaries).
# Run scripts/fetch-deps.sh, or point PDFIUM_DIR at an extracted package.
set(PDFIUM_DIR "${CMAKE_SOURCE_DIR}/third_party/pdfium" CACHE PATH "PDFium binary package")
find_path(PDFIUM_INCLUDE_DIR fpdfview.h PATHS "${PDFIUM_DIR}/include" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(PDFIUM_LIBRARY NAMES pdfium pdfium.dll PATHS "${PDFIUM_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
if(WIN32)
    set(PDFIUM_RUNTIME "${PDFIUM_DIR}/bin/pdfium.dll")
else()
    set(PDFIUM_RUNTIME "${PDFIUM_LIBRARY}")
endif()

# Core library: everything except the UI, so tests and tools can link it.
add_library(xpreview_core STATIC
    src/core/Document.cpp
    src/core/DocumentLoader.cpp
    src/core/Exporter.cpp
    src/core/FontSubstitution.cpp
    src/core/FormatSniffer.cpp
    src/core/SafeImage.cpp
    src/pdf/PdfDocument.cpp
    src/image/ImageDocument.cpp
    src/flow/FlowBuilder.cpp
    src/flow/FlowDocument.cpp
    src/rtf/RtfTokenizer.cpp
    src/rtf/RtfImporter.cpp
    src/odf/OdfPackage.cpp
    src/odf/OdfStyles.cpp
    src/odf/OdtImporter.cpp
    resources/resources.qrc
)
target_include_directories(xpreview_core PUBLIC src PRIVATE ${PDFIUM_INCLUDE_DIR})
target_compile_definitions(xpreview_core
    PUBLIC
        XPREVIEW_DISPLAY_NAME="${APP_DISPLAY_NAME}"
        XPREVIEW_VERSION="${APP_SHORT_VERSION}"
        XPREVIEW_SOURCE_URL="${APP_SOURCE_URL}"
        XPREVIEW_COPYRIGHT="${APP_COPYRIGHT}"
    PRIVATE
        XPREVIEW_DEV_FONT_DIR="${CMAKE_SOURCE_DIR}/resources/fonts")
target_link_libraries(xpreview_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Svg Qt6::PrintSupport Qt6::Concurrent
    miniz::miniz ${PDFIUM_LIBRARY})

# UI widgets as a library so the UI regression tests can link them.
add_library(xpreview_ui STATIC
    src/ui/MainWindow.cpp
    src/ui/DocumentTab.cpp
    src/ui/DocumentView.cpp
    src/ui/RenderService.cpp
    src/ui/ThumbnailSidebar.cpp
    src/ui/InfoBar.cpp
)
target_link_libraries(xpreview_ui PUBLIC xpreview_core Qt6::Widgets)

set(APP_SOURCES src/main.cpp)

if(APPLE)
    set(APP_ICON "${CMAKE_SOURCE_DIR}/resources/XPreview.icns")
    if(EXISTS "${APP_ICON}")
        set_source_files_properties("${APP_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        list(APPEND APP_SOURCES "${APP_ICON}")
    endif()
endif()

if(WIN32)
    # Icon + version information shown in Explorer's file properties.
    configure_file(resources/XPreview.rc.in "${CMAKE_BINARY_DIR}/XPreview.rc" @ONLY)
    list(APPEND APP_SOURCES "${CMAKE_BINARY_DIR}/XPreview.rc")
endif()

add_executable(XPreview MACOSX_BUNDLE WIN32 ${APP_SOURCES})
target_link_libraries(XPreview PRIVATE xpreview_ui)
set_target_properties(XPreview PROPERTIES OUTPUT_NAME "${APP_DISPLAY_NAME}")
# License texts travel with every binary distribution.
set(LICENSE_FILES "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md")

if(APPLE)
    set_target_properties(XPreview PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.xpreview.XPreview"
        MACOSX_BUNDLE_BUNDLE_NAME "${APP_DISPLAY_NAME}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${APP_SHORT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "${APP_COPYRIGHT}. Licensed under the GNU AGPL v3 or later."
        INSTALL_RPATH "@executable_path/../Frameworks"
        BUILD_WITH_INSTALL_RPATH ON)
    # Ship PDFium and bundled fonts inside the .app.
    add_custom_command(TARGET XPreview POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:XPreview>/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PDFIUM_RUNTIME}"
                "$<TARGET_BUNDLE_CONTENT_DIR:XPreview>/Frameworks/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/resources/fonts"
                "$<TARGET_BUNDLE_CONTENT_DIR:XPreview>/Resources/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LICENSE_FILES}
                "$<TARGET_BUNDLE_CONTENT_DIR:XPreview>/Resources/")
elseif(UNIX)
    set_target_properties(XPreview PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
    install(TARGETS XPreview RUNTIME DESTINATION bin)
    install(FILES "${PDFIUM_RUNTIME}" DESTINATION lib)
    install(DIRECTORY resources/fonts DESTINATION share/xpreview)
    install(FILES packaging/linux/xpreview.desktop DESTINATION share/applications OPTIONAL)
    install(FILES ${LICENSE_FILES} DESTINATION share/doc/xpreview)
elseif(WIN32)
    add_custom_command(TARGET XPreview POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PDFIUM_RUNTIME}" "$<TARGET_FILE_DIR:XPreview>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/resources/fonts"
                "$<TARGET_FILE_DIR:XPreview>/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LICENSE_FILES} "$<TARGET_FILE_DIR:XPreview>")
endif()

# Headless render tool: renders every page of a file to PNGs (golden tests,
# debugging importers) and can export to PDF.
add_executable(xpreview-render tests/render_tool.cpp)
target_link_libraries(xpreview-render PRIVATE xpreview_core)
set_target_properties(xpreview-render PROPERTIES BUILD_RPATH "${PDFIUM_DIR}/lib")

enable_testing()
add_executable(xpreview-tests tests/tst_importers.cpp)
target_link_libraries(xpreview-tests PRIVATE xpreview_core Qt6::Test)
set_target_properties(xpreview-tests PROPERTIES BUILD_RPATH "${PDFIUM_DIR}/lib")
add_test(NAME importers COMMAND xpreview-tests)

add_executable(xpreview-ui-tests tests/tst_ui.cpp)
target_link_libraries(xpreview-ui-tests PRIVATE xpreview_ui Qt6::Test)
set_target_properties(xpreview-ui-tests PROPERTIES BUILD_RPATH "${PDFIUM_DIR}/lib")
# Headless; Fusion has classic (space-taking) scrollbars, unlike macOS overlays.
add_test(NAME ui COMMAND xpreview-ui-tests -platform offscreen)
