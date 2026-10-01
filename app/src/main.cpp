#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "app/app.hpp"
#include "core/fonts.hpp"
#include "printer/logging.hpp"
#include "ui/inateck.hpp"

#include <GLFW/glfw3.h>
#include "stb_image.h"
#include "window_icon.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <memory>
#include <string>
#include <utility>

// Icone de la fenetre sous X11 (barre des taches, Alt+Tab). Sous Wayland, le bureau prend l'icone du
// fichier qrprotec.desktop designe par l'app_id de la fenetre (GLFW_WAYLAND_APP_ID).
static void set_window_icon(GLFWwindow* window)
{
#ifdef GLFW_PLATFORM_WAYLAND
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
        return;  // non pris en charge : GLFW afficherait une erreur
#endif
    const std::pair<const unsigned char*, int> sources[] = {
        { window_icon_192_png, static_cast<int>(sizeof(window_icon_192_png)) },
        { window_favicon_png, static_cast<int>(sizeof(window_favicon_png)) },
    };
    GLFWimage images[2];
    int count = 0;
    for (const auto& [data, size] : sources) {
        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char* pixels = stbi_load_from_memory(data, size, &width, &height, &channels, 4);
        if (pixels)
            images[count++] = GLFWimage{ width, height, pixels };
    }
    if (count > 0)
        glfwSetWindowIcon(window, count, images);
    for (int index = 0; index < count; ++index)
        stbi_image_free(images[index].pixels);
}

static void glfw_error_callback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static qrprotec::Inateck* active_inateck = nullptr;
static qrprotec::App* active_app = nullptr;

// Rendu a la demande : sur un ecran virtuel (kiosk) glfwSwapInterval ne limite rien et la boucle tournait
// a 100 % d'un coeur. On plafonne a ~60 images/s apres une entree, puis on attend les evenements en
// redessinant ~10 fois par seconde (scans du SDK, notifications, minuteries) quand rien ne se passe.
using frame_clock = std::chrono::steady_clock;
static constexpr auto active_frame_period = std::chrono::microseconds(1000000 / 60);
static constexpr double idle_wait_seconds = 0.1;
static constexpr auto active_duration = std::chrono::seconds(2);
static frame_clock::time_point last_input_time{};

static void note_input()
{
    last_input_time = frame_clock::now();
}

static void note_activity()
{
    note_input();
    if (active_app)
        active_app->note_activity();
}

// Police avec accents francais ; la police par defaut d'ImGui reste utilisee si aucune n'est trouvee.
static void load_font(float size)
{
    ImGuiIO& io = ImGui::GetIO();
    const std::string& path = qrprotec::ui_font_path();
    if (path.empty() || !io.Fonts->AddFontFromFileTTF(path.c_str(), size))
        std::fprintf(stderr, "Aucune police avec accents trouvee (installez DejaVu Sans ou definissez QRPROTEC_FONT)\n");
    else
        qrprotec::debug_log("Police : " + path);
    ImGui::GetStyle().FontSizeBase = size;
}

static void hid_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    note_activity();
    if (!active_inateck)
        return ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
    const qrprotec::Inateck::HidKeyResult result = active_inateck->handle_hid_key(key, action);
    for (const unsigned int character : result.replay)
        ImGui_ImplGlfw_CharCallback(window, character);
    if (!result.consume)
        ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
}

static void hid_char_callback(GLFWwindow* window, unsigned int character)
{
    note_input();
    if (!active_inateck)
        return ImGui_ImplGlfw_CharCallback(window, character);
    for (const unsigned int replay : active_inateck->handle_hid_character(character))
        ImGui_ImplGlfw_CharCallback(window, replay);
}

static void hid_focus_callback(GLFWwindow* window, int focused)
{
    note_input();
    if (active_inateck)
        active_inateck->handle_window_focus(focused != 0);
    ImGui_ImplGlfw_WindowFocusCallback(window, focused);
}

static void imgui_cursor_enter_callback(GLFWwindow* window, int entered)
{
    note_input();
    ImGui_ImplGlfw_CursorEnterCallback(window, entered);
}

static void imgui_cursor_position_callback(GLFWwindow* window, double x, double y)
{
    note_input();
    ImGui_ImplGlfw_CursorPosCallback(window, x, y);
}

static void imgui_mouse_button_callback(GLFWwindow* window, int button, int action, int mods)
{
    note_input();
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
}

static void imgui_scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    note_input();
    ImGui_ImplGlfw_ScrollCallback(window, xoffset, yoffset);
}

int main(int argc, char** argv)
{
    bool verbose = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--verbose" || std::string(argv[index]) == "-v") {
            verbose = true;
        } else {
            std::fprintf(stderr, "Usage: %s [--verbose]\n", argv[0]);
            return 2;
        }
    }
    qrprotec::set_verbose_logging(verbose);
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    // identifiant de l'application : le bureau y associe qrprotec.desktop (nom, icone)
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "qrprotec");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "qrprotec");
#ifdef GLFW_WAYLAND_APP_ID
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "qrprotec");
#endif
    GLFWwindow* window = glfwCreateWindow(1280, 800, "QRProtec", nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        return 1;
    }
    set_window_icon(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // la disposition est geree par l'application (disposition par defaut restauree apres inactivite)
    io.IniFilename = nullptr;
    ImGui::StyleColorsLight();
    ImGui::GetStyle().FrameRounding = 3.0f;

    qrprotec::Inateck inateck;
    active_inateck = &inateck;
    glfwSetKeyCallback(window, hid_key_callback);
    glfwSetCharCallback(window, hid_char_callback);
    glfwSetWindowFocusCallback(window, hid_focus_callback);
    ImGui_ImplGlfw_InitForOpenGL(window, false);
    glfwSetCursorEnterCallback(window, imgui_cursor_enter_callback);
    glfwSetCursorPosCallback(window, imgui_cursor_position_callback);
    glfwSetMouseButtonCallback(window, imgui_mouse_button_callback);
    glfwSetScrollCallback(window, imgui_scroll_callback);
    ImGui_ImplOpenGL3_Init(nullptr);

    // detruite avant le contexte OpenGL (textures de l'editeur)
    auto app = std::make_unique<qrprotec::App>(inateck);
    active_app = app.get();
    load_font(app->settings.font_size);
    glfwSetWindowSizeCallback(window, [](GLFWwindow*, int, int) { note_input(); });
    glfwSetWindowRefreshCallback(window, [](GLFWwindow*) { note_input(); });
    frame_clock::time_point frame_start = frame_clock::now();
    while (!glfwWindowShouldClose(window)) {
        inateck.begin_poll();
        if (frame_clock::now() - last_input_time < active_duration) {
            // actif : plafond d'images/s meme sans vsync
            std::this_thread::sleep_until(frame_start + active_frame_period);
            glfwPollEvents();
        } else {
            // inactif : endormi jusqu'a la prochaine entree, ou au plus idle_wait_seconds
            glfwWaitEventsTimeout(idle_wait_seconds);
        }
        frame_start = frame_clock::now();
        for (const unsigned int character : inateck.flush_hid_characters())
            ImGui_ImplGlfw_CharCallback(window, character);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app->begin_frame();
        app->draw();

        ImGui::Render();

        int display_width = 0;
        int display_height = 0;
        glfwGetFramebufferSize(window, &display_width, &display_height);
        glViewport(0, 0, display_width, display_height);
        // fond orange en mode privilegie
        const ImVec4 background = app->background_color();
        glClearColor(background.x, background.y, background.z, background.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    active_app = nullptr;
    app.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    active_inateck = nullptr;
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
