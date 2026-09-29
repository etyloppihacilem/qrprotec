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
    add_library(InateckScannerBle UNKNOWN IMPORTED)
    set_target_properties(InateckScannerBle PROPERTIES IMPORTED_LOCATION "${INATECK_SDK_LIBRARY}")
    target_compile_definitions(InateckScannerBle INTERFACE QRPROTEC_HAS_INATECK=1)
endif()

# -----------------------------------------------------------------------------
# 2. Fichiers sources d'ImGui et de ses backends
# -----------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

add_library(QRProtecPrinter STATIC
    src/printer/logging.cpp
    src/printer/niimbot_protocol.cpp
    src/printer/serial.cpp
    src/printer/printer.cpp
)
target_include_directories(QRProtecPrinter PUBLIC src)

# -----------------------------------------------------------------------------
# 3. Définition de l'exécutable
# -----------------------------------------------------------------------------
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/core/glob_utils.cpp
    src/core/template.cpp
    src/core/template_io.cpp
    src/render/raster.cpp
    src/inateck/hid_classifier.cpp
    src/inateck/inateck_worker.cpp
    src/ui/inateck.cpp
    src/ui/editor.cpp
    ${IMGUI_SOURCES}
)

# Inclusion des dossiers d'en-têtes (headers)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/scanner_lib/ble
)

# Édition de liens avec OpenGL et GLFW
target_link_libraries(${PROJECT_NAME} PRIVATE
    glfw
    OpenGL::GL
    PNG::PNG
    Freetype::Freetype
    QRProtecPrinter
)

if(TARGET InateckScannerBle)
    target_link_libraries(${PROJECT_NAME} PRIVATE InateckScannerBle ${INATECK_XDO_LIBRARY})
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/scanner_lib/ble/linux/x86_64-unknown-linux-gnu")
endif()

if(BUILD_TESTING)
    add_executable(QRProtecCoreTest
        tests/core_test.cpp
        src/core/template.cpp
        src/core/template_io.cpp
        src/render/raster.cpp
    )
    target_include_directories(QRProtecCoreTest PRIVATE src)
    target_link_libraries(QRProtecCoreTest PRIVATE PNG::PNG Freetype::Freetype)
    add_test(NAME QRProtecCoreTest COMMAND QRProtecCoreTest)

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
endif()
