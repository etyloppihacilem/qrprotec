cmake_minimum_required(VERSION 3.16)
project(QRProtecApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# 1. Recherche des dépendances système (GLFW et OpenGL)
# -----------------------------------------------------------------------------
find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)

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

# -----------------------------------------------------------------------------
# 3. Définition de l'exécutable
# -----------------------------------------------------------------------------
add_executable(${PROJECT_NAME}
    src/main.cpp
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
)
