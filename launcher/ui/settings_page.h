#pragma once
// The Settings page: a game's Saves/system_settings.dat (the game's format,
// mirrored in platform/system_settings.h; Sly 1's in the profile, Sly 2's in
// <profile>/sly2, the --user-dir each runtime is given) plus the launcher's own
// preferences in launcher.json. A row at the top picks the game; Sly 2
// (Sly2-Release) reads the display, graphics, controls and volume options, so
// only those categories are shown for it. Only options the game itself has are
// shown. Each game has its own defaults (Sly 2: its keyboard / mouse layout,
// settings::sly2Defaults()). Every change is validated with sanitizeStatic() and saved at
// once with an atomic write. The file is re-read whenever the game rewrites
// it, and it is never written while the game runs or when a newer build
// wrote it.
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "../backend.h"
#include "../native_dialog.h"
#include "brand.h"
#include "controller_family.h"
#include "sly2_action_bindings.h"
#include "job.h"
#include "prompt_bar.h"
#include "shell.h"
#include "system_settings.h"
#include "update_dialog.h"
#include "widgets.h"

namespace cane::ui {
// GLFW key name as the game prints it, with friendlier names for the
// non-printing keys.
inline std::string keyName(int key) {
    switch (key) {
    case GLFW_KEY_UNKNOWN: return "Unbound";
    case GLFW_KEY_ESCAPE: return "Esc";
    case GLFW_KEY_SPACE: return "Space";
    case GLFW_KEY_ENTER: return "Enter";
    case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_BACKSPACE: return "Backspace";
    case GLFW_KEY_INSERT: return "Insert";
    case GLFW_KEY_DELETE: return "Delete";
    case GLFW_KEY_HOME: return "Home";
    case GLFW_KEY_END: return "End";
    case GLFW_KEY_PAGE_UP: return "Page Up";
    case GLFW_KEY_PAGE_DOWN: return "Page Down";
    case GLFW_KEY_CAPS_LOCK: return "Caps Lock";
    case GLFW_KEY_LEFT_SHIFT: return "Left Shift";
    case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
    case GLFW_KEY_LEFT_CONTROL: return "Left Ctrl";
    case GLFW_KEY_RIGHT_CONTROL: return "Right Ctrl";
    case GLFW_KEY_LEFT_ALT: return "Left Alt";
    case GLFW_KEY_RIGHT_ALT: return "Right Alt";
    case GLFW_KEY_LEFT_SUPER: return "Left Super";
    case GLFW_KEY_RIGHT_SUPER: return "Right Super";
    case GLFW_KEY_UP: return "Up Arrow";
    case GLFW_KEY_DOWN: return "Down Arrow";
    case GLFW_KEY_LEFT: return "Left Arrow";
    case GLFW_KEY_RIGHT: return "Right Arrow";
    case GLFW_KEY_KP_ENTER: return "Keypad Enter";
    default: break;
    }
    if (settings::mouseBinding(key)) {
        static constexpr const char* mouse[] = { "Left Mouse", "Right Mouse", "Middle Mouse", "Mouse 4", "Mouse 5",
                                                 "Mouse 6", "Mouse 7", "Mouse 8", "Wheel Up", "Wheel Down" };
        return mouse[key - settings::kMouseBindingBase];
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25) return "F" + std::to_string(key - GLFW_KEY_F1 + 1);
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) return "Keypad " + std::to_string(key - GLFW_KEY_KP_0);
    if (const char* name = glfwGetKeyName(key, 0)) return upper(name);
    if (key >= 32 && key < 127) return std::string(1, static_cast<char>(key));
    return "Key " + std::to_string(key);
}

class SettingsPage {
    static constexpr int kAdvancedCapture = 1000;
public:
    enum Category { Display, Graphics, Audio, Controls, Interface, Launcher, Games, Advanced, CategoryCount };
    static constexpr std::array<const char*, CategoryCount> names{ "Display", "Graphics", "Audio", "Controls",
                                                                    "Interface", "Launcher", "Games", "Advanced" };
    static constexpr std::array<const char*, CategoryCount> ids{ "display", "graphics", "audio", "controls",
                                                                  "interface", "launcher", "games", "advanced" };
    static constexpr std::array<const char*, CategoryCount> blurbs{
        "How the game window opens and how fast it runs.",
        "Rendering quality. Higher settings need a faster GPU.",
        "Speaker layout and volume levels.",
        "Button prompts, mouse, and key and button bindings.",
        "The in-game menus and HUD.",
        "This launcher's own preferences.",
        "Installed regions of each game. Regions install side by side and share their saves.",
#ifdef CANE_LAUNCHER_SOURCE_BUILD
        "Troubleshooting, logs and custom runtimes.",
#else
        "Troubleshooting and logs.",
#endif
    };
    // Launcher preferences the page edits on behalf of main.cpp.
    std::function<void(float)> setScale;
    std::function<void(bool)> setLogging; // "Save logs"
    std::function<bool()> logging;
    // "Install another region" of a game (main.cpp opens its install page in that mode).
    std::function<void(GameId)> installRegion;
    bool* showLogs = nullptr;
    // Settings > Launcher's update rows (main.cpp's UpdateUi).
    UpdateUi* updates = nullptr;
    int category = Display;

    SettingsPage(LauncherBackend& backend, Job& job, GLFWwindow* window) : m_backend(backend), m_job(job), m_window(window) {
        GLint samples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &samples);
        m_maxSamples = samples;
        active() = this;
        previousKeyCallback() = glfwSetKeyCallback(window, keyCallback);
        load();
    }
    ~SettingsPage() {
        if (active() == this) active() = nullptr;
    }
    // Shared values the rest of the launcher reads.
    const settings::SystemSettings& values() const { return m_settings; }
    // The launcher's button prompt style (launcher.json "prompt_style").
    int promptStyle() const { return std::clamp(m_backend.config.value("prompt_style", 0), 0, input::kPromptStyleLast); }
    bool capturingPad() const { return m_captureAction >= 0 && m_captureController; }
    bool capturing() const { return m_captureAction >= 0; }
    void useFixture() { m_fixture = true; }
    // Screenshots: installed regions to show in Games.
    void fixturePs3Pack(GameId game, LauncherBackend::Ps3HdPack pack) { m_ps3Packs[static_cast<size_t>(game)] = std::move(pack); }
    void fixtureRegions(GameId game, std::vector<Installation> installs, std::string selected) {
        m_regionsFixture = true;
        m_installs[static_cast<size_t>(game)] = std::move(installs);
        m_selected[static_cast<size_t>(game)] = std::move(selected);
    }
    // The tab was opened: show what is on disk now (for the selected game,
    // when it has settings; otherwise the page keeps the game it shows).
    void open() {
        load();
        m_focusRail = true;
    }
    void open(GameId game) {
        if (hasSettings(game)) m_game = game;
        if (!shown(category)) category = Display;
        open();
    }
    GameId game() const { return m_game; }
    // The games whose settings this page edits.
    static bool hasSettings(GameId game) { return game == GameId::Sly1 || game == GameId::Sly2; }
    // Whether a category applies to the shown game.
    bool shown(int c) const {
        if (m_game == GameId::Sly1) return true;
        return c == Display || c == Graphics || c == Audio || c == Controls || c == Launcher || c == Games || c == Advanced;
    }
    void selectCategory(int next) {
        cancelCapture();
        const int step = next < category ? -1 : 1;
        int c = (next % CategoryCount + CategoryCount) % CategoryCount;
        for (int i = 0; i < CategoryCount && !shown(c); ++i) c = ((c + step) % CategoryCount + CategoryCount) % CategoryCount;
        category = c;
        m_scrollTop = true;
    }
    void selectCategory(const std::string& id) {
        for (int i = 0; i < CategoryCount; ++i)
            if (id == ids[static_cast<size_t>(i)]) category = i;
    }
    // Ends a binding capture without changing anything. main.cpp calls this
    // whenever the Settings tab is not shown, so a capture never outlives it.
    void cancelCapture() { m_captureAction = -1; }
    // Footer status while a controller binding is captured: every button can
    // be bound, so none of them can cancel; the capture times out instead.
    std::string captureStatus() const {
        if (!capturingPad()) return {};
        const int left = std::max(1, static_cast<int>(std::ceil(kPadCaptureSeconds - (ImGui::GetTime() - m_captureStarted))));
        return "Press the button for " + std::string(captureName()) + " · cancels in " +
               std::to_string(left) + " s, or press Esc";
    }
    // B (keyboard capture only) or Esc: cancel a capture, then return from
    // the pane to the rail.
    bool back() {
        if (capturing()) {
            cancelCapture();
            return true;
        }
        if (m_paneFocused) {
            m_focusRail = true;
            return true;
        }
        return false;
    }
    std::vector<launcher::Hint> hints() const {
        using input::Button;
        if (capturingPad()) return {};
        if (capturing()) return { { backButton(), GLFW_KEY_ESCAPE, "Cancel" } };
        std::vector<launcher::Hint> h{ { Button::LT, 0, "" }, { Button::RT, 0, "Category" } };
        if (m_paneFocused) h.push_back({ Button::DpadLeft, 0, "Change" });
        h.push_back({ backButton(), GLFW_KEY_ESCAPE, "Back" });
        h.push_back({ confirmButton(), GLFW_KEY_ENTER, "Select" });
        return h;
    }

    void draw(bool gameRunning) {
        poll();
        if (gameRunning) cancelCapture();
        pollPadCapture();
        pollMouseCapture();
        if (capturing() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancelCapture();
        m_help.clear();
        // Title row with the save state on the right.
        const ImVec2 titlePos = ImGui::GetCursorPos();
        const std::string subtitle = std::string("Game settings apply to ") + cane::game(m_game).title + ". Changes are saved at once.";
        titleBar("Settings", false, subtitle.c_str());
        saveIndicator(titlePos);
        const ImVec2 area = ImGui::GetContentRegionAvail();
        const float railW = std::min(px(220), std::floor(area.x * 0.24f));
        rail(railW, area.y);
        ImGui::SameLine(0, px(18));
        pane(area.x - railW - px(24), area.y, gameRunning);
        m_focusRail = false;
    }
    void drawDialogs() {
        if (auto r = m_locationDialog.poll()) {
            if (r->status == filedialog::Status::Chosen) applyLocation(r->path);
            else if (r->status == filedialog::Status::Unavailable) openLocationPicker();
            else if (r->status == filedialog::Status::Failed) m_locationError = r->error;
        }
        if (ImGuiFileDialog::Instance()->Display("game-locations", ImGuiWindowFlags_NoCollapse,
                                                 { px(650), px(400) }, { px(1100), px(760) })) {
            if (ImGuiFileDialog::Instance()->IsOk())
                applyLocation(ImGuiFileDialog::Instance()->GetCurrentPath());
            ImGuiFileDialog::Instance()->Close();
        }
        if (auto r = m_ps3Dialog.poll()) {  // the PS3 HD import's source (native_dialog.h)
            if (r->status == filedialog::Status::Chosen) startPs3Import(m_ps3Game, r->path);
            else if (r->status == filedialog::Status::Unavailable) openPs3Picker(m_ps3PickFolder);
        }
        if (ImGuiFileDialog::Instance()->Display("ps3src", ImGuiWindowFlags_NoCollapse, { px(650), px(400) }, { px(1100), px(760) })) {
            if (ImGuiFileDialog::Instance()->IsOk())
                startPs3Import(m_ps3Game, m_ps3PickFolder ? ImGuiFileDialog::Instance()->GetCurrentPath() : ImGuiFileDialog::Instance()->GetFilePathName());
            ImGuiFileDialog::Instance()->Close();
        }
        if (auto r = m_sourceDialog.poll()) {  // the system's folder picker (native_dialog.h)
            if (r->status == filedialog::Status::Chosen) std::snprintf(m_source, sizeof(m_source), "%s", r->path.c_str());
            else if (r->status == filedialog::Status::Unavailable) openSourcePicker();
        }
        if (!ImGuiFileDialog::Instance()->Display("source", ImGuiWindowFlags_NoCollapse, { px(650), px(400) }, { px(1100), px(760) }))
            return;
        if (ImGuiFileDialog::Instance()->IsOk())
            std::snprintf(m_source, sizeof(m_source), "%s", ImGuiFileDialog::Instance()->GetCurrentPath().c_str());
        ImGuiFileDialog::Instance()->Close();
    }

private:
    void openSourcePicker() {
        ImGuiFileDialog::Instance()->OpenDialog("source", "Choose the SafeHouse source folder", nullptr,
                                                utf8(filedialog::startFolder({ m_source })), "", 1, nullptr, ImGuiFileDialogFlags_Modal);
    }
    filedialog::Async m_sourceDialog;
    // ------------------------------------------------------------ storage
    fs::path file() const { return settings::settingsPath(m_backend.userDir(m_game)); }
    // The shown game's defaults (Sly 2 has its own keyboard / mouse layout).
    settings::SystemSettings gameDefaults() const { return m_game == GameId::Sly2 ? settings::sly2Defaults() : settings::defaults(); }
    void load() {
        m_result = settings::load(file(), m_settings, &m_version);
        if (!settings::loaded(m_result)) m_settings = gameDefaults();
        if (m_game == GameId::Sly2) m_actions = sly2input::load(m_backend.userDir(m_game));
        std::error_code ec;
        m_mtime = fs::last_write_time(file(), ec);
        if (ec) m_mtime = {};
        if (m_game == GameId::Sly2) {
            m_actionMtime = fs::last_write_time(sly2input::profilePath(m_backend.userDir(m_game)), ec);
            if (ec) m_actionMtime = {};
        }
        m_nextPoll = ImGui::GetCurrentContext() ? ImGui::GetTime() + 0.5 : 0.0;
        m_debug = readDebugMode();
    }
    void poll() {
        if (ImGui::GetTime() < m_nextPoll || capturing()) return;
        m_nextPoll = ImGui::GetTime() + 0.5;
        std::error_code ec;
        const auto now = fs::last_write_time(file(), ec);
        if ((ec ? fs::file_time_type{} : now) != m_mtime) load();
        if (m_game == GameId::Sly2) {
            const auto actionsNow = fs::last_write_time(sly2input::profilePath(m_backend.userDir(m_game)), ec);
            if ((ec ? fs::file_time_type{} : actionsNow) != m_actionMtime) load();
        }
    }
    void commitActions() {
        if (!writable() || m_actions.readOnly) return;
        m_saveOk = sly2input::save(m_backend.userDir(m_game), m_actions);
        m_savedAt = ImGui::GetTime();
        std::error_code ec;
        m_actionMtime = fs::last_write_time(sly2input::profilePath(m_backend.userDir(m_game)), ec);
        if (ec) m_actionMtime = {};
    }
    bool writable() const { return m_result != settings::LoadResult::Newer && !m_fixture; }
    void commit() {
        if (!writable()) return;
        settings::sanitizeStatic(m_settings, monitorHeight(), m_maxSamples,
                                 m_game == GameId::Sly2 ? settings::kSly2DefaultKeyboardBindings : settings::kDefaultKeyboardBindings);
        m_saveOk = settings::save(file(), m_settings);
        m_savedAt = ImGui::GetTime();
        if (m_saveOk) {
            m_result = settings::LoadResult::Ok;
            std::error_code ec;
            m_mtime = fs::last_write_time(file(), ec);
        }
    }
    void persistLauncher() {
        if (m_fixture) return;
        writeJson(m_backend.locations.config / "launcher.json", m_backend.config);
    }
    int monitorHeight() const {
        // Wayland does not reveal window positions: use the primary monitor,
        // as the game does there.
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            if (GLFWmonitor* primary = glfwGetPrimaryMonitor())
                if (const GLFWvidmode* mode = glfwGetVideoMode(primary)) return mode->height;
            return 0;
        }
        int count = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&count);
        int wx = 0, wy = 0, ww = 0, wh = 0;
        glfwGetWindowPos(m_window, &wx, &wy);
        glfwGetWindowSize(m_window, &ww, &wh);
        for (int i = 0; i < count; ++i) {
            int mx = 0, my = 0;
            glfwGetMonitorPos(monitors[i], &mx, &my);
            const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
            if (mode && wx + ww / 2 >= mx && wx + ww / 2 < mx + mode->width && wy + wh / 2 >= my && wy + wh / 2 < my + mode->height)
                return mode->height;
        }
        if (GLFWmonitor* primary = glfwGetPrimaryMonitor())
            if (const GLFWvidmode* mode = glfwGetVideoMode(primary)) return mode->height;
        return 0;
    }
    static constexpr int kUncappedFrameRate = 1000;
    std::vector<int> frameRates() const {
        std::vector<int> rates{ 30, 60, 90, 120, 240 };
        // Sly 2 runs 60 Hz steps drawn between them at any rate (Sly2-Release frameinterp.h): 144 and no limit too
        // (kUncappedFrameRate, an ordinary value of the shared field: Sly2-Release reads it as no limit)
        if (m_game == GameId::Sly2) rates.insert(rates.end(), { 144, kUncappedFrameRate });
        if (GLFWmonitor* m = glfwGetPrimaryMonitor()) {
            int count = 0;
            const GLFWvidmode* modes = glfwGetVideoModes(m, &count);
            int maximum = 0;
            for (int i = 0; i < count; ++i) maximum = std::max(maximum, modes[i].refreshRate);
            if (maximum >= 30) rates.push_back(maximum);
        }
        std::sort(rates.begin(), rates.end());
        rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
        return rates;
    }
    fs::path debugFile() const { return m_backend.debugModeFile(); }
    bool readDebugMode() const { return debugModeIn(debugFile()); }
    void writeDebugMode(bool enabled) {
        if (m_fixture) return;
        std::ofstream(debugFile()) << "[Debug]\nDebug Mode=" << (enabled ? 1 : 0) << '\n';
    }

    // ------------------------------------------------------------ capture
    static SettingsPage*& active() {
        static SettingsPage* page = nullptr;
        return page;
    }
    static GLFWkeyfun& previousKeyCallback() {
        static GLFWkeyfun callback = nullptr;
        return callback;
    }
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        SettingsPage* page = active();
        // Presses go to the capture and never reach the UI. Releases still do,
        // so the Enter that started the capture is not left held down.
        if (page && page->m_captureAction >= 0 && (!page->m_captureController || (page->m_captureAction >= kAdvancedCapture && (key == GLFW_KEY_DELETE || key == GLFW_KEY_ESCAPE))) && action != GLFW_RELEASE) {
            if (action == GLFW_PRESS) page->keyCaptured(key);
            return;
        }
        if (previousKeyCallback()) previousKeyCallback()(window, key, scancode, action, mods);
    }
    void keyCaptured(int key) {
        if (key == GLFW_KEY_ESCAPE) {
            cancelCapture();
            return;
        }
        if (m_captureAction >= kAdvancedCapture) {
            if (key != GLFW_KEY_DELETE && !settings::validKeyboardBinding(key)) { m_captureNote = "That key cannot be bound."; return; }
            (m_captureController ? m_actions.gamepad : m_actions.keyboard)[size_t(m_captureAction - kAdvancedCapture)] = key == GLFW_KEY_DELETE ? -1 : key;
            m_captureAction = -1;
            commitActions();
            return;
        }
        const bool camera = m_captureAction >= settings::kBindingCount;
        if (camera ? key == settings::kKeyUnknown || !settings::validCameraKey(key) : !settings::validKeyboardBinding(key)) {
            m_captureNote = "That key cannot be bound.";
            return;
        }
        if (camera) m_settings.cameraKeyBindings[m_captureAction - settings::kBindingCount] = key;
        else m_settings.keyboardBindings[m_captureAction] = key;
        m_captureAction = -1;
        commit();
    }
    const char* captureName() const {
        if (m_captureAction >= kAdvancedCapture) return sly2input::kActions[size_t(m_captureAction - kAdvancedCapture)].label;
        return m_captureAction >= settings::kBindingCount ? cameraNames[static_cast<size_t>(m_captureAction - settings::kBindingCount)]
                                                          : actionNames[static_cast<size_t>(m_captureAction)];
    }
    // Action capture accepts mouse buttons and the wheel after the click that
    // selected the row has been released. Camera bindings use keyboard keys.
    void pollMouseCapture() {
        if (!capturing() || m_captureController || (m_captureAction >= settings::kBindingCount && m_captureAction < kAdvancedCapture)) return;
        const auto& io = ImGui::GetIO();
        if (!m_captureArmed) {
            bool held = false;
            for (int b = 0; b < 5; ++b) held |= ImGui::IsMouseDown(b);
            if (!held) m_captureArmed = true;
            return;
        }
        int binding = -1;
        for (int button = 0; button < 5 && binding < 0; ++button)
            if (ImGui::IsMouseClicked(button)) binding = settings::kMouseBindingBase + button;
        if (binding < 0 && io.MouseWheel > 0) binding = settings::kMouseWheelUp;
        if (binding < 0 && io.MouseWheel < 0) binding = settings::kMouseWheelDown;
        if (binding < 0) return;
        if (m_captureAction >= kAdvancedCapture) {
            m_actions.keyboard[size_t(m_captureAction - kAdvancedCapture)] = binding;
            m_captureAction = -1;
            m_captureCooldown = ImGui::GetTime() + 0.3;
            commitActions();
            return;
        }
        m_settings.keyboardBindings[m_captureAction] = binding;
        m_captureAction = -1;
        m_captureCooldown = ImGui::GetTime() + 0.3;
        commit();
    }
    void startCapture(int action, bool controller) {
        m_captureAction = action;
        m_captureController = controller;
        m_captureArmed = false;
        m_captureStarted = ImGui::GetTime();
        m_captureNote.clear();
    }
    static constexpr double kPadCaptureSeconds = 6.0;
    void pollPadCapture() {
        if (!capturingPad()) return;
        if (ImGui::GetTime() - m_captureStarted > kPadCaptureSeconds) {
            cancelCapture();
            return;
        }
        int pressed = -1;
        bool any = false;
        for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
            GLFWgamepadstate state{};
            if (!glfwJoystickIsGamepad(jid) || input::ignoredSteamDuplicate(jid) || !glfwGetGamepadState(jid, &state)) continue;
            for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
                if (state.buttons[b] == GLFW_PRESS && b != GLFW_GAMEPAD_BUTTON_GUIDE) {
                    any = true;
                    if (pressed < 0) pressed = b;
                }
            if ((state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1) * 0.5f > 0.5f) any = true, pressed = pressed < 0 ? settings::kGamepadLeftTrigger : pressed;
            if ((state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1) * 0.5f > 0.5f) any = true, pressed = pressed < 0 ? settings::kGamepadRightTrigger : pressed;
        }
        // Wait for the button that started the capture to be released.
        if (!m_captureArmed) {
            m_captureArmed = !any;
            return;
        }
        if (pressed < 0) return;
        const bool advanced = m_captureAction >= kAdvancedCapture;
        if (advanced) m_actions.gamepad[size_t(m_captureAction - kAdvancedCapture)] = pressed;
        else m_settings.gamepadBindings[m_captureAction] = pressed;
        m_captureAction = -1;
        m_captureCooldown = ImGui::GetTime() + 0.3;
        if (advanced) commitActions(); else commit();
    }

    // ------------------------------------------------------------ chrome
    void saveIndicator(ImVec2 titlePos) {
        const char* label = nullptr;
        uint32_t tone = color::success;
        if (m_result == settings::LoadResult::Newer) label = "Read only", tone = color::warning;
        else if (!m_saveOk) label = "Could not save settings", tone = color::danger;
        else if (ImGui::GetTime() - m_savedAt < 2.5) label = "Saved";
        if (!label) return;
        const ImVec2 after = ImGui::GetCursorPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const auto t = measure(fonts().caption, label);
        const ImVec2 p{ ImGui::GetWindowPos().x + titlePos.x + w - t.x, ImGui::GetWindowPos().y + titlePos.y - ImGui::GetScrollY() + px(12) };
        auto* dl = ImGui::GetWindowDrawList();
        drawIcon(dl, tone == color::success ? Icon::Check : Icon::Warning, { p.x - px(14), p.y + t.y * 0.5f }, px(16), ink(tone));
        dl->AddText(fonts().caption, fonts().caption->FontSize, p, ink(tone), label);
        ImGui::SetCursorPos(after);
    }
    void rail(float width, float height) {
        // Padding keeps the focus ring inside the rail's clip rectangle.
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - px(6));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(6), px(6) });
        ImGui::BeginChild("settings-rail", { width + px(12), height }, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NavFlattened);
        ImGui::PopStyleVar();
        for (int i = 0; i < CategoryCount; ++i) {
            if (!shown(i)) continue;
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const ImGuiID id = window->GetID(names[static_cast<size_t>(i)]);
            const ImRect bb(window->DC.CursorPos, { window->DC.CursorPos.x + width, window->DC.CursorPos.y + px(48) });
            ImGui::ItemSize(bb.GetSize());
            if (!ImGui::ItemAdd(bb, id)) continue;
            bool hovered = false, held = false;
            if (ImGui::ButtonBehavior(bb, id, &hovered, &held) && category != i) selectCategory(i);
            // Pad and keyboard focus selects (console style).
            if (ImGui::IsItemFocused() && !input::tracker().lastWasMouse() && category != i && !m_focusRail) selectCategory(i);
            if ((m_focusRail || (focusRequested() && GImGui->FrameCount != focusRequestFrame())) && category == i) {
                setFocus(id, window, !input::tracker().lastWasMouse());
                focusRequested() = false;
            }
            // Right moves into the category's rows.
            if (ImGui::IsItemFocused() && navRightPressed()) m_focusRows = true;
            const bool selected = category == i;
            auto* dl = window->DrawList;
            if (selected) {
                dl->AddRectFilled(bb.Min, bb.Max, ink(color::raised), px(8));
                dl->AddRectFilled({ bb.Min.x, bb.Min.y + px(12) }, { bb.Min.x + px(3), bb.Max.y - px(12) }, ink(color::cta), px(2));
            } else if (hovered)
                dl->AddRectFilled(bb.Min, bb.Max, ink(color::surface), px(8));
            ImFont* font = selected ? fonts().tab : fonts().body;
            dl->AddText(font, font->FontSize, { bb.Min.x + px(18), std::round(bb.GetCenter().y - font->FontSize * 0.5f) },
                        ink(selected ? color::text : hovered ? color::text : color::text2), names[static_cast<size_t>(i)]);
            focusRing(bb, id, px(8));
            recordLayout(names[static_cast<size_t>(i)], bb);
        }
        ImGui::EndChild();
    }
    void describe(const char* help) {
        if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) m_help = help;
    }
    void pane(float width, float height, bool gameRunning) {
        beginCard("settings-pane", { width, height }, 0);
        const float padX = px(24);
        ImGui::SetCursorPos({ padX, px(20) });
        ImGui::BeginGroup();
        heading(names[static_cast<size_t>(category)]);
        tighten(8);
        text(blurbs[static_cast<size_t>(category)], fonts().caption, color::muted);
        ImGui::EndGroup();
        const bool systemCategory = category <= Interface;
        const bool locked = systemCategory && gameRunning;
        const bool readOnly = systemCategory && m_result == settings::LoadResult::Newer;
        ImGui::SetCursorPosX(padX);
        ImGui::PushItemWidth(width - padX * 2);
        if (locked || readOnly || (systemCategory && m_result == settings::LoadResult::Corrupt)) {
            ImGui::BeginChild("pane-banner", { width - padX * 2, 0 }, ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NavFlattened);
            if (locked) banner("locked", "Close the game to change settings – changes made in-game are saved automatically.", BannerTone::Info, false);
            else if (readOnly) banner("newer", "These settings were saved by a newer version of the game. Update SafeHouse to change them here.", BannerTone::Warning, false);
            else banner("corrupt", "The settings file could not be read, so defaults are shown. Changing anything saves a fresh file.", BannerTone::Warning, false);
            ImGui::EndChild();
        }
        ImGui::PopItemWidth();
        const float footer = px(56);
        ImGui::SetCursorPosX(px(4));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(6), px(6) });
        ImGui::BeginChild("pane-rows", { width - px(8), ImGui::GetContentRegionAvail().y - footer }, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NavFlattened);
        ImGui::PopStyleVar();
        if (m_scrollTop) ImGui::SetScrollY(0), m_scrollTop = false;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { px(12), px(4) });
        const bool paneWasFocused = m_paneFocused;
        m_paneFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        firstRow() = {};
        if (systemCategory) gameRow(locked);
        ImGui::BeginDisabled(locked || readOnly);
        switch (category) {
        case Display: displayRows(); break;
        case Graphics: graphicsRows(); break;
        case Audio: audioRows(); break;
        case Controls: controlsRows(); break;
        case Interface: interfaceRows(); break;
        case Launcher: launcherRows(); break;
        case Games: gamesRows(gameRunning); break;
        case Advanced: advancedRows(gameRunning); break;
        default: break;
        }
        ImGui::EndDisabled();
        // Entering the rows from the rail (or the header) with the pad or the
        // keyboard starts at the top row, not at the row level with the rail.
        if (((m_paneFocused && !paneWasFocused && !input::tracker().lastWasMouse()) || m_focusRows) && firstRow().id &&
            GImGui->NavId != firstRow().id)
            setFocus(firstRow().id, firstRow().window, true);
        m_focusRows = false;
        gap(8);
        ImGui::PopStyleVar();
        padScrollHere();
        ImGui::EndChild();
        // Footer: description of the focused row, and Restore defaults.
        ImVec2 p = ImGui::GetCursorScreenPos();
        p.x = ImGui::GetWindowPos().x;
        p.y -= ImGui::GetStyle().ItemSpacing.y;
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddLine({ p.x, p.y }, { p.x + width, p.y }, ink(color::divider), px(1));
        const float restoreW = px(180);
        const float wrap = width - padX * 2 - restoreW - px(16);
        const char* help = m_help.empty() ? "" : m_help.c_str();
        const auto t = fonts().caption->CalcTextSizeA(fonts().caption->FontSize, FLT_MAX, wrap, help);
        dl->AddText(fonts().caption, fonts().caption->FontSize, { p.x + padX, std::round(p.y + (footer - t.y) * 0.5f) },
                    ink(color::text2), help, nullptr, wrap);
        ImGui::SetCursorScreenPos({ p.x + width - padX - restoreW, p.y + (footer - controlHeight()) * 0.5f });
        ImGui::BeginDisabled(locked || readOnly);
        if (button("Restore defaults", Tone::Quiet, { restoreW, 0 }, Icon::Refresh)) restoreDefaults();
        ImGui::EndDisabled();
        endCard();
    }
    void restoreDefaults() {
        const auto d = gameDefaults();
        auto& s = m_settings;
        switch (category) {
        case Display:
            s.windowMode = d.windowMode, s.vsyncEnabled = d.vsyncEnabled, s.aspectMode = d.aspectMode, s.frameRate = d.frameRate;
            break;
        case Graphics:
            s.internalResolutionHeight = d.internalResolutionHeight, s.msaaEnabled = d.msaaEnabled, s.msaaSamples = d.msaaSamples;
            s.drawDistance = d.drawDistance, s.fogType = d.fogType, s.longShadows = d.longShadows;
            break;
        case Audio: settings::defaultAudio(s); break;
        case Controls:
            if (m_game == GameId::Sly2 && m_actions.enabled) {
                m_actions = sly2input::defaults();
                m_actions.enabled = true;
                commitActions();
                return;
            }
            std::copy(std::begin(d.keyboardBindings), std::end(d.keyboardBindings), s.keyboardBindings);
            std::copy(std::begin(d.gamepadBindings), std::end(d.gamepadBindings), s.gamepadBindings);
            s.disableControllerInputWhenUnfocused = d.disableControllerInputWhenUnfocused;
            s.alwaysOriginalPsPrompts = d.alwaysOriginalPsPrompts, s.promptStyle = d.promptStyle;
            s.instantSkipCutscenes = d.instantSkipCutscenes;
            s.mouseCamera = d.mouseCamera, s.mouseSensitivity = d.mouseSensitivity, s.mouseAimSensitivity = d.mouseAimSensitivity;
            s.mouseInvertX = d.mouseInvertX, s.mouseInvertY = d.mouseInvertY, s.mouseRawInput = d.mouseRawInput;
            s.padInvertCameraX = d.padInvertCameraX, s.padInvertCameraY = d.padInvertCameraY;
            s.autoCameraRotation = d.autoCameraRotation;
            s.mouseInvertBinocX = d.mouseInvertBinocX, s.mouseInvertBinocY = d.mouseInvertBinocY;
            s.mouseInvertTurretX = d.mouseInvertTurretX, s.mouseInvertTurretY = d.mouseInvertTurretY;
            s.mouseWheelZoom = d.mouseWheelZoom, s.mouseMenus = d.mouseMenus, s.hackMouseAim = d.hackMouseAim;
            s.controllerVibration = d.controllerVibration;
            std::copy(std::begin(d.cameraKeyBindings), std::end(d.cameraKeyBindings), s.cameraKeyBindings);
            break;
        case Interface: s.guiStyle = d.guiStyle, s.guiScale = d.guiScale; break;
        case Launcher:
            m_backend.config.erase("ui_scale");
            m_backend.config.erase("start_fullscreen");
            m_backend.config.erase("prompt_style");
            m_backend.config.erase("check_updates");
            persistLauncher();
            if (setScale) setScale(1.0f);
            return;
        case Advanced:
            m_debug = false;
            writeDebugMode(false);
            if (showLogs) *showLogs = false;
            m_backend.config.erase("show_logs");
            persistLauncher();
            if (setLogging && !m_fixture) setLogging(false);
            return;
        default: return;
        }
        commit();
    }

    // ------------------------------------------------------------ rows
    // The game whose settings are shown (Sly 1 / Sly 2).
    void gameRow(bool locked) {
        static const std::vector<std::string> games{ cane::game(GameId::Sly1).shortName, cane::game(GameId::Sly2).shortName };
        int index = m_game == GameId::Sly2 ? 1 : 0;
        ImGui::BeginDisabled(locked);
        if (stepperRow("Game", &index, games)) {
            cancelCapture();
            m_game = index == 1 ? GameId::Sly2 : GameId::Sly1;
            if (!shown(category)) category = Display;
            load();
        }
        ImGui::EndDisabled();
        describe("Which game's settings this page shows. Each game keeps its own settings file.");
    }
    template <class T> static int indexOf(const std::vector<T>& values, T value) {
        auto it = std::find(values.begin(), values.end(), value);
        return it == values.end() ? -1 : static_cast<int>(it - values.begin());
    }
    bool toggle(const char* label, int& field, const char* help) {
        bool value = field != 0;
        const bool changed = toggleRow(label, &value);
        describe(help);
        if (changed) field = value ? 1 : 0, commit();
        return changed;
    }
    // A whole-number option as a stepper over min..max.
    bool rangeRow(const char* label, int& field, int minimum, int maximum, int step, const char* help,
                  const std::function<std::string(int)>& name) {
        std::vector<int> values;
        std::vector<std::string> labels;
        for (int v = minimum; v <= maximum; v += step) values.push_back(v), labels.push_back(name(v));
        int index = indexOf(values, field);
        const std::string custom = name(field);
        const bool changed = stepperRow(label, &index, labels, nullptr, index < 0 ? custom.c_str() : nullptr);
        describe(help);
        if (changed) field = values[static_cast<size_t>(index)], commit();
        return changed;
    }
    static std::string percent(int v) { return std::to_string(v) + "%"; }
    void displayRows() {
        static const std::vector<std::string> modes{ "Windowed", "Borderless", "Fullscreen" };
        int mode = m_settings.windowMode;
        if (stepperRow("Window mode", &mode, modes)) m_settings.windowMode = mode, commit();
        describe("Borderless fills the screen without changing its resolution.");
        toggle("VSync", m_settings.vsyncEnabled, "Matches the frame rate to the display to stop tearing.");
        static const std::vector<std::string> aspects{ "Fit to screen", "4:3", "16:9", "16:10", "4:3 (PS2)", "16:9 (PS2)" };
        int aspect = m_settings.aspectMode;
        if (stepperRow("Aspect ratio", &aspect, aspects)) m_settings.aspectMode = aspect, commit();
        describe("Fit to screen uses the whole window. The PS2 options match the original framing.");
        const auto rates = frameRates();
        std::vector<std::string> labels;
        for (int r : rates) labels.push_back(r >= kUncappedFrameRate ? std::string("Uncapped") : std::to_string(r) + " FPS");
        int index = indexOf(rates, m_settings.frameRate);
        const std::string custom = std::to_string(m_settings.frameRate) + " FPS";
        if (stepperRow("Frame rate limit", &index, labels, nullptr, index < 0 ? custom.c_str() : nullptr))
            m_settings.frameRate = rates[static_cast<size_t>(index)], commit();
        describe("The highest frame rate the game runs at. Gameplay speed is the same at every setting.");
    }
    void graphicsRows() {
        const int monitor = monitorHeight();
        std::vector<int> heights;
        std::vector<std::string> labels;
        for (int h : settings::kInternalResolutions)
            if (h == 0 || monitor <= 0 || h <= monitor) heights.push_back(h), labels.push_back(h ? std::to_string(h) + "p" : "Native");
        int index = indexOf(heights, m_settings.internalResolutionHeight);
        // A height above this monitor's (set on a bigger display) is kept
        // and shown as it is until another value is picked.
        const std::string stored = std::to_string(m_settings.internalResolutionHeight) + "p";
        if (stepperRow("Internal resolution", &index, labels, nullptr, index < 0 ? stored.c_str() : nullptr))
            m_settings.internalResolutionHeight = heights[static_cast<size_t>(index)], commit();
        describe("Renders the 3D scene at this height, then scales it to the window. Native matches the window.");
        std::vector<int> samples{ 0 };
        std::vector<std::string> aa{ "Off" };
        for (int n : { 2, 4, 8, 16 })
            if (m_maxSamples <= 0 || n <= m_maxSamples) samples.push_back(n), aa.push_back(std::to_string(n) + "x");
        int current = m_settings.msaaEnabled ? m_settings.msaaSamples : 0;
        int aaIndex = indexOf(samples, current);
        if (stepperRow("Anti-aliasing", &aaIndex, aa, nullptr, aaIndex < 0 ? "Custom" : nullptr)) {
            const int n = samples[static_cast<size_t>(aaIndex)];
            m_settings.msaaEnabled = n != 0;
            if (n) m_settings.msaaSamples = n;
            commit();
        }
        describe("Smooths jagged edges (MSAA). Higher values cost more GPU time.");
        static const std::vector<float> distances{ 0.75f, 1.0f, 1.5f, 2.0f, 3.0f };
        static const std::vector<std::string> distanceLabels{ "Low", "Original", "High", "Very high", "Extreme" };
        int d = -1;
        for (size_t i = 0; i < distances.size(); ++i)
            if (std::fabs(distances[i] - m_settings.drawDistance) < 0.01f) d = static_cast<int>(i);
        char customDistance[32];
        std::snprintf(customDistance, sizeof(customDistance), "%.2fx", m_settings.drawDistance);
        if (stepperRow("Draw distance", &d, distanceLabels, nullptr, d < 0 ? customDistance : nullptr))
            m_settings.drawDistance = distances[static_cast<size_t>(d)], commit();
        describe(m_game == GameId::Sly2 ? "How far away scenery, props and effects stay visible. Original matches the PS2; higher settings "
                                          "keep characters and gameplay at the PS2's distances."
                                        : "How far away objects stay visible. Original matches the PS2.");
        // (settings v16 longShadows: Sly2-Release render.cpp FBuildShadow2, a PC improvement, not the original)
        if (m_game == GameId::Sly2)
            toggle("Long shadows", m_settings.longShadows,
                   "Character shadows from low lights reach the floor in full. Off: they end at a fixed length, as on the PS2.");
        if (m_game != GameId::Sly1) return;   // (Sly 2 has no fog option)
        static const std::vector<std::string> fogs{ "PS2 style", "PS3 style" };
        int fog = m_settings.fogType == settings::Fog_PS3 ? 1 : m_settings.fogType == settings::Fog_PS2 ? 0 : -1;
        if (stepperRow("Fog", &fog, fogs, nullptr, fog < 0 ? "Off" : nullptr))
            m_settings.fogType = fog == 1 ? settings::Fog_PS3 : settings::Fog_PS2, commit();
        describe("PS2 style is the original look. PS3 style follows the remaster.");
    }
    void audioRows() {
        if (m_game == GameId::Sly1) {   // (Sly 2 has stereo / mono in its own options)
            static const std::vector<std::string> speakers{ "Stereo", "Mono" };
            int speaker = m_settings.stereoEnabled ? 0 : 1;
            if (stepperRow("Speakers", &speaker, speakers)) m_settings.stereoEnabled = speaker == 0, commit();
            describe("Mono mixes both channels together, for a single speaker or one ear.");
        }
        auto volume = [&](const char* label, float& value, const char* help) {
            float v = value;
            if (sliderRow(label, &v)) value = v, m_volumeDirty = true;
            describe(help);
        };
        volume("Music", m_settings.musicVolume, "Background music.");
        volume("Sound effects", m_settings.soundEffectsVolume, "Gadgets, footsteps, guards and the world around you.");
        volume("Dialogue", m_settings.dialogueVolume, "Character voices and cutscenes.");
        if (m_volumeDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_volumeDirty = false, commit();
    }
    static constexpr std::array<const char*, settings::kBindingCount> actionNames{
        "Move up", "Move down", "Move left", "Move right", "Square", "Circle", "Cross", "Triangle",
        "Start", "Select", "L1", "R1", "L2", "R2", "L3", "R3" };
    static constexpr std::array<input::Button, settings::kBindingCount> actionButtons{
        input::Button::DpadUp, input::Button::DpadDown, input::Button::DpadLeft, input::Button::DpadRight,
        input::Button::West,   input::Button::East,     input::Button::South,    input::Button::North,
        input::Button::Start,  input::Button::Back,     input::Button::LB,       input::Button::RB,
        input::Button::LT,     input::Button::RT,       input::Button::LSPress,  input::Button::RSPress };
    void section(const char* title) {
        gap(10);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(16));
        overline(title);
        tighten(4);
    }
    // One binding row: the action (with its PlayStation glyph) and its key or button.
    void bindingRow(int action, bool controller, input::Family family) {
        ImGui::PushID(action + (controller ? 100 : 0));
        const bool advanced = action >= kAdvancedCapture;
        const int index = advanced ? action - kAdvancedCapture : action;
        const char* label = advanced ? sly2input::kActions[size_t(index)].label : actionNames[size_t(index)];
        const Row r = rowBehavior(label, px(52), false, 0.45f);
        if (r.visible) {
            const bool capturingThis = m_captureAction == action && m_captureController == controller;
            if (r.activated && !r.disabled && !capturingThis && ImGui::GetTime() > m_captureCooldown) startCapture(action, controller);
            auto* dl = ImGui::GetWindowDrawList();
            const float cy = r.bb.GetCenter().y;
            if (!advanced) drawPadButton(dl, { r.bb.Min.x + px(30), cy }, px(28), input::Family::PlayStation, actionButtons[size_t(index)]);
            dl->AddText(fonts().body, fonts().body->FontSize, { r.bb.Min.x + px(advanced ? 16 : 56), std::round(cy - fonts().body->FontSize * 0.5f) },
                        ink(color::text), label);
            recordLayout(label, r.bb);
            const int* bindings = advanced ? (controller ? m_actions.gamepad.data() : m_actions.keyboard.data())
                                          : controller ? m_settings.gamepadBindings : m_settings.keyboardBindings;
            const int value = bindings[index];
            int duplicate = -1;
            for (int other = 0; !advanced && other < settings::kBindingCount; ++other)
                if (other != action && value != settings::kKeyUnknown && bindings[other] == value) duplicate = other;
            const float right = r.control.Max.x;
            if (capturingThis) {
                const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
                const char* prompt = controller ? "Press a button…" : "Press a key or mouse button…";
                const auto t = measure(fonts().tab, prompt);
                dl->AddRect({ r.control.Min.x, r.bb.Min.y + px(8) }, { right, r.bb.Max.y - px(8) }, ink(color::cta, pulse), px(6), 0, px(2));
                dl->AddText(fonts().tab, fonts().tab->FontSize, { std::round(r.control.GetCenter().x - t.x * 0.5f), std::round(cy - t.y * 0.5f) },
                            ink(color::cta), prompt);
                m_help = m_captureNote.empty() ? (controller ? std::string("Press the controller button for this action. Any button can be "
                                                                           "bound, so wait 6 seconds or press Escape to cancel.")
                                                             : family == input::Family::Keyboard
                                                                   ? std::string("Press a key, mouse button, or scroll the wheel. Escape cancels.")
                                                                   : "Press a key, mouse button, or scroll the wheel. Escape or " +
                                                                         std::string(backButtonLabel()) + " cancels.")
                                               : m_captureNote;
            } else if (controller) {
                const auto button = input::fromGlfwBinding(value);
                const char* label = input::buttonLabel(button, family);
                const auto t = measure(fonts().body, label);
                dl->AddText(fonts().body, fonts().body->FontSize, { right - t.x, std::round(cy - t.y * 0.5f) }, ink(color::text2), label);
                const float glyph = px(30);
                const float gw = padButtonWidth(glyph, family, button);
                if (button != input::Button::Count) drawPadButton(dl, { right - t.x - px(12) - gw * 0.5f, cy }, glyph, family, button);
            } else {
                const std::string name = keyName(value);
                const float h = px(30);
                const float w = keycapWidth(name.c_str(), h);
                if (value == settings::kKeyUnknown) {
                    const auto t = measure(fonts().body, "Unbound");
                    dl->AddText(fonts().body, fonts().body->FontSize, { right - t.x, std::round(cy - t.y * 0.5f) }, ink(color::muted), "Unbound");
                } else
                    drawKeycap(dl, { right - w, std::round(cy - h * 0.5f) - px(1) }, h, name.c_str());
            }
            if (duplicate >= 0 && !capturingThis) {
                drawIcon(dl, Icon::Warning, { r.control.Min.x + px(10), cy }, px(18), ink(color::warning));
                if (ImGui::IsItemFocused() || ImGui::IsItemHovered())
                    m_help = std::string("Also used by ") + actionNames[static_cast<size_t>(duplicate)] + ". Both actions trigger together.";
            }
            if (!capturingThis && m_help.empty() && (ImGui::IsItemFocused() || ImGui::IsItemHovered()))
                m_help = controller ? "Select, then press the controller button to use for this action."
                                    : "Select, then press the key to use for this action. Escape cancels.";
        }
        ImGui::PopID();
    }
    static constexpr std::array<const char*, settings::kCameraKeyCount> cameraNames{
        "Camera left", "Camera right", "Camera up", "Camera down" };
    // A keyboard camera (right stick) key (from ThiefRush-Dev's settings page).
    void cameraKeyRow(int index) {
        ImGui::PushID(200 + index);
        const char* label = cameraNames[static_cast<size_t>(index)];
        const Row r = rowBehavior(label, px(52), false, 0.45f);
        if (r.visible) {
            const int action = settings::kBindingCount + index;
            const bool capturingThis = m_captureAction == action && !m_captureController;
            if (r.activated && !r.disabled && !capturingThis && ImGui::GetTime() > m_captureCooldown) startCapture(action, false);
            auto* dl = ImGui::GetWindowDrawList();
            const float cy = r.bb.GetCenter().y;
            drawPadButton(dl, { r.bb.Min.x + px(30), cy }, px(28), input::Family::PlayStation, input::Button::RS);
            dl->AddText(fonts().body, fonts().body->FontSize, { r.bb.Min.x + px(56), std::round(cy - fonts().body->FontSize * 0.5f) },
                        ink(color::text), label);
            recordLayout(label, r.bb);
            const float right = r.control.Max.x;
            const int value = m_settings.cameraKeyBindings[index];
            if (capturingThis) {
                const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
                const auto t = measure(fonts().tab, "Press a key…");
                dl->AddRect({ r.control.Min.x, r.bb.Min.y + px(8) }, { right, r.bb.Max.y - px(8) }, ink(color::cta, pulse), px(6), 0, px(2));
                dl->AddText(fonts().tab, fonts().tab->FontSize, { std::round(r.control.GetCenter().x - t.x * 0.5f), std::round(cy - t.y * 0.5f) },
                            ink(color::cta), "Press a key…");
                m_help = m_captureNote.empty() ? std::string("Press the key that turns the camera. Escape cancels.") : m_captureNote;
            } else if (value == settings::kKeyUnknown) {
                const auto t = measure(fonts().body, "Unbound");
                dl->AddText(fonts().body, fonts().body->FontSize, { right - t.x, std::round(cy - t.y * 0.5f) }, ink(color::muted), "Unbound");
            } else {
                const std::string name = keyName(value);
                const float h = px(30), w = keycapWidth(name.c_str(), h);
                drawKeycap(dl, { right - w, std::round(cy - h * 0.5f) - px(1) }, h, name.c_str());
            }
            if (!capturingThis && (ImGui::IsItemFocused() || ImGui::IsItemHovered()))
                m_help = "Keyboard camera, like the right stick. In menus these keys also move the highlight.";
        }
        ImGui::PopID();
    }
    void controlsRows() {
        const bool advanced = m_game == GameId::Sly2 && m_actions.enabled;
        const auto family = input::promptFamily(promptStyle(), 0);
        if (m_game == GameId::Sly2) {
            ImGui::BeginDisabled(m_actions.readOnly);
            bool enabled = m_actions.enabled;
            if (toggleRow("Advanced Input Mapping", &enabled)) {
                m_actions.enabled = enabled;
                cancelCapture();
                commitActions();
            }
            describe("Bind gameplay actions independently. Off restores your original button mapping without changing it.");
            ImGui::EndDisabled();
            if (m_actions.readOnly) caption("The advanced controls file cannot be edited by this version.");
        }
        if (advanced) {
            section("Action bindings");
            static const std::vector<std::string> devices{"Keyboard and mouse", "Controller"};
            stepperRow("Input device", &m_actionDevice, devices);
            std::vector<std::string> groups;
            for (size_t i = 0; i < sly2input::kActionCount; ++i) {
                if (!m_debug && sly2input::isNoclipAction(sly2input::Action(i))) continue;
                const auto& action = sly2input::kActions[i];
                if (std::find(groups.begin(), groups.end(), action.group) == groups.end()) groups.emplace_back(action.group);
            }
            m_actionGroup = std::clamp(m_actionGroup, 0, int(groups.size()) - 1);
            stepperRow("Action group", &m_actionGroup, groups);
            ImGui::BeginDisabled(m_actions.readOnly);
            for (size_t i = 0; i < sly2input::kActionCount; ++i)
                if (groups[size_t(m_actionGroup)] == sly2input::kActions[i].group)
                    bindingRow(kAdvancedCapture + int(i), m_actionDevice == 1, family);
            ImGui::EndDisabled();
            caption("Bindings may be shared. Actions apply only in their gameplay context. Escape cancels capture. Delete clears a binding.");
        }
        toggle("Disable controller input when unfocused", m_settings.disableControllerInputWhenUnfocused,
               "Ignore the controller while another window has focus.");
        // (settings v15 controllerVibration: platform/pad_rumble.h; Sly 1 and Sly 2 drive the motors)
        if (m_game == GameId::Sly1 || m_game == GameId::Sly2)
            toggle("Controller vibration", m_settings.controllerVibration,
                   "The game's rumble on hits, explosions and scripted moments. The in-game Vibration option must also be on.");
        section("Button prompts");
        static const std::vector<std::string> styles{ "Automatic", "PlayStation", "Xbox", "Nintendo", "Steam Deck", "Steam Controller", "PlayStation 4", "PlayStation 5", "Steam Controller (2026)" };
        int style = std::clamp(m_settings.promptStyle, 0, input::kPromptStyleLast);
        if (stepperRow("Button icons", &style, styles)) m_settings.promptStyle = style, commit();
        describe("Which buttons the game's prompts show. Automatic follows the controller or keyboard you used last.");
        toggle("Original PlayStation icons", m_settings.alwaysOriginalPsPrompts,
               "Every device shows the game's original PlayStation 2 button icons. Off: PS4 / PS5 pads show their own icons, other devices theirs.");
        toggle("Instant skip cutscenes", m_settings.instantSkipCutscenes,
               "One press of Circle skips a cutscene. Off: hold Circle for 2 seconds.");
        if (m_game == GameId::Sly1 || m_game == GameId::Sly2) {
            static const std::vector<std::string> cameraRotationLabels{ "Off", "On" };
            int cameraRotation = m_settings.autoCameraRotation != 0 ? 1 : 0;
            if (stepperRow("Automatic camera rotation", &cameraRotation, cameraRotationLabels))
                m_settings.autoCameraRotation = cameraRotation, commit();
            describe("Rotate the follow camera as you move.");
        }
        section("Controller");
        // (settings v17: Sly 2's controller camera inversion; off = the original's orbit camera)
        if (m_game == GameId::Sly2) {
            toggle("Invert camera horizontal", m_settings.padInvertCameraX,
                   "Off: the right stick orbits the camera as on the PS2 (right swings it around to the right, so the view turns left). On: right turns the view right.");
            toggle("Invert camera vertical", m_settings.padInvertCameraY,
                   "Off: as on the PS2, pushing the right stick up raises the camera to look down. On: up looks up.");
        }
        if (!advanced) for (int i = 0; i < settings::kBindingCount; ++i) bindingRow(i, true, family);
        section("Keyboard");
        if (!advanced) for (int i = 0; i < settings::kBindingCount; ++i) bindingRow(i, false, family);
        for (int i = 0; i < settings::kCameraKeyCount; ++i) cameraKeyRow(i);
        ImGui::Indent(px(16));
        caption("Escape also pauses and goes back, and Enter confirms in menus.");
        ImGui::Unindent(px(16));
        section("Mouse");
        toggle("Mouse camera", m_settings.mouseCamera,
               "While playing, the mouse turns the camera and aims the binocucom. Pause to free the cursor.");
        rangeRow("Camera sensitivity", m_settings.mouseSensitivity, 10, 500, 10, "How far the camera turns per mouse movement.", percent);
        rangeRow("Aim sensitivity", m_settings.mouseAimSensitivity, 10, 500, 10, "Binocucom aiming speed.", percent);
        if (m_game == GameId::Sly2) {
            // (settings v17: each view's own mouse inversion; off = natural everywhere)
            toggle("Invert camera horizontal", m_settings.mouseInvertX, "Off: moving the mouse right turns the camera right. On: it turns left.");
            toggle("Invert camera vertical", m_settings.mouseInvertY, "Off: moving the mouse up looks up. On: it looks down.");
            toggle("Invert binocucom horizontal", m_settings.mouseInvertBinocX, "Off: moving the mouse right looks right in the binocucom. On: it looks left.");
            toggle("Invert binocucom vertical", m_settings.mouseInvertBinocY, "Off: moving the mouse up looks up in the binocucom. On: it looks down.");
            toggle("Invert turret horizontal", m_settings.mouseInvertTurretX, "Off: moving the mouse right aims a turret right. On: it aims left.");
            toggle("Invert turret vertical", m_settings.mouseInvertTurretY, "Off: moving the mouse up aims a turret up. On: it aims down.");
        } else {
            toggle("Invert horizontal", m_settings.mouseInvertX, "Moving the mouse right turns the camera left.");
            toggle("Invert aim vertical", m_settings.mouseInvertY, "Moving the mouse up aims down in the binocucom.");
        }
        toggle("Raw input", m_settings.mouseRawInput, "Uses unaccelerated mouse motion where the system supports it.");
        if (m_game == GameId::Sly2)
            toggle("Mouse wheel zoom", m_settings.mouseWheelZoom, "Zoom the Binocucom and turrets with unassigned wheel inputs.");
        else
            toggle("Wheel zooms binocucom", m_settings.mouseWheelZoom, "When no action is bound to the wheel.");
        toggle("Mouse in menus", m_settings.mouseMenus, "Point at menu items, left click to choose and right click to go back.");
        if (m_game == GameId::Sly2)
            toggle("Mouse aim in hacking", m_settings.hackMouseAim,
                   "In Bentley's hacking games, the mouse moves a cursor the ship aims at and left click fires.");
        // (no Debug section: the debug keys are Settings > Advanced > Debug mode since 2026-11-04; settings v14
        //  debugKeys is ignored by the games and left as loaded)
        if (m_game == GameId::Sly2) {
            section("Set in each save");
            ImGui::Indent(px(16));
            caption("The controller's binocucom and turret inversion are the game's own options, saved with each game. "
                    "Change them in the in-game Options (Invert Binocucom Controls, Invert Turret Controls), also under "
                    "Controls and Prompts > Invert Controls.");
            ImGui::Unindent(px(16));
        }
        if (m_game != GameId::Sly1) return;
        section("Set in each save");
        ImGui::Indent(px(16));
        caption("Camera, binocular, turret and jetpack inversion and vibration are saved with each game. "
                "Change them from the in-game Controls menu.");
        ImGui::Unindent(px(16));
    }
    void interfaceRows() {
        static const std::vector<std::string> styles{ "PS2", "Modern" };
        int style = std::clamp(m_settings.guiStyle, 0, 1);
        if (stepperRow("Menu style", &style, styles)) {
            m_settings.guiStyle = style;
            m_settings.guiScale = settings::coupledGuiScale(style, m_settings.guiScale);
            commit();
        }
        describe("PS2 keeps the original menus. Modern uses cleaner, higher-resolution text.");
        const bool modern = m_settings.guiStyle == settings::Gui_Modern;
        const std::vector<float> scales = modern ? std::vector<float>{ 1.0f, 1.25f } : std::vector<float>{ 0.75f, 1.0f };
        const std::vector<std::string> labels = modern ? std::vector<std::string>{ "100%", "125%" } : std::vector<std::string>{ "75%", "100%" };
        int index = std::fabs(m_settings.guiScale - scales[0]) < 0.01f ? 0 : std::fabs(m_settings.guiScale - scales[1]) < 0.01f ? 1 : -1;
        if (stepperRow("Menu size", &index, labels)) m_settings.guiScale = scales[static_cast<size_t>(index)], commit();
        describe("The size of menus and the HUD. The choices depend on the menu style.");
    }
    void launcherRows() {
        static const std::vector<float> scales{ 1.0f, 1.25f, 1.5f };
        static const std::vector<std::string> labels{ "100%", "125%", "150%" };
        const float current = m_backend.config.value("ui_scale", 1.0f);
        int index = 0;
        for (size_t i = 0; i < scales.size(); ++i)
            if (std::fabs(scales[i] - current) < 0.01f) index = static_cast<int>(i);
        if (stepperRow("Launcher size", &index, labels)) {
            m_backend.config["ui_scale"] = scales[static_cast<size_t>(index)];
            persistLauncher();
            if (setScale) setScale(scales[static_cast<size_t>(index)]);
        }
        describe("Makes the launcher's text and controls larger, for TVs and handhelds.");
        bool fullscreen = m_backend.config.value("start_fullscreen", defaultFullscreen());
        if (toggleRow("Start in fullscreen", &fullscreen)) {
            m_backend.config["start_fullscreen"] = fullscreen;
            persistLauncher();
        }
        describe("Opens the launcher full screen. On by default on Steam Deck. Applies the next time it opens.");
        static const std::vector<std::string> styles{ "Automatic", "PlayStation", "Xbox", "Nintendo", "Steam Deck", "Steam Controller", "PlayStation 4", "PlayStation 5", "Steam Controller (2026)" };
        int style = promptStyle();
        if (stepperRow("Button prompts", &style, styles)) {
            m_backend.config["prompt_style"] = style;
            if (style == 0) m_backend.config.erase("prompt_style");
            persistLauncher();
        }
        describe("Which controller's buttons the launcher shows. Automatic follows the controller you used last.");
        if (updates) updates->settingsRows([this](const char* help) { describe(help); }, m_fixture);
    }
    // Each installable game: its installed regions (choosing one makes it the one Play starts) and "Install another
    // region", with the regions it installs from and the space each needs.
    void gamesRows(bool gameRunning) {
        section("Game folders");
        ImGui::BeginDisabled(gameRunning || m_job.busy || m_fixture || m_locationDialog.running());
        const auto installFolder = utf8(m_backend.installRoot());
        if (linkRow("Game installation folder", "Browse...", installFolder.c_str(), Icon::Folder))
            chooseLocation(true);
        describe("Folder for future game installations. Existing installations stay where they are and remain playable.");
        if (m_backend.config.contains("install_dir")) {
            if (button("Use default install folder", Tone::Quiet, {0, 0})) {
                try { m_backend.setInstallRoot({}); m_nextRegions = 0; m_locationError.clear(); }
                catch (const std::exception& e) { m_locationError = e.what(); }
            }
        }
        gap(10);
        const auto library = m_backend.isoLibrary();
        const auto libraryLabel = library.empty() ? std::string("Usual folders") : utf8(library);
        if (linkRow("Disc images folder", "Browse...", libraryLabel.c_str(), Icon::Folder))
            chooseLocation(false);
        describe("Scans this folder and subfolders for supported .iso and .chd discs when opening Install.");
        if (!library.empty()) {
            if (button("Clear disc images folder", Tone::Quiet, {0, 0})) {
                try { m_backend.setIsoLibrary({}); m_locationError.clear(); }
                catch (const std::exception& e) { m_locationError = e.what(); }
            }
            std::error_code ec;
            if (!fs::is_directory(library, ec))
                caption("Disc images folder is unavailable. Select a valid folder to scan it.", color::warning);
        }
        ImGui::EndDisabled();
        if (!m_locationError.empty()) {
            if (banner("game-locations-error", m_locationError, BannerTone::Warning)) m_locationError.clear();
        }
        gap(12);
        if (!m_fixture || m_regionsFixture) {
            const double now = ImGui::GetTime();
            if (!m_regionsFixture && now >= m_nextRegions) {
                m_nextRegions = now + 1.0;
                for (GameId g : { GameId::Sly1, GameId::Sly2, GameId::Sly3 }) {
                    m_installs[static_cast<size_t>(g)] = m_backend.installations(g);
                    m_selected[static_cast<size_t>(g)] = m_backend.selectedRegion(g);
                    m_ps3Packs[static_cast<size_t>(g)] = m_backend.ps3HdPack(g);
                }
            }
        }
        for (GameId g : { GameId::Sly1, GameId::Sly2, GameId::Sly3 }) {
            ImGui::PushID(static_cast<int>(g));
            section(cane::game(g).title);
            const auto& installs = m_installs[static_cast<size_t>(g)];
            const auto& selected = m_selected[static_cast<size_t>(g)];
            for (const auto& in : installs) {
                const std::string state = in.outdated ? "Update needed" : !in.current ? "Needs repair"
                                          : installs.size() > 1 && in.region == selected ? "Plays · Ready" : "Ready";
                ImGui::PushID(in.region.c_str());
                const bool chosen = linkRow(in.name().c_str(), state.c_str(), utf8(in.dir).c_str(), installs.size() > 1 ? Icon::ChevronRight : Icon::None);
                ImGui::PopID();
                describe(installs.size() > 1 ? "Choose this region: Play, Verify and Add to Steam use it." : "The installed region.");
                if (chosen && installs.size() > 1 && !gameRunning && !m_fixture) {
                    try {
                        m_backend.setSelectedRegion(g, in.region);
                    } catch (...) {
                    }
                    m_nextRegions = 0;
                }
            }
            if (installs.empty()) {
                ImGui::Indent(px(16));
                caption("Not installed.");
                ImGui::Unindent(px(16));
            }
            ImGui::BeginDisabled(m_job.busy || gameRunning);
            const std::string label = installs.empty() ? std::string("Install ") + cane::game(g).shortName : std::string("Install another region");
            if (linkRow(label.c_str(), nullptr, nullptr, Icon::Plus) && installRegion) installRegion(g);
            ImGui::EndDisabled();
            describe("Opens the install page for a disc image of another region. The installed regions stay.");
            std::string regions, later;
            for (const auto& r : knownRegions(g)) {
                if (r.supported) regions += (regions.empty() ? "" : " · ") + r.name + " " + aboutGb(r.needBytes);
                else later += (later.empty() ? "" : ", ") + r.name;
            }
            ImGui::Indent(px(16));
            caption(("Installs from: " + regions + (later.empty() ? "" : ". Recognized, not supported yet: " + later) + ".").c_str());
            ImGui::Unindent(px(16));
            if (LauncherBackend::ps3HdGame(g) && !installs.empty()) ps3HdRows(g, gameRunning);
            ImGui::PopID();
        }
    }
    // The PS3 HD textures of a game (backend.h importPs3Hd): on / off, import from The Sly Collection (a disc image,
    // a .zip of one, or the extracted disc), with the job's progress and Cancel, and Remove. The cutscenes stay the
    // PS2's (the PS3 movies are not sharper), which a note says.
    void chooseLocation(bool install) {
        if (m_locationDialog.running()) return;
        m_pickInstallLocation = install;
        const auto selected = install ? m_backend.installRoot() : m_backend.isoLibrary();
        const auto initial = filedialog::startFolder({utf8(selected)});
        const char* title = install ? "Choose game installation folder" : "Choose disc images folder";
        if (!m_locationDialog.start(filedialog::Kind::PickFolder, title, initial))
            openLocationPicker();
    }
    void openLocationPicker() {
        const auto selected = m_pickInstallLocation ? m_backend.installRoot() : m_backend.isoLibrary();
        ImGuiFileDialog::Instance()->OpenDialog("game-locations",
            m_pickInstallLocation ? "Choose game installation folder" : "Choose disc images folder",
            nullptr, utf8(filedialog::startFolder({utf8(selected)})), "", 1, nullptr,
            ImGuiFileDialogFlags_Modal);
    }
    void applyLocation(const std::string& folder) {
        try {
            if (m_pickInstallLocation) { m_backend.setInstallRoot(path(folder)); m_nextRegions = 0; }
            else m_backend.setIsoLibrary(path(folder));
            m_locationError.clear();
        } catch (const std::exception& e) {
            m_locationError = e.what();
        }
    }
    void ps3HdRows(GameId g, bool gameRunning) {
        const size_t gi = static_cast<size_t>(g);
        const auto& pack = m_ps3Packs[gi];
        gap(6);
        ImGui::Indent(px(16));
        field("PS3 HD textures");
        ImGui::Unindent(px(16));
        const std::string label = std::string("PS3 HD import (") + cane::game(g).shortName + ")";
        const bool importing = m_job.busy && m_job.label == label;
        ImGui::BeginDisabled(m_job.busy || gameRunning || m_fixture);
        if (pack.installed) {
            bool on = pack.enabled;
            char sub[160];
            std::snprintf(sub, sizeof(sub), "%d textures from The Sly Collection (PS3), %.0f MB%s%s%s", pack.textures, pack.megabytes,
                          pack.png ? ", PNG files" : "", pack.date.empty() ? "" : ", imported ", pack.date.c_str());
            if (toggleRow("Use the PS3 HD textures", &on, sub)) {
                try {
                    m_backend.setPs3HdEnabled(g, on);
                } catch (...) {
                }
                m_nextRegions = 0;
            }
            describe("The redrawn textures of The Sly Collection (PS3), up to 4x sharper, over the original ones. The levels, "
                     "the gameplay and your Mods stay as they are; your own Mods/Textures win. Applies the next time the game starts.");
        }
        bool png = m_backend.ps3HdPng(g);
        if (toggleRow("Extract as PNG files", &png)) {
            try {
                m_backend.setPs3HdPng(g, png);
            } catch (...) {
            }
        }
        describe("The next import writes the textures as PNG files in a Textures folder, to look through or edit, instead of "
                 "one Textures.pak file. The game reads either; the one file loads faster.");
        if (linkRow(pack.installed ? "Import again from a disc image…" : "Import PS3 HD textures (The Sly Collection)…", ".iso / .zip", nullptr, Icon::Disc))
            startPs3Pick(g, false);
        describe("Choose The Sly Collection (PS3): its disc image (.iso) or a .zip of it, read as it is, nothing is unpacked. "
                 "SafeHouse then matches its textures to your installed game: Sly 1 about 2 minutes, Sly 2 about 6.");
        if (linkRow("Import from the extracted disc…", "folder", nullptr, Icon::Folder)) startPs3Pick(g, true);
        describe("Choose the extracted disc: the folder with PS3_GAME in it, PS3_GAME, or PS3_GAME/USRDIR.");
        if (pack.installed) {
            if (linkRow("Remove the PS3 HD textures", nullptr, nullptr, Icon::Trash)) {
                try {
                    m_backend.removePs3Hd(g);
                } catch (...) {
                }
                m_nextRegions = 0;
            }
            describe("Deletes the imported pack. The game's own textures are not touched.");
        }
        ImGui::EndDisabled();
        if (importing) {
            std::string stage;
            float progress = 0;
            {
                std::lock_guard lock(m_job.mutex);
                stage = stageLabel(m_job.stage);
                if (!m_job.item.empty()) stage += " · " + m_job.item;
                progress = m_job.progress;
            }
            ImGui::Indent(px(16));
            char text[320];
            std::snprintf(text, sizeof(text), "%s · %d %%", stage.empty() ? "Starting" : stage.c_str(), int(progress * 100));
            caption(text, color::text2);
            if (button("Cancel import", Tone::Secondary, { px(190), 0 }, Icon::Close)) m_job.cancel = true;
            ImGui::Unindent(px(16));
        }
        ImGui::Indent(px(16));
        caption(g == GameId::Sly1
                    ? "Cutscenes stay the PS2's: the PS3 movies are the same 640x480 picture at 30 fps, re-encoded in Bink, "
                      "so they would not be sharper."
                    : "Cutscenes stay the PS2's: the PS3 movies are the same 640x480 picture, re-encoded in Bink, so they "
                      "would not be sharper.");
        ImGui::Unindent(px(16));
    }
    void startPs3Pick(GameId g, bool folder) {
        m_ps3Game = g;
        m_ps3PickFolder = folder;
        const char* title = folder ? "Choose the extracted Sly Collection (PS3)" : "Choose The Sly Collection (PS3) disc image";
        const fs::path start = filedialog::startFolder({ m_backend.config.value("ps3_browse_dir", std::string{}) });
        if (m_ps3Dialog.running()) return;
        if (!m_ps3Dialog.start(folder ? filedialog::Kind::PickFolder : filedialog::Kind::OpenPs3, title, start))
            openPs3Picker(folder);
    }
    void openPs3Picker(bool folder) {
        m_ps3PickFolder = folder;
        ImGuiFileDialog::Instance()->OpenDialog("ps3src", folder ? "Choose the extracted Sly Collection (PS3)" : "Choose The Sly Collection (PS3) disc image",
                                                folder ? nullptr : ".iso,.zip,.ISO,.ZIP",
                                                utf8(filedialog::startFolder({ m_backend.config.value("ps3_browse_dir", std::string{}) })), "", 1,
                                                nullptr, ImGuiFileDialogFlags_Modal);
    }
    void startPs3Import(GameId g, const std::string& source) {
        try {
            m_backend.config["ps3_browse_dir"] = utf8(path(source).parent_path());
            persistLauncher();
        } catch (...) {
        }
        const std::string label = std::string("PS3 HD import (") + cane::game(g).shortName + ")";
        const bool png = m_backend.ps3HdPng(g);
        m_job.start(label, [this, g, source, png](const auto& log) { m_backend.importPs3Hd(g, source, png, m_job.cancel, log); });
    }
    void advancedRows(bool gameRunning) {
        bool debug = m_debug;
        if (toggleRow("Debug mode", &debug)) {
            m_debug = debug;
            writeDebugMode(debug);
        }
        // (one switch for everything debug: Debug Mode.ini, cane::debugMode(); the games' debug keys, Sly 2's debug menu
        //  and PC Extras, Sly 1's developer debug mode. Backend::play copies it to Sly 2's / Sly 3's own user folder.)
        describe(m_game == GameId::Sly2
                     ? "Turns on the debug keys and the debug menu (F10 / R1+Select) with PC Extras. F1 free camera, "
                       "F2 Paris, F3 next world, F5 save, F6 unlock all, F7 infinite health, F9 finish the job. "
                       "For testing or reporting a problem."
                     : m_game == GameId::Sly1
                     ? "Turns on the debug keys (F3 opens the debug window) plus the game's debug features. "
                       "For testing or reporting a problem."
                     : "Turns on the debug keys plus the game's debug features. For testing or reporting a problem.");
        bool save = logging ? logging() : false;
        if (toggleRow("Save logs", &save) && setLogging && !m_fixture) setLogging(save);
        describe("Off by default. When on, launcher and game output is saved to launcher.log in the log folder, for bug reports.");
        bool logs = showLogs ? *showLogs : false;
        if (toggleRow("Show activity log", &logs)) {
            if (showLogs) *showLogs = logs;
            m_backend.config["show_logs"] = logs;
            persistLauncher();
        }
        describe("Shows the install and build log drawer. The View button also opens it.");
        if (linkRow("Save folder", nullptr, nullptr, Icon::Folder)) openSafe(m_backend.userDir(m_game));
        m_saveFolderHelp = std::string("Opens the folder with your ") + cane::game(m_game).shortName + " saves and settings.";
        describe(m_saveFolderHelp.c_str());
        if (linkRow("Log folder", nullptr, nullptr, Icon::Folder)) openSafe(m_backend.locations.logs);
        describe("Opens the folder with the launcher and game logs.");
        const bool custom = m_backend.config.contains("runtime_dir");
#ifndef CANE_LAUNCHER_SOURCE_BUILD
        // The packages players download run the bundled programs (built by CI): the section is only the way back from
        // a runtime an older launcher built from source.
        if (!custom) return;
#endif
        section("Custom runtime");
        ImGui::Indent(px(16));
        caption(custom ? ("Using a runtime built from source: " + utf8(m_backend.tools)).c_str() : "Using the runtime bundled with SafeHouse.",
                color::text2);
        ImGui::BeginDisabled(m_job.busy || gameRunning);
#ifdef CANE_LAUNCHER_SOURCE_BUILD
        caption("Building needs CMake and a C++20 compiler, and may download dependencies.");
        gap(14);
        field("Source checkout");
        const float browseW = px(120);
        inputField("##source", m_source, sizeof(m_source), launcher::Charset::Free, "SafeHouse source folder",
                   ImGui::GetContentRegionAvail().x - browseW - px(28));
        ImGui::SameLine(0, px(12));
        if (button("Browse…##source", Tone::Secondary, { browseW, 0 }) && !m_sourceDialog.running() &&
            !m_sourceDialog.start(filedialog::Kind::PickFolder, "Choose the SafeHouse source folder", filedialog::startFolder({ m_source })))
            openSourcePicker();
        gap(12);
        ImGui::BeginDisabled(!m_source[0]);
        if (button("Build and use", Tone::Primary, { px(170), 0 })) {
            const std::string selected = m_source;
            m_job.start("Source build", [this, selected](const auto& log) { m_backend.buildSource(path(selected), m_job.cancel, log); });
            if (showLogs) *showLogs = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, px(12));
#else
        gap(14);
#endif
        ImGui::BeginDisabled(!custom);
        if (button("Use bundled runtime", Tone::Secondary, { px(210), 0 })) {
            m_backend.tools = executableDirectory();
            m_backend.config.erase("runtime_dir");
            persistLauncher();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::Unindent(px(16));
    }
    void openSafe(const fs::path& folder) {
        try {
            openFolder(folder);
        } catch (const std::exception&) {
        }
    }

public:
    static bool defaultFullscreen() {
        const char* deck = std::getenv("SteamDeck");
        const char* gamescope = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
        const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
        return (deck && std::string(deck) == "1") || (gamescope && *gamescope) || (desktop && std::string(desktop) == "gamescope");
    }

private:
    LauncherBackend& m_backend;
    Job& m_job;
    GLFWwindow* m_window;
    settings::SystemSettings m_settings = settings::defaults();
    sly2input::Profile m_actions = sly2input::defaults();
    fs::file_time_type m_actionMtime{};
    int m_actionDevice = 0, m_actionGroup = 0;
    settings::LoadResult m_result = settings::LoadResult::Missing;
    uint32_t m_version = 0;
    fs::file_time_type m_mtime{};
    double m_nextPoll = 0, m_savedAt = -10, m_captureStarted = 0, m_captureCooldown = 0;
    int m_maxSamples = 0, m_captureAction = -1;
    bool m_captureController = false, m_captureArmed = false, m_saveOk = true, m_volumeDirty = false, m_debug = false;
    bool m_focusRows = false;
    bool m_paneFocused = false, m_focusRail = false, m_scrollTop = false, m_fixture = false;
    std::string m_help, m_captureNote, m_saveFolderHelp;
    GameId m_game = GameId::Sly1;
    char m_source[4096]{};
    std::array<std::vector<Installation>, kGameCount> m_installs;
    std::array<std::string, kGameCount> m_selected;
    double m_nextRegions = 0;
    bool m_regionsFixture = false;
    std::array<LauncherBackend::Ps3HdPack, kGameCount> m_ps3Packs{};
    filedialog::Async m_locationDialog;
    bool m_pickInstallLocation = true;
    std::string m_locationError;
    filedialog::Async m_ps3Dialog;
    GameId m_ps3Game = GameId::Sly2;
    bool m_ps3PickFolder = false;
};
} // namespace cane::ui
