cmake_minimum_required(VERSION 3.16)
project(QRProtecApp LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# 1. Recherche des dépendances système (GLFW et OpenGL)
# -----------------------------------------------------------------------------
find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)
find_package(PNG REQUIRED)
find_package(Freetype REQUIRED)
find_package(Threads REQUIRED)
# HTTPS vers un back distant (fronts sur d'autres machines)
find_package(OpenSSL REQUIRED)

set(INATECK_SDK_DEFAULT_LIBRARY
    "${CMAKE_CURRENT_SOURCE_DIR}/scanner_lib/ble/linux/x86_64-unknown-linux-gnu/libinateck_scanner_ble.so")
if(NOT DEFINED INATECK_SDK_LIBRARY OR
   (INATECK_SDK_LIBRARY STREQUAL "" AND EXISTS "${INATECK_SDK_DEFAULT_LIBRARY}"))
    set(INATECK_SDK_LIBRARY "${INATECK_SDK_DEFAULT_LIBRARY}" CACHE FILEPATH
        "Path to the Inateck Linux shared library" FORCE)
endif()
if(INATECK_SDK_LIBRARY)
    if(NOT EXISTS "${INATECK_SDK_LIBRARY}")
        message(FATAL_ERROR "Inateck SDK library not found: ${INATECK_SDK_LIBRARY}")
    endif()
    find_library(INATECK_XDO_LIBRARY NAMES xdo libxdo.so.3)
    if(NOT INATECK_XDO_LIBRARY)
        message(FATAL_ERROR "The Inateck Linux SDK requires libxdo.so.3. Install libxdo-dev or configure with -DINATECK_SDK_LIBRARY=.")
    endif()
    add_library(InateckScannerBle SHARED IMPORTED)
    # La bibliotheque n'a pas de SONAME : sans IMPORTED_NO_SONAME, l'editeur de liens enregistrerait son
    # chemin absolu de compilation dans l'executable (DT_NEEDED), introuvable une fois installe.
    set_target_properties(InateckScannerBle PROPERTIES
        IMPORTED_LOCATION "${INATECK_SDK_LIBRARY}"
        IMPORTED_NO_SONAME TRUE)
    target_compile_definitions(InateckScannerBle INTERFACE QRPROTEC_HAS_INATECK=1)
endif()

# -----------------------------------------------------------------------------
# 2. Fichiers sources d'ImGui et de ses backends
# -----------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# Graphes (previsions de stock)
set(IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/implot)
set(IMPLOT_SOURCES
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)

# Coeur sans interface (teste unitairement) : modeles d'etiquettes, rendu, QR codes, JSON, API
add_library(QRProtecCore STATIC
    src/core/codes.cpp
    src/core/fonts.cpp
    src/core/glob_utils.cpp
    src/core/json.cpp
    src/core/paths.cpp
    src/core/placeholders.cpp
    src/core/search.cpp
    src/core/template.cpp
    src/core/template_io.cpp
    src/net/connection.cpp
    src/net/http.cpp
    src/net/api_client.cpp
    src/net/remote_scanner_link.cpp
    src/net/websocket.cpp
    src/printer/logging.cpp
    src/render/image_loader.cpp
    src/render/raster.cpp
    third_party/qrcodegen/qrcodegen.cpp
)
target_include_directories(QRProtecCore PUBLIC
    src
    third_party/qrcodegen
    third_party/stb
)
target_link_libraries(QRProtecCore PUBLIC PNG::PNG Freetype::Freetype Threads::Threads OpenSSL::SSL)
# Police avec accents fournie avec les sources (copiee aussi a cote de l'executable)
target_compile_definitions(QRProtecCore PRIVATE
    QRPROTEC_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts"
    # modeles d'etiquettes suivis par git (app/templates)
    QRPROTEC_TEMPLATES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/templates")

add_library(QRProtecPrinter STATIC
    src/printer/niimbot_protocol.cpp
    src/printer/serial.cpp
    src/printer/printer.cpp
)
target_include_directories(QRProtecPrinter PUBLIC src)
target_link_libraries(QRProtecPrinter PUBLIC QRProtecCore)

# Icone de la fenetre (environnement de bureau) : celle du site web (`make icon` a la racine du depot),
# embarquee dans l'executable
set(QRPROTEC_ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../database/inventory/web" CACHE PATH
    "Dossier des icones du site (icon-512.png, icon-192.png, favicon.png)")
set(QRPROTEC_ICON_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/window_icon.hpp")
set(QRPROTEC_ICON_CONTENT "// Genere par CMakeLists.txt depuis ${QRPROTEC_ICON_DIR}\n#pragma once\n")
foreach(icon IN ITEMS icon-192 favicon)
    set(icon_file "${QRPROTEC_ICON_DIR}/${icon}.png")
    if(NOT EXISTS "${icon_file}")
        message(FATAL_ERROR "Icone introuvable : ${icon_file}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${icon_file}")
    file(READ "${icon_file}" icon_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," icon_bytes "${icon_hex}")
    string(MAKE_C_IDENTIFIER "${icon}" icon_symbol)
    string(APPEND QRPROTEC_ICON_CONTENT "static const unsigned char window_${icon_symbol}_png[] = {${icon_bytes}};\n")
endforeach()
# configure_file ne reecrit l'en-tete que s'il change (pas de recompilation a chaque configuration)
file(WRITE "${QRPROTEC_ICON_HEADER}.in" "${QRPROTEC_ICON_CONTENT}")
configure_file("${QRPROTEC_ICON_HEADER}.in" "${QRPROTEC_ICON_HEADER}" COPYONLY)

# -----------------------------------------------------------------------------
# 3. Définition de l'exécutable
# -----------------------------------------------------------------------------
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/app/app.cpp
    src/app/feedback.cpp
    src/app/labels.cpp
    src/app/print_queue.cpp
    src/app/scan_stack.cpp
    src/app/settings.cpp
    src/inateck/hid_classifier.cpp
    src/inateck/inateck_worker.cpp
    src/inateck/sdk_json.cpp
    src/ui/inateck.cpp
    src/ui/editor.cpp
    src/ui/label_preview.cpp
    src/ui/widgets.cpp
    src/ui/windows/inventory_window.cpp
    src/ui/windows/journal_window.cpp
    src/ui/windows/pack_window.cpp
    src/ui/windows/phone_window.cpp
    src/ui/windows/lot_admin_window.cpp
    src/ui/windows/lots_window.cpp
    src/ui/windows/scan_window.cpp
    src/ui/windows/settings_window.cpp
    src/ui/windows/stock_window.cpp
    src/ui/windows/users_window.cpp
    src/ui/windows/verif_window.cpp
    ${IMGUI_SOURCES}
    ${IMPLOT_SOURCES}
)

# Inclusion des dossiers d'en-têtes (headers)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/cpp
    ${IMPLOT_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/scanner_lib/ble
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# Édition de liens avec OpenGL et GLFW
target_link_libraries(${PROJECT_NAME} PRIVATE
    glfw
    OpenGL::GL
    QRProtecCore
    QRProtecPrinter
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts $<TARGET_FILE_DIR:${PROJECT_NAME}>/fonts)

if(TARGET InateckScannerBle)
    target_link_libraries(${PROJECT_NAME} PRIVATE InateckScannerBle ${INATECK_XDO_LIBRARY})
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/scanner_lib/ble/linux/x86_64-unknown-linux-gnu")
endif()

if(BUILD_TESTING)
    add_executable(QRProtecCoreTest tests/core_test.cpp)
    target_link_libraries(QRProtecCoreTest PRIVATE QRProtecCore)
    add_test(NAME QRProtecCoreTest COMMAND QRProtecCoreTest)

    add_executable(QRProtecCodesTest tests/codes_test.cpp)
    target_link_libraries(QRProtecCodesTest PRIVATE QRProtecCore)
    add_test(NAME QRProtecCodesTest COMMAND QRProtecCodesTest)

    add_executable(QRProtecPrinterTest
        tests/printer_test.cpp
        src/printer/niimbot_protocol.cpp
    )
    target_include_directories(QRProtecPrinterTest PRIVATE src)
    add_test(NAME QRProtecPrinterTest COMMAND QRProtecPrinterTest)

    add_executable(QRProtecHidClassifierTest
        tests/hid_classifier_test.cpp
        src/inateck/hid_classifier.cpp
    )
    target_include_directories(QRProtecHidClassifierTest PRIVATE src)
    add_test(NAME QRProtecHidClassifierTest COMMAND QRProtecHidClassifierTest)

    add_executable(QRProtecSdkJsonTest
        tests/sdk_json_test.cpp
        src/inateck/sdk_json.cpp
    )
    target_include_directories(QRProtecSdkJsonTest PRIVATE src)
    add_test(NAME QRProtecSdkJsonTest COMMAND QRProtecSdkJsonTest)

    # Les tests reposent sur assert() (parfois avec effets de bord) : le garder actif en Release
    foreach(test_target QRProtecCoreTest QRProtecCodesTest QRProtecPrinterTest QRProtecHidClassifierTest QRProtecSdkJsonTest)
        target_compile_options(${test_target} PRIVATE -UNDEBUG)
    endforeach()
endif()
