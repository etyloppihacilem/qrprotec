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
    src/ui/editor.cpp
    ${IMGUI_SOURCES}
)

# Inclusion des dossiers d'en-têtes (headers)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# Édition de liens avec OpenGL et GLFW
target_link_libraries(${PROJECT_NAME} PRIVATE
    glfw
    OpenGL::GL
    PNG::PNG
    Freetype::Freetype
    QRProtecPrinter
)

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
endif()
