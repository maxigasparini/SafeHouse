#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <imgui/ImGuiFileDialog.h>
#include <imgui/imgui.h>
#include <imgui/imgui_impl_glfw.h>
#include <imgui/imgui_impl_opengl3.h>
#include <imgui/imgui_internal.h>

#include <atomic>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include "backend.h"
#include "desktop_shortcut.h"
#include "steam.h"
#include "brand.h"
#include "button_prompts.h"
#include "games.h"
#include "gamepad_mappings.h"
#include "gl_context.h"
#include "glfw_platform.h"
#include "ui/face_buttons.h"
#include "ui/fixtures.h"
#include "ui/game_page.h"
#include "ui/gamepad_nav.h"
#include "ui/install_page.h"
#include "ui/job.h"
#include "ui/osk.h"
#include "ui/settings_page.h"
#include "ui/shell.h"
#include "ui/update_dialog.h"
#include "update.h"
#include "launcher_game_sly1.h"
#include "launcher_game_sly2.h"
#include "launcher_game_sly3.h"
#include "launcher_game_sly4.h"
#include "launcher_game_hackpack.h"
#include "launcher_icon.h"
#include "launcher_logo.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace {
namespace ui = cane::ui;
using cane::GameId;
// Embedded game logos (launcher/assets/games), in GameId order.
struct EmbeddedImage {
    const unsigned char* data;
    size_t size;
};
const EmbeddedImage cane_game_logos[] = {{cane_game_sly1, sizeof(cane_game_sly1)},
                                         {cane_game_sly2, sizeof(cane_game_sly2)},
                                         {cane_game_sly3, sizeof(cane_game_sly3)},
                                         {cane_game_sly4, sizeof(cane_game_sly4)},
                                         {cane_game_hackpack, sizeof(cane_game_hackpack)}};
static_assert(std::size(cane_game_logos) == cane::kGameCount);
std::string fixtureIso(int index) { return cane::utf8(ui::fixtures::isos()[static_cast<size_t>(index)].path); }
struct Texture {
    GLuint id = 0;
    int width = 0, height = 0;
    // Logos are drawn far below their source size, so they get mipmaps.
    Texture(const unsigned char* data, size_t size, const char* what) {
        auto pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, nullptr, 4);
        if (!pixels) throw std::runtime_error(std::string("Cannot load launcher ") + what);
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glGenerateMipmap(GL_TEXTURE_2D);
        stbi_image_free(pixels);
    }
    Texture(const Texture&) = delete;
    ~Texture() {
        if (id) glDeleteTextures(1, &id);
    }
    ImTextureID imgui() const { return reinterpret_cast<ImTextureID>(static_cast<intptr_t>(id)); }
    float aspect() const { return static_cast<float>(width) / static_cast<float>(height); }
};
void windowIcon(GLFWwindow* window) {
    // Wayland delegates window/taskbar icons to the installed desktop entry.
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND || glfwGetPlatform() == GLFW_PLATFORM_COCOA) return;
    GLFWimage icon{};
    icon.pixels = stbi_load_from_memory(cane_icon, sizeof(cane_icon), &icon.width, &icon.height, nullptr, 4);
    if (!icon.pixels) throw std::runtime_error("Cannot load launcher icon");
    glfwSetWindowIcon(window, 1, &icon);
    stbi_image_free(icon.pixels);
}
void screenshot(int w, int h, const cane::fs::path& file) {
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    stbi_flip_vertically_on_write(true);
    if (!stbi_write_png(cane::utf8(file).c_str(), w, h, 4, pixels.data(), w * 4))
        throw std::runtime_error("Could not save screenshot");
}
// Layout of the screenshot frame for scripts/check-launcher-screenshots.py:
// labelled rectangles, the focused item and the prompt family.
void dumpLayout(const cane::fs::path& file, const ImRect& focus, const ui::Shell& shell) {
    auto entries = cane::json::array();
    for (const auto& e : ui::layoutEntries())
        entries.push_back({{"label", e.label}, {"x", e.rect.Min.x}, {"y", e.rect.Min.y}, {"w", e.rect.GetWidth()}, {"h", e.rect.GetHeight()}});
    static constexpr const char* views[] = {"game", "install", "settings", "about"};
    cane::json out{{"entries", entries},
                   {"family", cane::input::familyId(cane::input::tracker().family())},
                   {"atlas", cane::input::promptAtlas().uploaded()},
                   {"game", cane::game(shell.game).key},
                   {"view", views[static_cast<int>(shell.view)]},
                   {"focus", nullptr}};
    if (focus.GetWidth() > 0) out["focus"] = {{"x", focus.Min.x}, {"y", focus.Min.y}, {"w", focus.GetWidth()}, {"h", focus.GetHeight()}};
    cane::writeJson(file, out);
}
// The keyboard/pad focus rectangle, when the focus ring is showing.
ImRect focusRect() {
    ImGuiContext& g = *GImGui;
    if (!g.NavWindow || !g.NavId || g.NavDisableHighlight) return {};
    return ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]);
}
// Offscreen target for fixture screenshots: exact pixel size, no visible window.
struct Offscreen {
    GLuint fbo = 0, color = 0;
    Offscreen(int w, int h) {
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("Cannot create the screenshot framebuffer");
    }
    ~Offscreen() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &color);
    }
};
std::string activityText(const std::string& label, const std::string& stage, float progress) {
    if (label.empty()) return {};
    if (label.rfind("Playing", 0) == 0) return label;
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%s · %s · %.0f%%", label.c_str(), stage.empty() ? "Starting" : ui::stageLabel(stage).c_str(),
                  progress * 100);
    return buffer;
}

// Replays --nav presses: frame 8 + 4i presses token i, the next frame releases it.
// "a" and "b" are confirm and back, so they press the physical button the
// prompt family uses (east and south for Nintendo). Runs between
// FaceButtonSwap::begin() and end(), where ImGui sees physical positions.
void replayNav(const ui::fixtures::Options& options, int frame, cane::launcher::PadEdges& pad) {
    auto& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const bool keyboard = !cane::input::tracker().lastWasGamepad();
    const int step = frame - 8;
    if (step < 0 || step / 4 >= static_cast<int>(options.nav.size()) || step % 4 > 1) return;
    const bool down = step % 4 == 0;
    const std::string& token = options.nav[static_cast<size_t>(step / 4)];
    static const std::pair<const char*, std::pair<ImGuiKey, ImGuiKey>> keys[] = {
        {"up", {ImGuiKey_GamepadDpadUp, ImGuiKey_UpArrow}},       {"down", {ImGuiKey_GamepadDpadDown, ImGuiKey_DownArrow}},
        {"left", {ImGuiKey_GamepadDpadLeft, ImGuiKey_LeftArrow}}, {"right", {ImGuiKey_GamepadDpadRight, ImGuiKey_RightArrow}},
        {"a", {ImGuiKey_GamepadFaceDown, ImGuiKey_Enter}}};
    const bool swapped = ui::eastConfirms(ui::faceButtonFamily());
    for (const auto& [name, key] : keys)
        if (token == name)
            io.AddKeyEvent(keyboard ? key.second : swapped && token == "a" ? ImGuiKey_GamepadFaceRight : key.first, down);
    // Back is the button ImGui cancels with; GamepadNav also reports east as
    // PadEdges::back.
    if (token == "b" && !keyboard) io.AddKeyEvent(swapped ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight, down);
    if (token == "b" && keyboard) io.AddKeyEvent(ImGuiKey_Escape, down);
    if (!down) return;
    if (token == "b" && !keyboard && !swapped) pad.back = true;
    if (token == "x") pad.x = true;
    if (token == "y") pad.y = true;
    if (token == "lb") pad.lb = true;
    if (token == "rb") pad.rb = true;
    if (token == "lt") pad.lt = true;
    if (token == "rt") pad.rt = true;
    if (token == "start") pad.start = true;
    if (token == "select") pad.select = true;
}
// The drop target and router: main() installs one per window.
std::function<void(std::string)>& dropHandler() {
    static std::function<void(std::string)> handler;
    return handler;
}

// The update the launcher finishes after its window closed (launcher/update.h).
struct PendingUpdate {
    std::optional<cane::update::Plan> plan;
    bool restart = false;
};
PendingUpdate& pendingUpdate() {
    static PendingUpdate pending;
    return pending;
}
// Windows: the helper swaps the files once this process ended. Linux / macOS: the files are swapped here (running
// programs keep their old files open), then the new launcher starts.
void finishUpdate(const cane::LauncherBackend& backend) {
    auto& pending = pendingUpdate();
    if (!pending.plan) return;
    const auto& plan = *pending.plan;
#ifdef _WIN32
    (void)backend;
    try {
        cane::update::launchHelper(plan, pending.restart);
    } catch (const std::exception& e) {
        MessageBoxW(nullptr, cane::path(std::string("SafeHouse could not be updated: ") + e.what()).c_str(), L"SafeHouse", MB_OK | MB_ICONWARNING);
    }
#else
    std::ofstream log(plan.staging / "update.log", std::ios::app);
    std::string message;
    bool ok = true;
    try {
        cane::update::apply(plan, [&](const std::string& line) { log << line << std::endl; });
    } catch (const std::exception& e) {
        ok = false;
        message = e.what();
        log << message << std::endl;
    }
    cane::update::writeResult(backend.locations.config, plan, ok, message);
    if (pending.restart) cane::update::startLauncher(plan);
#endif
    pending.plan.reset();
}

int desktop(cane::LauncherBackend& backend, const ui::fixtures::Options* fixture, const cane::fs::path& screenshotPath) {
    glfwSetErrorCallback([](int code, const char* message) { std::cerr << "GLFW " << code << ": " << message << '\n'; });
    if (!cane::initGlfw()) throw std::runtime_error("Cannot initialize desktop display");
    cane::input::installJoystickCallback();
    cane::input::applyBundledMappings();
    cane::input::tracker().forceFromEnv();
    cane::gl::ApplyNativeWindowHints(3, 3);
    glfwWindowHintString(GLFW_X11_CLASS_NAME, cane::brand::wmClass);
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, cane::brand::appId);
    if (fixture) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    const int width0 = fixture ? fixture->width : 1280, height0 = fixture ? fixture->height : 800;
    auto window = glfwCreateWindow(width0, height0, cane::brand::windowTitle, nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        throw std::runtime_error("Cannot create launcher window");
    }
    glfwSetWindowSizeLimits(window, 960, 600, GLFW_DONT_CARE, GLFW_DONT_CARE);
    windowIcon(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(fixture ? 0 : 1);
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
        throw std::runtime_error("Cannot initialize launcher graphics");
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    // Ctrl+Tab window switching means nothing in a single-window launcher.
    GImGui->ConfigNavWindowingKeyNext = GImGui->ConfigNavWindowingKeyPrev = 0;
    // The interface scale: the "Launcher size" setting times a factor for big
    // windows (1440p and 4K screens), in quarter steps.
    float userScale = std::clamp(backend.config.value("ui_scale", 1.0f), 1.0f, 1.5f);
    if (fixture) userScale = 1.0f;
    const auto windowScale = [](int w, int h) {
        const float fit = std::min(static_cast<float>(w) / 1280.0f, static_cast<float>(h) / 800.0f);
        return std::clamp(std::floor(fit * 4.0f) / 4.0f, 1.0f, 2.0f);
    };
    float scale = std::clamp(userScale * windowScale(width0, height0), 1.0f, 3.0f);
    ui::loadFonts(scale);
    ui::applyStyle(scale);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    cane::launcher::GamepadNav nav;
    nav.init(window);
    ui::FaceButtonSwap faceSwap;
    cane::input::promptAtlas().upload();
    Texture logo(cane_logo, sizeof(cane_logo), "logo");
    Texture icon(cane_icon, sizeof(cane_icon), "icon");
    ui::images().logo = logo.imgui();
    ui::images().logoAspect = logo.aspect();
    ui::images().icon = icon.imgui();
    std::vector<std::unique_ptr<Texture>> gameLogos;
    for (int i = 0; i < cane::kGameCount; ++i) {
        gameLogos.push_back(std::make_unique<Texture>(cane_game_logos[i].data, cane_game_logos[i].size, "game logo"));
        ui::images().games[i] = {gameLogos.back()->imgui(), gameLogos.back()->aspect()};
    }

    // Declared before the job so its worker is joined before these go away.
    ui::SessionLog sessionLog(backend.locations.logs);
    std::atomic<bool> logging{backend.loggingEnabled()};
    ui::Job job;
    job.sessionLog = &sessionLog;
    job.logging = &logging;
    ui::Shell shell;
    shell.game = fixture && fixture->game ? *fixture->game : backend.lastGame();
    ui::GamePage page(backend, job);
    ui::InstallPage install(backend, job);
    ui::SettingsPage settings(backend, job, window);
    ui::UpdateUi updates(backend);
    settings.updates = &updates;
    bool showLogs = backend.config.value("show_logs", false);
    float pendingScale = 0;
    settings.showLogs = &showLogs;
    settings.setScale = [&](float next) { pendingScale = next; };
    settings.logging = [&] { return logging.load(); };
    settings.setLogging = [&](bool enabled) {
        logging = enabled;
        try {
            backend.setLoggingEnabled(enabled);
        } catch (const std::exception& e) {
            shell.toast(e.what(), false);
        }
    };
    auto openInstall = [&](GameId game, std::string iso = {}, bool addRegion = false) {
        if (game != shell.game || shell.view != ui::View::Game) shell.selectGame(game);
        install.open(game, std::move(iso), addRegion);
        shell.open(ui::View::Install);
    };
    page.openInstall = [&](GameId game) { openInstall(game); };
    // "Re-extract" with the remembered disc image: the install page, already installing.
    page.reextract = [&](GameId game, std::string iso) {
        openInstall(game, std::move(iso));
        install.installNow();
    };
    settings.installRegion = [&](GameId game) { openInstall(game, {}, true); };
    page.addRegion = [&](GameId game) { openInstall(game, {}, true); };
    install.onBack = [&] {
        shell.open(ui::View::Game);
    };
    install.onRoute = [&](GameId game, std::string iso) { openInstall(game, std::move(iso)); };
    install.onStarted = [&](GameId game) { page.installingGame = static_cast<int>(game); };
    // Drag and drop: the image goes to the page of the game it is.
    dropHandler() = [&](std::string file) {
        if (job.busy) {
            shell.toast("Wait for the current task to finish first.", false);
            return;
        }
        // (the install page shows the region and any problem with the image)
        const auto check = cane::checkDisc(cane::path(file));
        if (check.game && cane::installable(*check.game) && check.status != cane::DiscCheck::Status::NotSupportedYet) {
            openInstall(*check.game, file);
            return;
        }
        shell.toast(check.message.empty() ? "That disc image cannot be installed." : check.message, false);
    };
    glfwSetDropCallback(window, [](GLFWwindow*, int count, const char** paths) {
        if (count && dropHandler()) dropHandler()(paths[0]);
    });

    std::unique_ptr<Offscreen> offscreen;
    if (fixture && fixture->live) {
        // --live: the screens read the (private, XDG) profile as it is; only the screen and the nav are scripted.
        offscreen = std::make_unique<Offscreen>(fixture->width, fixture->height);
        const auto& s = fixture->screen;
        if (s == "install") openInstall(shell.game, fixture->iso);
        else if (s == "install-region") openInstall(shell.game, fixture->iso, true);
        else if (s == "reextract") page.showReextract(shell.game);
        else if (s == "desktop-shortcut") page.showDesktopShortcut(shell.game, false);
        else if (s.rfind("settings", 0) == 0) {
            shell.open(ui::View::Settings);
            if (auto colon = s.find(':'); colon != std::string::npos) settings.selectCategory(s.substr(colon + 1));
        }
        ui::layoutRecording() = !fixture->layoutPath.empty();
    } else if (fixture) {
        offscreen = std::make_unique<Offscreen>(fixture->width, fixture->height);
        std::array<ui::GameStatus, cane::kGameCount> statuses{};
        statuses[0] = {fixture->installed, true, false, fixture->installed ? fixtureIso(0) : std::string{}};
        statuses[1] = {fixture->sly2Installed, fixture->sly2Runtime, false, fixture->sly2Installed ? fixtureIso(1) : std::string{}};
        page.useFixture(statuses);
        install.useFixture(shell.game == GameId::Sly1 ? fixture->installed : fixture->sly2Installed);
        settings.useFixture();
        const auto& s = fixture->screen;
        if (s == "install") openInstall(shell.game);
        else if (s == "install-progress") {
            // A fake installation in progress (no worker thread).
            openInstall(shell.game);
            install.showProgressFixture();
            page.installingGame = static_cast<int>(shell.game);
            job.busy = true;
            job.label = "Installation";
            const bool sly2 = shell.game == GameId::Sly2;
            job.stage = sly2 ? "convert" : "levels";
            job.item = sly2 ? "Music/29 Wine Cellar.vpk" : "Mz. Ruby's Swamp";
            job.progress = 0.42f;
            for (const char* line : {"Checking your disc", sly2 ? "Disc verified: Sly 2: Band of Thieves (USA)"
                                                                : "Disc verified: Sly Cooper and the Thievius Raccoonus (USA)",
                                     sly2 ? "Converting the game files (812 of 1903)" : "Preparing worlds (14 of 33)"})
                job.lines.push_back(line);
        } else if (s == "progress-page") {
            page.installingGame = static_cast<int>(shell.game);
            job.busy = true;
            job.label = "Installation";
            job.stage = "copy";
            job.progress = 0.42f;
        } else if (s == "about") shell.open(ui::View::About);
        else if (s == "sly3-notice") page.showSly3Notice();
        else if (s == "desktop-shortcut") page.showDesktopShortcut(shell.game, false);
        else if (s == "desktop-shortcut-done") page.showDesktopShortcut(shell.game, true);
        else if (s.rfind("settings", 0) == 0) {
            shell.open(ui::View::Settings);
            if (auto colon = s.find(':'); colon != std::string::npos) settings.selectCategory(s.substr(colon + 1));
        }
        ui::layoutRecording() = !fixture->layoutPath.empty();
    } else if (backend.config.value("start_fullscreen", ui::SettingsPage::defaultFullscreen())) {
        if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
            if (const GLFWvidmode* mode = glfwGetVideoMode(monitor))
                glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    }
    if (fixture && fixture->screen.rfind("update", 0) == 0) updates.fixture(fixture->screen);
    else if (!fixture) updates.startup(shell);
    ui::requestFocus();

    int frames = 0;
    bool blockedLastFrame = false, wasBusy = false, windowHidden = false;
    std::string lastLabel;
    while (!glfwWindowShouldClose(window)) {
        // While a game runs the launcher redraws a few times a second instead
        // of competing with it for the CPU.
        const bool gameRunning = job.busy && page.playingGame >= 0;
        if (!fixture && (gameRunning || glfwGetWindowAttrib(window, GLFW_ICONIFIED))) glfwWaitEventsTimeout(0.25);
        else glfwPollEvents();
        // The window is away while a game runs and comes back when it quits.
        if (!fixture && !windowHidden && job.busy && page.playingGame >= 0) {
            glfwHideWindow(window);
            windowHidden = true;
        }
        if (windowHidden) {
            job.reap();
            updates.poll(shell, true);
            if (job.busy) continue;
            glfwShowWindow(window);
            glfwFocusWindow(window);
            windowHidden = false;
        }
        if (pendingScale > 0) userScale = pendingScale, pendingScale = 0;
        {
            int ww = width0, wh = height0;
            if (!fixture) glfwGetWindowSize(window, &ww, &wh);
            const float wanted = ww > 0 && wh > 0 ? std::clamp(userScale * windowScale(ww, wh), 1.0f, 3.0f) : scale;
            if (std::fabs(wanted - scale) > 0.01f) scale = wanted, pendingScale = -1;
        }
        if (pendingScale < 0) {
            pendingScale = 0;
            ui::loadFonts(scale);
            ui::applyStyle(scale);
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            ImGui_ImplOpenGL3_CreateFontsTexture();
        }
        const bool focused = fixture || glfwGetWindowAttrib(window, GLFW_FOCUSED);
        const bool iconified = glfwGetWindowAttrib(window, GLFW_ICONIFIED);
        // Prompts, and which face button confirms, follow the prompt family.
        const auto family = cane::input::promptFamily(settings.promptStyle(), 0);
        ui::faceButtonFamily() = family;
        cane::launcher::setPromptPreferences(settings.promptStyle(), 0);
        // ImGui's own cancel (the back button) leaves the nav-flattened content
        // child or clears the focus. The launcher handles back itself, so the
        // focus is put back below unless a screen moves it.
        ImGuiContext& g = *GImGui;
        const ImGuiID navBefore = g.ActiveId == 0 && g.OpenPopupStack.Size == 0 && g.NavLayer == ImGuiNavLayer_Main ? g.NavId : 0;
        ImGuiWindow* const navWindowBefore = g.NavWindow;
        ImGui_ImplOpenGL3_NewFrame();
        faceSwap.begin(family);
        nav.beforeBackendNewFrame();
        ImGui_ImplGlfw_NewFrame();
        auto pad = nav.afterBackendNewFrame(focused && !iconified && !settings.capturingPad());
        if (fixture) {
            io.DisplaySize = {static_cast<float>(fixture->width), static_cast<float>(fixture->height)};
            io.DisplayFramebufferScale = {1, 1};
            replayNav(*fixture, frames, pad);
        }
        faceSwap.end();
        ImGui::NewFrame();
        // Back: the button ImGui cancels with (south for Nintendo). Under a
        // swap PadEdges::back is east, which confirms there.
        const bool padBack = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || (pad.back && !ui::eastConfirms(family));
        const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if ((padBack || escape) && navBefore && navWindowBefore && g.NavId != navBefore)
            ui::setFocus(navBefore, navWindowBefore, !cane::input::tracker().lastWasMouse());
        ui::layoutEntries().clear();
        ui::padScrollAmount() = pad.scrollY;
        job.reap();
        updates.poll(shell);

        // A finished job: report it where the player is.
        const bool busy = job.busy.load();
        if (busy) lastLabel = job.label;
        if (wasBusy && !busy && !fixture) {
            std::string message;
            {
                std::lock_guard lock(job.mutex);
                message = job.message;
            }
            const int installed = page.installingGame;
            page.invalidate();
            install.invalidate();
            if (installed >= 0 && message == "Completed") {
                const GameId id = static_cast<GameId>(installed);
                shell.toast(std::string(cane::game(id).title) + " is installed.");
                if (shell.view == ui::View::Install && shell.game == id) shell.open(ui::View::Game);
            } else if (message == "Completed" && lastLabel == "Verification")
                shell.toast("All game files are intact.");
            else if (message == "Completed" && lastLabel == "Uninstall")
                shell.toast("Game files removed. Your saves are kept.");
            else if (message == "Completed" && lastLabel.rfind("PS3 HD import", 0) == 0)
                shell.toast("PS3 HD textures installed. They show the next time the game starts.");
            else if (message != "Completed" && message != "Cancelled" && !(shell.view == ui::View::Install && installed >= 0))
                shell.toast(message, false);
            page.installingGame = page.playingGame = -1;
        }
        wasBusy = busy;
        // A downloaded update: the launcher closes and comes back as the new version once no game or task runs.
        if (!fixture && updates.restartNow(busy || gameRunning)) glfwSetWindowShouldClose(window, true);

        // Global controls (pad buttons ImGui does not use, and Escape).
        // Ignored while a text field, popup, dialog or capture owns input.
        const bool inPage = !shell.railFocused();
        if (settings.capturing()) {
            // A binding capture only listens for its cancel: B for a keyboard
            // binding (a controller binding may bind B itself) or Escape.
            if ((padBack && !settings.capturingPad()) || escape) settings.back();
        } else if (!blockedLastFrame) {
            if (shell.view == ui::View::Game || shell.view == ui::View::Install) {
                if (pad.lb) shell.cycleGame(-1);
                if (pad.rb) shell.cycleGame(+1);
            }
            if (shell.view == ui::View::Settings && (pad.lt || pad.rt)) settings.selectCategory(settings.category + (pad.rt ? 1 : -1));
            if (pad.select) showLogs = !showLogs;
            if (padBack || escape) {
                if (showLogs) showLogs = false;
                else if (shell.view == ui::View::Install) {
                    if (!install.back()) shell.open(ui::View::Game);
                } else if (shell.view == ui::View::Settings && settings.back()) {
                } else if (inPage)
                    shell.focusRail();
            }
            if (pad.start) {
                if (shell.view == ui::View::Game) page.primaryAction(shell.game);
                else if (shell.view == ui::View::Install) install.primaryAction(false);
            }
            if (pad.x && shell.view == ui::View::Install) install.actionX();
        }
        if (shell.changed) {
            if (shell.view == ui::View::Settings) settings.open(shell.game);
            if (shell.view == ui::View::Install && install.game() != shell.game) install.open(shell.game);
            if (!fixture && shell.game != backend.lastGame()) try {
                    backend.setLastGame(shell.game);
                } catch (...) {
                }
            shell.changed = false;
        }

        int width, height;
        if (fixture) width = fixture->width, height = fixture->height;
        else glfwGetWindowSize(window, &width, &height);
        const ImVec2 size{static_cast<float>(width), static_cast<float>(height)};
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin(cane::brand::appName, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar();
        // Page first (it may change the selection), then the rail over it.
        const float railW = ui::Shell::railWidth(size.x);
        const ImVec2 pageSize{size.x - railW, size.y - ui::footerHeight()};
        ImGui::SetCursorPos({railW, 0});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::vec(ui::color::bg));
        if (shell.view == ui::View::Game) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
            ImGui::BeginChild("page", pageSize, ImGuiChildFlags_None,
                              ImGuiWindowFlags_NavFlattened | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            page.draw(shell.game, pageSize);
        } else {
            // Other pages: a centred column at most 1120 px wide.
            const float gutter = std::round(std::clamp(pageSize.x * 0.04f, ui::px(24), ui::px(48)));
            const float column = std::min(pageSize.x - gutter * 2, ui::px(1120));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {std::floor((pageSize.x - column) * 0.5f), ui::px(28)});
            ImGui::BeginChild("page", pageSize, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NavFlattened);
            ImGui::PopStyleVar();
            {
                auto* dl = ImGui::GetWindowDrawList();
                const ImVec2 o = ImGui::GetWindowPos();
                dl->AddRectFilledMultiColor(o, {o.x + pageSize.x, o.y + ui::px(260)}, ui::u32(ui::color::bgTop), ui::u32(ui::color::bgTop),
                                            ui::u32(ui::color::bg), ui::u32(ui::color::bg));
            }
            switch (shell.view) {
            case ui::View::Install: install.draw(false); break;
            case ui::View::Settings: settings.draw(gameRunning || busy); break;
            case ui::View::About: ui::aboutPage(); break;
            default: break;
            }
        }
        ui::padScrollHere();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        shell.rail(size);
        // Log drawer and footer.
        std::string stage, label;
        float progress = 0;
        std::deque<std::string> lines;
        {
            std::lock_guard lock(job.mutex);
            stage = job.stage;
            label = job.label;
            progress = job.progress;
            if (showLogs) lines = job.lines;
        }
        shell.drawer(size, &showLogs, lines, busy);
        using cane::input::Button;
        std::vector<cane::launcher::Hint> hints;
        if (shell.railFocused() && !settings.capturing())
            hints = {{Button::Dpad, 0, "Choose"}, {Button::Back, 0, "Log"}, {ui::confirmButton(), GLFW_KEY_ENTER, "Open"}};
        else {
            switch (shell.view) {
            case ui::View::Game: hints = page.hints(shell.game); break;
            case ui::View::Install:
                hints = {{Button::LB, 0, ""}, {Button::RB, 0, "Games"}};
                for (const auto& h : install.hints()) hints.push_back(h);
                break;
            case ui::View::Settings: hints = settings.hints(); break;
            case ui::View::About: hints = {{ui::backButton(), GLFW_KEY_ESCAPE, "Back"}, {ui::confirmButton(), GLFW_KEY_ENTER, "Select"}}; break;
            }
        }
        const std::string captureStatus = shell.view == ui::View::Settings ? settings.captureStatus() : std::string{};
        shell.footer(size, hints, family,
                     !captureStatus.empty() ? captureStatus : busy ? activityText(label, stage, progress) : std::string{},
                     captureStatus.empty() && busy ? progress : -1.0f);
        ImGui::End();
        install.drawDialogs();
        settings.drawDialogs();
        page.drawDialogs();
        updates.drawDialog(busy || gameRunning);
        cane::launcher::drawOnScreenKeyboard();
        shell.toasts(size);
        // Nothing on the page took the focus (an "In development" page has no
        // controls): give it to the rail.
        if (ui::focusRequested() && ui::focusRequestFrame() >= 0 && g.FrameCount > ui::focusRequestFrame() + 2) {
            ui::focusRequested() = false;
            shell.focusRail();
        } else if (ui::focusRequested() && ui::focusRequestFrame() < 0)
            ui::focusRequestFrame() = g.FrameCount;
        // A capture belongs to the Settings page: leaving it ends it, so keys
        // are never bound unseen and the pad is never left ignored.
        if (shell.view != ui::View::Settings || iconified) settings.cancelCapture();
        const ImRect focus = fixture && !fixture->layoutPath.empty() ? focusRect() : ImRect{};
        blockedLastFrame = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || ImGui::IsAnyItemActive() ||
                           cane::launcher::onScreenKeyboardOpen() || settings.capturing();

        ImGui::Render();
        if (offscreen) glBindFramebuffer(GL_FRAMEBUFFER, offscreen->fbo);
        int fw = width, fh = height;
        if (!fixture) glfwGetFramebufferSize(window, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(ui::vec(ui::color::bg).x, ui::vec(ui::color::bg).y, ui::vec(ui::color::bg).z, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (fixture && ++frames >= 10 + 4 * static_cast<int>(fixture->nav.size()) && glfwGetTime() >= fixture->delay) {
            screenshot(fw, fh, screenshotPath);
            if (!fixture->layoutPath.empty()) dumpLayout(cane::path(fixture->layoutPath), focus, shell);
            glfwSetWindowShouldClose(window, true);
        }
        if (!fixture) glfwSwapBuffers(window);
    }
    dropHandler() = nullptr;
    if (!fixture)
        if (auto plan = updates.pendingPlan()) pendingUpdate() = {plan, updates.restartAfterApply()};
    offscreen.reset();
    if (fixture) job.busy = false;
    job.cancel = true;
    if (job.worker.joinable()) job.worker.join();
    gameLogos.clear();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
// Hidden screenshot options: --screenshot PATH [--screen NAME] [--game KEY]
// [--size WxH] [--installed-fixture] [--sly2-installed-fixture]
// [--sly2-runtime-fixture] [--dump-layout JSON] [--nav a,down,…].
bool screenshotOptions(const std::vector<std::string>& args, ui::fixtures::Options& options) {
    for (size_t i = 2; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--screen" && i + 1 < args.size()) options.screen = args[++i];
        else if (a == "--game" && i + 1 < args.size()) {
            auto id = cane::gameFromKey(args[++i]);
            if (!id) return false;
            options.game = *id;
        } else if (a == "--size" && i + 1 < args.size()) {
            if (std::sscanf(args[++i].c_str(), "%dx%d", &options.width, &options.height) != 2 || options.width < 320 ||
                options.height < 240 || options.width > 7680 || options.height > 4320)
                return false;
        } else if (a == "--installed-fixture") options.installed = true;
        else if (a == "--sly2-installed-fixture") options.sly2Installed = true;
        else if (a == "--sly2-runtime-fixture") options.sly2Runtime = true;
        else if (a == "--dump-layout" && i + 1 < args.size()) options.layoutPath = args[++i];
        else if (a == "--live") options.live = true;
        else if (a == "--iso" && i + 1 < args.size()) options.iso = args[++i];
        else if (a == "--delay" && i + 1 < args.size()) options.delay = std::atof(args[++i].c_str());
        else if (a == "--nav" && i + 1 < args.size()) {
            std::stringstream list(args[++i]);
            for (std::string token; std::getline(list, token, ',');)
                if (!token.empty()) options.nav.push_back(token);
        } else return false;
    }
    return true;
}
// --play/--verify [GAME] [--region KEY]: Sly 1 when no game is named; the selected region when none is.
std::optional<GameId> gameArgument(std::vector<std::string> args, std::string* region = nullptr) {
    if (args.size() >= 3 && args[args.size() - 2] == "--region") {
        if (region) *region = args.back();
        args.resize(args.size() - 2);
    }
    if (args.size() == 1) return GameId::Sly1;
    if (args.size() == 2) return cane::gameFromKey(args[1]);
    return std::nullopt;
}
}  // namespace
int main(int argc, char** argv) {
    bool interactive = true;
    try {
        {
            const auto early = cane::arguments(argc, argv);
            // --version: the release this build is (empty for development builds).
            if (early.size() == 1 && early[0] == "--version") {
                std::cout << cane::brand::appName << " " << cane::update::describe(cane::update::current()) << "\n";
                return 0;
            }
            // --apply-update PLAN [--wait-pid PID] [--restart]: the file swap of an update (the Windows helper; tests).
            // Before the backend: it must not create profile folders next to the helper.
            if (early.size() >= 2 && early[0] == "--apply-update") return cane::update::applyMain(early);
        }
        cane::LauncherBackend backend;
        std::atomic<bool> cancel{false};
        auto log = [](const std::string& s) { std::cout << s << std::endl; };
        auto args = cane::arguments(argc, argv);
        interactive = args.empty();
        if (!args.empty()) {
            if (args[0] == "--install" && args.size() == 2) {
                // The disc decides the game (Sly 1 for anything unrecognised,
                // which the extractor then rejects).
                const auto identity = cane::identifyDiscImage(cane::path(args[1]));
                const GameId game = identity.game && cane::installable(*identity.game) ? *identity.game : GameId::Sly1;
                backend.install(args[1], game, cancel, log);
                return 0;
            }
            std::string region;
            if (args[0] == "--verify")
                if (auto game = gameArgument(args, &region)) {
                    const auto in = region.empty() ? std::nullopt : backend.installation(*game, region);
                    if (!region.empty() && !in) throw std::runtime_error("No " + cane::regionName(region) + " installation");
                    cane::verifyInstallation(in ? in->dir : backend.dataDir(*game), {}, [&] { return cancel.load(); });
                    std::cout << "Installation verified\n";
                    return 0;
                }
            if (args[0] == "--play")
                if (auto game = gameArgument(args, &region)) {
                    backend.play(*game, cancel, log, region);
                    return 0;
                }
            // --installs: every installed region (the selected one marked with *), for scripts and tests.
            if (args[0] == "--installs" && args.size() == 1) {
                for (auto game : {GameId::Sly1, GameId::Sly2, GameId::Sly3}) {
                    const auto selected = backend.selectedRegion(game);
                    for (const auto& in : backend.installations(game))
                        std::cout << cane::game(game).key << " " << in.region << (in.region == selected ? " * " : "   ") << in.name() << " "
                                  << (in.current ? "ready" : in.outdated ? "outdated" : "damaged") << " " << cane::utf8(in.dir) << "\n";
                }
                return 0;
            }
            // --add-to-steam GAME: the game page's "Add to Steam" without the window (SAFEHOUSE_STEAM_ROOT picks a
            // Steam folder, for tests). --steam-status: what the launcher sees of Steam.
            if (args[0] == "--add-to-steam" && args.size() == 2)
                if (auto game = cane::gameFromKey(args[1])) {
                    const auto steam = cane::steam::find();
                    if (!steam) throw std::runtime_error("Steam was not found on this computer");
                    if (cane::steam::running(*steam))
                        std::cout << "Steam is running: restart it to see the shortcut\n";
                    const auto r = cane::steam::add(*steam, *game);
                    const auto sc = cane::steam::shortcutFor(*game);
                    std::cout << "Steam: " << cane::utf8(steam->root) << "\nshortcut: " << sc.name << " app id " << sc.appId
                              << " exe " << sc.exe << " options " << sc.launchOptions << "\naccounts " << r.accounts << ", added "
                              << r.addedEntries << ", updated " << r.updatedEntries << ", images " << r.images << "\n";
                    for (const auto& b : r.backups) std::cout << "backup: " << cane::utf8(b) << "\n";
                    return 0;
                }
            if (args[0] == "--steam-status" && args.size() == 1) {
                const auto steam = cane::steam::find();
                if (!steam) {
                    std::cout << "Steam: not found\n";
                    return 0;
                }
                std::cout << "Steam: " << cane::utf8(steam->root) << (steam->flatpak ? " (Flatpak)" : "") << ", "
                          << steam->accounts.size() << " account(s), " << (cane::steam::running(*steam) ? "running" : "not running") << "\n";
                for (auto game : {GameId::Sly1, GameId::Sly2, GameId::Sly3})
                    std::cout << cane::game(game).key << ": " << (cane::steam::added(*steam, game) ? "in Steam" : "not in Steam") << "\n";
                return 0;
            }
            // --add-desktop-shortcut GAME [--menu]: the game page's "Add desktop shortcut" (--menu: also the applications /
            // Start menu). --desktop-shortcut-status: where the shortcuts go and what is there.
            if (args[0] == "--add-desktop-shortcut" && (args.size() == 2 || (args.size() == 3 && args[2] == "--menu")))
                if (auto game = cane::gameFromKey(args[1])) {
                    std::vector<cane::shortcut::Where> places{cane::shortcut::Where::Desktop};
                    if (args.size() == 3) places.push_back(cane::shortcut::Where::Menu);
                    for (auto where : places) {
                        const auto r = cane::shortcut::create(*game, where);
                        std::cout << cane::shortcut::whereName(where) << ": " << cane::utf8(r.file)
                                  << (!r.existed ? " (created)" : r.changed ? " (updated)" : " (up to date)")
#ifndef _WIN32
                                  << (where == cane::shortcut::Where::Desktop ? (r.trusted ? " trusted" : " not marked trusted (no gio)") : "")
#endif
                                  << "\n";
                    }
                    return 0;
                }
            if (args[0] == "--desktop-shortcut-status" && args.size() == 1) {
                for (auto game : {GameId::Sly1, GameId::Sly2})
                    for (auto where : {cane::shortcut::Where::Desktop, cane::shortcut::Where::Menu}) {
                        const auto file = cane::shortcut::file(game, where);
                        const bool there = cane::shortcut::exists(game, where);
                        std::cout << cane::game(game).key << " " << cane::shortcut::whereName(where) << ": " << cane::utf8(file)
                                  << (there ? "" : " (none)") << "\n";
#ifdef _WIN32
                        if (there) {
                            const auto l = cane::shortcut::readLink(file);
                            std::cout << "  target " << l.target << "\n  arguments " << l.arguments << "\n  directory "
                                      << l.workingDirectory << "\n  icon " << l.iconFile << "," << l.iconIndex << "\n";
                        }
#endif
                    }
                return 0;
            }
            // --pick-file / --pick-folder [FOLDER]: the Browse buttons' system dialog (native_dialog.h), blocking; prints the
            // backend and the result (tests; SAFEHOUSE_FILE_DIALOG picks the backend).
            if ((args[0] == "--pick-file" || args[0] == "--pick-folder") && args.size() <= 2) {
                namespace fd = cane::filedialog;
                const bool file = args[0] == "--pick-file";
                const auto r = fd::pickBlocking(file ? fd::Kind::OpenIso : fd::Kind::PickFolder,
                                                file ? "Choose your disc image" : "Choose a folder",
                                                args.size() == 2 ? cane::path(args[1]) : fd::startFolder({}));
                const char* names[] = {"idle", "running", "chosen", "cancelled", "unavailable", "failed"};
                std::cout << "backend " << (r.backend.empty() ? "none" : r.backend) << "\nstatus " << names[static_cast<int>(r.status)] << "\n";
                if (!r.path.empty()) std::cout << "path " << r.path << "\n";
                if (!r.error.empty()) std::cout << "error " << r.error << "\n";
                return r.status == fd::Status::Chosen ? 0 : 2;
            }
            // --import-ps3-hd GAME SOURCE [png]: Settings > Games > "Import PS3 HD textures" without the window (SOURCE: the
            // extracted Sly Collection, its .iso or a .zip of it; png: as PNG files). --ps3-hd GAME [on|off|remove]: the
            // pack's state.
            if (args[0] == "--import-ps3-hd" && (args.size() == 3 || (args.size() == 4 && args[3] == "png")))
                if (auto game = cane::gameFromKey(args[1])) {
                    backend.importPs3Hd(*game, args[2], args.size() == 4, cancel, log);
                    const auto pack = backend.ps3HdPack(*game);
                    std::cout << "PS3 HD textures installed: " << pack.textures << " textures, " << pack.megabytes << " MB\n";
                    return 0;
                }
            if (args[0] == "--ps3-hd" && (args.size() == 2 || args.size() == 3))
                if (auto game = cane::gameFromKey(args[1])) {
                    if (args.size() == 3 && args[2] == "on") backend.setPs3HdEnabled(*game, true);
                    else if (args.size() == 3 && args[2] == "off") backend.setPs3HdEnabled(*game, false);
                    else if (args.size() == 3 && args[2] == "remove") backend.removePs3Hd(*game);
                    else if (args.size() == 3) throw std::runtime_error("--ps3-hd GAME [on|off|remove]");
                    const auto pack = backend.ps3HdPack(*game);
                    std::cout << cane::game(*game).key << " PS3 HD textures: "
                              << (!pack.installed ? "not installed" : pack.enabled ? "on" : "off");
                    if (pack.installed) std::cout << ", " << pack.textures << " textures, " << pack.megabytes << " MB, from " << pack.source;
                    std::cout << "\n";
                    for (const auto& a : backend.ps3HdArguments(*game)) std::cout << "  play argument: " << a << "\n";
                    return 0;
                }
#ifdef CANE_LAUNCHER_SOURCE_BUILD
            if (args[0] == "--build-source" && args.size() == 2) {
                backend.buildSource(cane::path(args[1]), cancel, log);
                return 0;
            }
#endif
            // --check-update: what the update check finds (SAFEHOUSE_UPDATE_URL points it at a test server).
            // --update: the same, then Update now without the window (no restart; Windows: the helper finishes the
            // swap after this program exited).
            if ((args[0] == "--check-update" || args[0] == "--update") && args.size() == 1) {
                namespace up = cane::update;
                const auto build = up::current();
                const auto install = up::detectInstall();
                std::cout << "current " << up::describe(build) << "\ninstall " << (install.kind == up::Kind::Unsupported ? "unsupported" : up::kindKey(install.kind))
                          << (install.target.empty() ? "" : " " + cane::utf8(install.target)) << "\n";
                if (!install.reason.empty()) std::cout << "reason " << install.reason << "\n";
                const auto release = up::fetchLatest();
                const bool newer = up::compareVersions(release.version, build.version) > 0;
                std::cout << "latest " << release.version << " " << release.tag << " " << release.date << "\n"
                          << (newer ? "newer" : "up to date") << ", " << up::daysBehind(build.date, release.date) << " days\n";
                const auto package = up::packageFor(release, install);
                std::cout << "package " << (package ? package->file + " " + package->sha256 : std::string("none")) << "\n";
                if (args[0] == "--check-update" || !newer) return 0;
                if (!package) throw std::runtime_error(install.reason.empty() ? "No package for this installation" : install.reason);
                uint64_t shown = 0;
                const auto plan = up::stage(release, install, [&](uint64_t done, uint64_t total) {
                    if (done != shown && (done - shown >= (16u << 20) || done == total)) std::cout << "downloaded " << done << " of " << total << std::endl, shown = done;
                }, &cancel, log);
#ifdef _WIN32
                up::launchHelper(plan, false);
                std::cout << "staged; the helper finishes the update after this program exits\n";
                return 0;
#else
                std::ofstream updateLog(plan.staging / "update.log", std::ios::app);
                try {
                    up::apply(plan, [&](const std::string& line) {
                        updateLog << line << std::endl;
                        log(line);
                    });
                } catch (const std::exception& e) {
                    up::writeResult(backend.locations.config, plan, false, e.what());
                    throw;
                }
                up::writeResult(backend.locations.config, plan, true, {});
                std::cout << "updated to " << plan.version << "\n";
                return 0;
#endif
            }
            ui::fixtures::Options options;
            if (args[0] == "--screenshot" && args.size() >= 2 && screenshotOptions(args, options))
                return desktop(backend, &options, cane::path(args[1]));
            std::cout << "safehouse [--install ISO | --verify [GAME] [--region R] | --play [GAME] [--region R] | --installs | --add-to-steam GAME |\n"
                         "           --steam-status | --add-desktop-shortcut GAME [--menu] | --desktop-shortcut-status |\n"
#ifdef CANE_LAUNCHER_SOURCE_BUILD
                         "           --build-source CHECKOUT |\n"
#endif
                         "           --pick-file [FOLDER] | --pick-folder [FOLDER] |\n"
                         "           --import-ps3-hd GAME SOURCE [png] | --ps3-hd GAME [on|off|remove] | --version | --check-update | --update]\n"
                         "GAME: sly1 (default), sly2 or sly3. R: usa, europe, japan, korea (default: the region selected in the launcher)\n";
            return args[0] == "--help" ? 0 : 1;
        }
        const int code = desktop(backend, nullptr, {});
        finishUpdate(backend);
        return code;
    } catch (const std::exception& e) {
        std::cerr << cane::brand::launcherDescription << ": " << e.what() << std::endl;
#ifdef _WIN32
        if (interactive)
            MessageBoxW(nullptr, cane::path(e.what()).c_str(), cane::path(cane::brand::launcherDescription).c_str(), MB_OK | MB_ICONERROR);
#endif
        return 1;
    }
}

#if defined(_WIN32) && defined(CANE_GUI_SUBSYSTEM)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(0, nullptr); }
#endif
