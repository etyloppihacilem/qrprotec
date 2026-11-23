cmake_minimum_required(VERSION 3.16)
project(QRProtecApp LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Windows (front seul, zip portable) : sources en UTF-8 (textes francais), M_PI, pas de macros min/max de windows.h,
# runtime C++ lie statiquement (vcpkg x64-windows-static : un seul .exe, plus la DLL Inateck)
if(WIN32)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        add_compile_options(/utf-8 /bigobj)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

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

# macOS : la bibliotheque Inateck existe en dylib (scanner_lib/ble/mac), sans libxdo ; Windows : DLL MSVC
# (scanner_lib/ble/windows) avec sa bibliotheque d'import
if(WIN32)
    set(INATECK_SDK_DEFAULT_LIBRARY
        "${CMAKE_CURRENT_SOURCE_DIR}/scanner_lib/ble/windows/x86_64-pc-windows-msvc/inateck_scanner_ble.dll")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(INATECK_SDK_TRIPLE "mac/aarch64-apple-darwin")
    else()
        set(INATECK_SDK_TRIPLE "mac/x86_64-apple-darwin")
    endif()
    set(INATECK_SDK_DEFAULT_LIBRARY
        "${CMAKE_CURRENT_SOURCE_DIR}/scanner_lib/ble/${INATECK_SDK_TRIPLE}/libinateck_scanner_ble.dylib")
else()
    set(INATECK_SDK_DEFAULT_LIBRARY
        "${CMAKE_CURRENT_SOURCE_DIR}/scanner_lib/ble/linux/x86_64-unknown-linux-gnu/libinateck_scanner_ble.so")
endif()
if(NOT DEFINED INATECK_SDK_LIBRARY OR
   (INATECK_SDK_LIBRARY STREQUAL "" AND EXISTS "${INATECK_SDK_DEFAULT_LIBRARY}"))
    set(INATECK_SDK_LIBRARY "${INATECK_SDK_DEFAULT_LIBRARY}" CACHE FILEPATH
        "Path to the Inateck shared library" FORCE)
endif()
if(INATECK_SDK_LIBRARY)
    if(NOT EXISTS "${INATECK_SDK_LIBRARY}")
        message(FATAL_ERROR "Inateck SDK library not found: ${INATECK_SDK_LIBRARY}")
    endif()
    add_library(InateckScannerBle SHARED IMPORTED)
    if(APPLE)
        # Le dylib fourni porte comme identifiant un chemin absolu de la machine d'Inateck : copie
        # renommee en @rpath/... (puis re-signee, obligatoire sur arm64) pour la trouver a cote de
        # l'executable.
        set(INATECK_SDK_COPY "${CMAKE_CURRENT_BINARY_DIR}/libinateck_scanner_ble.dylib")
        if("${INATECK_SDK_LIBRARY}" IS_NEWER_THAN "${INATECK_SDK_COPY}")
            file(COPY_FILE "${INATECK_SDK_LIBRARY}" "${INATECK_SDK_COPY}")
            execute_process(COMMAND install_name_tool -id @rpath/libinateck_scanner_ble.dylib "${INATECK_SDK_COPY}"
                            COMMAND_ERROR_IS_FATAL ANY)
            execute_process(COMMAND codesign --force --sign - "${INATECK_SDK_COPY}" COMMAND_ERROR_IS_FATAL ANY)
        endif()
        set_target_properties(InateckScannerBle PROPERTIES IMPORTED_LOCATION "${INATECK_SDK_COPY}")
    elseif(WIN32)
        set_target_properties(InateckScannerBle PROPERTIES
            IMPORTED_LOCATION "${INATECK_SDK_LIBRARY}"
            IMPORTED_IMPLIB "${INATECK_SDK_LIBRARY}.lib")
    else()
        find_library(INATECK_XDO_LIBRARY NAMES xdo libxdo.so.3)
        if(NOT INATECK_XDO_LIBRARY)
            message(FATAL_ERROR "The Inateck Linux SDK requires libxdo.so.3. Install libxdo-dev or configure with -DINATECK_SDK_LIBRARY=.")
        endif()
        # La bibliotheque n'a pas de SONAME : sans IMPORTED_NO_SONAME, l'editeur de liens enregistrerait son
        # chemin absolu de compilation dans l'executable (DT_NEEDED), introuvable une fois installe.
        set_target_properties(InateckScannerBle PROPERTIES
            IMPORTED_LOCATION "${INATECK_SDK_LIBRARY}"
            IMPORTED_NO_SONAME TRUE)
    endif()
    target_compile_definitions(InateckScannerBle INTERFACE QRPROTEC_HAS_INATECK=1)
endif()

# -----------------------------------------------------------------------------
# 2. Fichiers sources d'ImGui et de ses backends
# -----------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)

# ImPlot et ImGui en bibliotheques a part, declarees avant les autres cibles et sans dependance entre
# cibles : sous make -j comme sous Ninja, leurs objets (les plus longs a compiler) partent des le debut de
# la construction, en parallele du reste, au lieu de finir seuls en queue.
#
# Graphes (previsions de stock). implot_items.cpp instancie chaque fonction Plot* pour tous les types
# numeriques (10 par defaut) : plus de deux minutes de compilation en Release. Le poste ne trace que des
# double : seuls float et double sont instancies (un autre type donnerait une erreur d'edition de liens,
# a ajouter ici).
set(IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/implot)
add_library(QRProtecImPlot STATIC
    ${IMPLOT_DIR}/implot_items.cpp
    ${IMPLOT_DIR}/implot.cpp
)
# en-tetes d'ImGui seulement, sans target_link_libraries(QRProtecImGui) : sous make, une dependance entre
# cibles ferait attendre la fin d'ImGui avant de commencer implot_items.cpp
target_include_directories(QRProtecImPlot PUBLIC ${IMPLOT_DIR} PRIVATE ${IMGUI_DIR})
target_compile_definitions(QRProtecImPlot PUBLIC "IMPLOT_CUSTOM_NUMERIC_TYPES=(float)(double)")

# ImGui, juste apres ImPlot, pour la meme raison. Sources de la plus longue a la plus courte.
add_library(QRProtecImGui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(QRProtecImGui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/cpp
)
target_link_libraries(QRProtecImGui PUBLIC glfw OpenGL::GL)

if(APPLE)
    # OpenGL est deprecie (mais toujours present) sur macOS
    target_compile_definitions(QRProtecImGui PUBLIC GL_SILENCE_DEPRECATION)
endif()

# Coeur sans interface (teste unitairement) : modeles d'etiquettes, rendu, QR codes, JSON, API
add_library(QRProtecCore STATIC
    src/core/code128.cpp
    src/core/codes.cpp
    src/core/fonts.cpp
    src/core/glob_utils.cpp
    src/core/hotspot_state.cpp
    src/core/json.cpp
    src/core/paths.cpp
    src/core/placeholders.cpp
    src/core/search.cpp
    src/core/system.cpp
    src/core/template.cpp
    src/core/template_io.cpp
    src/core/update_check.cpp
    src/core/verif_cache.cpp
    src/core/verif_plan.cpp
    src/core/print_counter.cpp
    src/net/connection.cpp
    src/net/front_config.cpp
    src/net/http.cpp
    src/net/api_client.cpp
    src/net/websocket_link.cpp
    src/net/websocket.cpp
    src/printer/logging.cpp
    src/render/image_loader.cpp
    src/render/label_types.cpp
    src/render/raster.cpp
    third_party/qrcodegen/qrcodegen.cpp
)
target_include_directories(QRProtecCore PUBLIC
    src
    third_party/qrcodegen
    third_party/stb
)
target_link_libraries(QRProtecCore PUBLIC PNG::PNG Freetype::Freetype Threads::Threads OpenSSL::SSL)
if(WIN32)
    # Winsock (HTTP, WebSocket) ; crypt32 : magasin de certificats de Windows (OpenSSL winstore)
    target_link_libraries(QRProtecCore PUBLIC ws2_32 crypt32)
endif()
# Police avec accents fournie avec les sources (copiee aussi a cote de l'executable)
target_compile_definitions(QRProtecCore PRIVATE
    QRPROTEC_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts"
    # modeles d'etiquettes suivis par git (app/templates)
    QRPROTEC_TEMPLATES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/templates")

add_library(QRProtecPrinter STATIC
    src/printer/niimbot_protocol.cpp
    src/printer/label_languages.cpp
    src/printer/raw_transport.cpp
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
    src/app/app_cascade.cpp
    src/app/app_data.cpp
    src/app/app_offline.cpp
    src/app/app_phone.cpp
    src/app/app_reports.cpp
    src/app/app_scans.cpp
    src/app/app_session.cpp
    src/app/app_setup.cpp
    src/app/app_verifs.cpp
    src/app/app_windows.cpp
    src/app/hub.cpp
    src/app/feedback.cpp
    src/app/label_prints.cpp
    src/app/poste_actions.cpp
    src/app/labels.cpp
    src/app/print_queue.cpp
    src/app/roll_change.cpp
    src/app/scan_stack.cpp
    src/app/settings.cpp
    src/app/updater.cpp
    src/inateck/hid_classifier.cpp
    src/inateck/inateck_worker.cpp
    src/inateck/scanner_settings.cpp
    src/inateck/sdk_json.cpp
    src/ui/inateck.cpp
    src/ui/editor.cpp
    src/ui/label_preview.cpp
    src/ui/widgets.cpp
    src/ui/file_picker.cpp
    src/ui/windows/inventory_window.cpp
    src/ui/windows/journal_window.cpp
    src/ui/windows/pack_window.cpp
    src/ui/windows/phone_window.cpp
    src/ui/windows/reports_window.cpp
    src/ui/windows/print_counts_window.cpp
    src/ui/windows/lists_window.cpp
    src/ui/windows/lot_admin_window.cpp
    src/ui/windows/lots_window.cpp
    src/ui/windows/scan_window.cpp
    src/ui/windows/settings_window.cpp
    src/ui/windows/stock_window.cpp
    src/ui/windows/lot_types_window.cpp
    src/ui/windows/users_window.cpp
    src/ui/windows/verif_window.cpp
)
# Wifi de secours de la borne : seulement sous Linux (kiosk Fedora, paquet qrprotec-kiosk)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(${PROJECT_NAME} PRIVATE src/app/hotspot.cpp src/app/app_hotspot.cpp)
    target_compile_definitions(${PROJECT_NAME} PRIVATE QRPROTEC_HOTSPOT=1)
endif()

if(WIN32)
    # application fenetree (pas de console) qui garde main() ; icone et manifeste (UTF-8) dans les ressources
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME QRProtec)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/windows/qrprotec.rc)
    # bip (PlaySound), boite « Ouvrir » (GetOpenFileNameW)
    target_link_libraries(${PROJECT_NAME} PRIVATE winmm comdlg32)
    if(MSVC)
        # manifeste du .rc seulement (sinon doublon avec celui de l'editeur de liens)
        target_link_options(${PROJECT_NAME} PRIVATE /ENTRY:mainCRTStartup /MANIFEST:NO)
    endif()
    # modeles fournis a cote de l'executable : copies au premier lancement dans %APPDATA%\QRProtec\templates
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/templates $<TARGET_FILE_DIR:${PROJECT_NAME}>/templates)
endif()

# Inclusion des dossiers d'en-têtes (headers)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/scanner_lib/ble
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# Édition de liens avec OpenGL et GLFW
target_link_libraries(${PROJECT_NAME} PRIVATE
    QRProtecImPlot
    QRProtecImGui
    QRProtecCore
    QRProtecPrinter
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts $<TARGET_FILE_DIR:${PROJECT_NAME}>/fonts)

# Mise a jour automatique (macOS) : version de cette construction (tag sans le v, passee par la CI sur un tag
# vX.Y.Z) et fichier de la release GitHub a telecharger. Sans version (developpement), pas de mise a jour.
set(QRPROTEC_VERSION "" CACHE STRING "Version de l'application (tag sans le v)")
set(QRPROTEC_UPDATE_REPO "etyloppihacilem/qrprotec" CACHE STRING "Depot GitHub des releases (mises a jour macOS)")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(QRPROTEC_UPDATE_ASSET_DEFAULT "QRProtec-macos-arm64.dmg")
else()
    set(QRPROTEC_UPDATE_ASSET_DEFAULT "QRProtec-macos-x86_64.dmg")
endif()
set(QRPROTEC_UPDATE_ASSET "${QRPROTEC_UPDATE_ASSET_DEFAULT}" CACHE STRING "Fichier de la release (mises a jour macOS)")
# sur ce seul fichier : changer de version ne recompile pas tout
set_source_files_properties(src/app/updater.cpp PROPERTIES COMPILE_DEFINITIONS
    "QRPROTEC_VERSION=\"${QRPROTEC_VERSION}\";QRPROTEC_UPDATE_REPO=\"${QRPROTEC_UPDATE_REPO}\";QRPROTEC_UPDATE_ASSET=\"${QRPROTEC_UPDATE_ASSET}\"")

if(TARGET InateckScannerBle)
    if(WIN32)
        target_link_libraries(${PROJECT_NAME} PRIVATE InateckScannerBle)
        # la DLL est copiee a cote de l'executable
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${INATECK_SDK_LIBRARY} $<TARGET_FILE_DIR:${PROJECT_NAME}>)
    elseif(APPLE)
        target_link_libraries(${PROJECT_NAME} PRIVATE InateckScannerBle)
        # le dylib est copie a cote de l'executable
        set_target_properties(${PROJECT_NAME} PROPERTIES BUILD_RPATH "@executable_path")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${INATECK_SDK_COPY} $<TARGET_FILE_DIR:${PROJECT_NAME}>)
    else()
        target_link_libraries(${PROJECT_NAME} PRIVATE InateckScannerBle ${INATECK_XDO_LIBRARY})
        set_target_properties(${PROJECT_NAME} PROPERTIES
            BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/scanner_lib/ble/linux/x86_64-unknown-linux-gnu")
    endif()
endif()

if(BUILD_TESTING)
    add_executable(QRProtecCoreTest tests/core_test.cpp)
    target_link_libraries(QRProtecCoreTest PRIVATE QRProtecCore)
    add_test(NAME QRProtecCoreTest COMMAND QRProtecCoreTest)

    add_executable(QRProtecCodesTest tests/codes_test.cpp)
    target_link_libraries(QRProtecCodesTest PRIVATE QRProtecCore)
    # modeles fournis (ex : item_codebarre.qr) : valides et lisibles
    target_compile_definitions(QRProtecCodesTest PRIVATE QRPROTEC_TEMPLATES_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/templates")
    add_test(NAME QRProtecCodesTest COMMAND QRProtecCodesTest)

    # scenarios de vérif communs au serveur, au front web et au poste (tests/verif-scenarios/*.json)
    add_executable(QRProtecVerifScenariosTest tests/verif_scenarios_test.cpp)
    target_link_libraries(QRProtecVerifScenariosTest PRIVATE QRProtecCore)
    target_compile_definitions(QRProtecVerifScenariosTest PRIVATE
        QRPROTEC_SCENARIOS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tests/verif-scenarios")
    add_test(NAME QRProtecVerifScenariosTest COMMAND QRProtecVerifScenariosTest)

    add_executable(QRProtecPrinterTest
        tests/printer_test.cpp
        src/printer/niimbot_protocol.cpp
        src/printer/label_languages.cpp
        src/printer/raw_transport.cpp
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
        src/inateck/scanner_settings.cpp
        src/inateck/sdk_json.cpp
    )
    target_include_directories(QRProtecSdkJsonTest PRIVATE src)
    add_test(NAME QRProtecSdkJsonTest COMMAND QRProtecSdkJsonTest)

    # Les tests reposent sur assert() (parfois avec effets de bord) : le garder actif en Release
    foreach(test_target QRProtecCoreTest QRProtecCodesTest QRProtecVerifScenariosTest QRProtecPrinterTest QRProtecHidClassifierTest QRProtecSdkJsonTest)
        target_compile_options(${test_target} PRIVATE -UNDEBUG)
    endforeach()
endif()
