#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "app/app.hpp"
#include "printer/logging.hpp"
#include "ui/inateck.hpp"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

static void glfw_error_callback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static qrprotec::Inateck* active_inateck = nullptr;
static qrprotec::App* active_app = nullptr;

static void note_activity()
{
    if (active_app)
        active_app->note_activity();
}

// Police avec accents francais ; la police par defaut d'ImGui reste utilisee si aucune n'est trouvee.
static void load_font(float size)
{
    const char* font_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf"};
    ImGuiIO& io = ImGui::GetIO();
    for (const char* path : font_paths) {
        std::error_code error;
        if (std::filesystem::exists(path, error) && io.Fonts->AddFontFromFileTTF(path, size)) {
            break;
        }
    }
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
    if (!active_inateck)
        return ImGui_ImplGlfw_CharCallback(window, character);
    for (const unsigned int replay : active_inateck->handle_hid_character(character))
        ImGui_ImplGlfw_CharCallback(window, replay);
}

static void hid_focus_callback(GLFWwindow* window, int focused)
{
    if (active_inateck)
        active_inateck->handle_window_focus(focused != 0);
    ImGui_ImplGlfw_WindowFocusCallback(window, focused);
}

static void imgui_cursor_enter_callback(GLFWwindow* window, int entered)
{
    ImGui_ImplGlfw_CursorEnterCallback(window, entered);
}

static void imgui_cursor_position_callback(GLFWwindow* window, double x, double y)
{
    ImGui_ImplGlfw_CursorPosCallback(window, x, y);
}

static void imgui_mouse_button_callback(GLFWwindow* window, int button, int action, int mods)
{
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
}

static void imgui_scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
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
    GLFWwindow* window = glfwCreateWindow(1280, 800, "QRProtec", nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        return 1;
    }
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
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
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
