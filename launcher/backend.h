#pragma once
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

#include "install.h"
#include "process.h"
#include "regions.h"
namespace cane {
// One installed region of a game (regions.h). Storage: the first region installed keeps the game's original folder
// installations/<game> (so the installs made before regions, and older launchers, keep working, read in place), each
// further region gets installations/<game>-<region> beside it. The region is read from the folder's
// installation.json, never from the folder name. Nothing is ever moved or renamed.
struct Installation {
    GameId game = GameId::Sly1;
    std::string region;   // "usa", "europe", ... ("" when the manifest cannot be read)
    std::string release;  // "Europe v2.01"
    fs::path dir;
    bool current = false;   // installed with this launcher's asset format: playable
    bool outdated = false;  // a valid installation in an older asset format: an update changed it, re-extract
    int assetVersion = 0, wantedVersion = 0;
    std::string iso;        // the disc image it was installed from (its manifest)
    std::string name() const { return release.empty() ? regionName(region) : release; }
};
// The asset version this launcher installs for a game.
inline int currentAssetVersion(GameId game) {
    if (game == GameId::Sly1) return assetVersion;
    if (const DiscImageInstall* d = discImageInstall(game)) return d->assetVersion;
    return 0;
}
// The region a manifest was installed from (old manifests have no "region": Sly 1 asset version 1 and the first
// Sly 2 / Sly 3 installs were USA only).
inline std::pair<std::string, std::string> manifestRegion(const json& m, GameId game) {
    auto text = [&](const char* key) { return m.contains(key) && m.at(key).is_string() ? m.at(key).get<std::string>() : std::string{}; };
    if (game == GameId::Sly1) {
        if (const Sly1Disc* d = sly1DiscFromSha256(text("disc_sha256"))) return {regionKey(d->region), regionName(regionKey(d->region))};
        if (const Sly1Disc* d = sly1DiscFromBootFile(text("disc"))) return {regionKey(d->region), regionName(regionKey(d->region))};
    } else if (const DiscImageInstall* di = discImageInstall(game)) {
        if (const DiscVariant* v = di->variantBySha1(text("disc_sha1"))) return {regionKey(v->region), v->region};
    }
    if (!text("region").empty()) {
        const auto key = regionKey(text("region"));
        return {key, text("region") == "NTSC-U" || text("region") == "PAL" || text("region").rfind("NTSC", 0) == 0 ? regionName(key) : text("region")};
    }
    if (auto r = regionByBootFile(game, text("disc"))) return {r->key, r->name};
    return {"usa", "USA"};
}
inline Installation readInstallation(const fs::path& dir, GameId game) {
    Installation in;
    in.game = game;
    in.dir = dir;
    in.wantedVersion = currentAssetVersion(game);
    try {
        const json m = readJson(dir / "installation.json");
        if (manifestGame(m) != game) return in;
        std::tie(in.region, in.release) = manifestRegion(m, game);
        in.current = manifestCurrent(m, game);
        if (m.contains("asset_version") && m.at("asset_version").is_number_integer()) in.assetVersion = m.at("asset_version").get<int>();
        in.outdated = !in.current && in.assetVersion > 0 && in.assetVersion < in.wantedVersion;
        if (m.contains("iso_path") && m.at("iso_path").is_string()) in.iso = m.at("iso_path").get<std::string>();
    } catch (...) {
    }
    return in;
}
inline std::string executableName(const std::string& name) {
#ifdef _WIN32
    return name + ".exe";
#else
    return name;
#endif
}
struct LauncherBackend {
    UserPaths locations = userPaths();
    fs::path customInstallRoot;
    fs::path tools = executableDirectory();
    json config = json::object();
    LauncherBackend() {
        for (auto& p :
             {locations.data.parent_path(), locations.user, locations.config, locations.logs})
            fs::create_directories(p);
        {
            FileLock migrationLock(locations.config / "profile-migration.lock");
            importDirectoryIfEmpty(runtimeStorage(locations) / "Saves", locations.user / "Saves");
            importDirectoryIfEmpty(legacyLogDirectory(locations), locations.logs);
            importDirectoryIfEmpty(executableDirectory() / "Saves", locations.user / "Saves");
        }
        fs::create_directories(locations.user / "Saves");
        auto file = locations.config / "launcher.json";
        if (fs::exists(file)) {
            try {
                config = readJson(file);
                if (!config.is_object()) throw std::runtime_error("Invalid launcher configuration");
                for (auto key : {"runtime_dir", "iso_path"})
                    if (config.contains(key) && !config.at(key).is_string())
                        throw std::runtime_error("Invalid launcher configuration field");
                // An unexpected value only disables logging; keep the rest.
                if (config.contains("logging") && !config.at("logging").is_boolean())
                    config.erase("logging");
                // Launcher preferences fall back to their defaults one by one.
                for (auto key : {"sly2_iso_path", "sly3_iso_path", "last_game", "sly2_runtime", "browse_dir", "skipped_update", "install_dir", "iso_library_dir"})
                    if (config.contains(key) && !config.at(key).is_string()) config.erase(key);
                // "sly2_wide_backgrounds" (Sly 2's "Widescreen backgrounds", removed 2026-11-05) is ignored and kept as it is.
                for (auto key : {"show_logs", "start_fullscreen", "sly2_native_preview", "sly3_notice_hidden", "check_updates"})
                    if (config.contains(key) && !config.at(key).is_boolean()) config.erase(key);
                for (auto key : {"ui_scale", "prompt_style"})
                    if (config.contains(key) && !config.at(key).is_number()) config.erase(key);
                // {"sly1": "europe"}: the region last chosen / played; {"sly1": {"europe": ISO}}: the disc images.
                for (auto key : {"regions", "region_isos"})
                    if (config.contains(key) && !config.at(key).is_object()) config.erase(key);
                if (config.contains("install_roots")) {
                    if (!config["install_roots"].is_array()) config.erase("install_roots");
                    else {
                        json valid = json::array();
                        for (const auto& root : config["install_roots"])
                            if (root.is_string() && path(root.get<std::string>()).is_absolute())
                                valid.push_back(root);
                        config["install_roots"] = std::move(valid);
                    }
                }
            } catch (...) {
                auto backup = file;
                backup += ".broken";
                std::error_code ec;
                fs::rename(file, backup, ec);
                config = json::object();
            }
        }
        // Only game assets use the optional location. Saves, settings, logs and runtime builds stay
        // in their existing profile, including portable installations.
        if (config.contains("install_dir")) {
            const auto dir = path(config.at("install_dir").get<std::string>());
            if (dir.is_absolute() && dir != dir.root_path()) customInstallRoot = dir.lexically_normal();
        }
        if (config.contains("runtime_dir"))
            tools = locations.portable ? runtimeStorage(locations) / "runtime" / "bin"
                                       : path(config.at("runtime_dir").get<std::string>());
        // Recover activation interrupted between its two renames.
        for (const auto& target : {locations.data, runtimeStorage(locations) / "runtime"}) {
            auto previous = target;
            previous += ".previous";
            if (!fs::exists(target) && fs::exists(previous)) {
                try {
                    InstallationTransaction recovery(target);
                } catch (...) {
                }
            }
        }
    }
    // Log files are opt-in: they are only written for bug reports.
    bool loggingEnabled() const { return config.value("logging", false); }
    void setLoggingEnabled(bool enabled) {
        if (enabled)
            config["logging"] = true;
        else
            config.erase("logging");
        saveConfig();
    }
    bool sly2NativePreviewEnabled() const { return config.value("sly2_native_preview", false); }
    void setSly2NativePreviewEnabled(bool enabled) {
        if (sly2NativePreviewEnabled() == enabled) return;
        const auto previous = config;
        config["sly2_native_preview"] = enabled;
        try {
            saveConfig();
        } catch (...) {
            config = previous;
            throw;
        }
    }
    void saveConfig() { writeJson(locations.config / "launcher.json", config); }
    // Empty selects the original OS/portable default. Existing game installations
    // remain where they are and are still discovered, regardless of the selected root.
    fs::path installRoot() const {
        return customInstallRoot.empty() ? locations.data.parent_path() : customInstallRoot;
    }
    void setInstallRoot(const fs::path& requested) {
        fs::path next;
        if (!requested.empty()) {
            next = fs::absolute(requested).lexically_normal();
            if (next == next.root_path())
                throw std::runtime_error("Choose a folder rather than the root of a drive");
            fs::create_directories(next);
            if (!fs::is_directory(next)) throw std::runtime_error("Installation path is not a folder");
            // Test actual write access rather than trusting filesystem permission bits.
            const auto probe = next / ".safehouse-write-test";
            if (fs::exists(probe)) throw std::runtime_error("Cannot test write access to installation folder");
            {
                std::ofstream out(probe, std::ios::binary | std::ios::trunc);
                if (!out || !(out << "safehouse")) throw std::runtime_error("Installation folder is not writable");
            }
            fs::remove(probe);
        }
        const auto oldConfig = config;
        // Previously selected custom roots remain discoverable even after switching
        // to another drive or resetting to the default. Never move existing files.
        if (!customInstallRoot.empty() && customInstallRoot != next) {
            if (!config.contains("install_roots") || !config["install_roots"].is_array())
                config["install_roots"] = json::array();
            const auto previous = utf8(customInstallRoot);
            if (std::find(config["install_roots"].begin(), config["install_roots"].end(), previous) == config["install_roots"].end())
                config["install_roots"].push_back(previous);
        }
        if (next.empty() || next == locations.data.parent_path()) config.erase("install_dir");
        else config["install_dir"] = utf8(next);
        try { saveConfig(); }
        catch (...) { config = oldConfig; throw; }
        customInstallRoot = config.contains("install_dir") ? next : fs::path{};
    }
    fs::path isoLibrary() const {
        if (!config.contains("iso_library_dir") || !config.at("iso_library_dir").is_string()) return {};
        const auto dir = path(config.at("iso_library_dir").get<std::string>());
        return dir.is_absolute() ? dir : fs::path{};
    }
    void setIsoLibrary(const fs::path& requested) {
        fs::path next;
        if (!requested.empty()) {
            next = fs::absolute(requested).lexically_normal();
            if (!fs::is_directory(next)) throw std::runtime_error("Disc images folder does not exist");
        }
        const auto oldConfig = config;
        if (next.empty()) config.erase("iso_library_dir");
        else config["iso_library_dir"] = utf8(next);
        try { saveConfig(); }
        catch (...) { config = oldConfig; throw; }
    }
    // "Don't show again" on the Sly 3 early-development notice before Play (launcher.json "sly3_notice_hidden").
    bool sly3NoticeHidden() const { return config.value("sly3_notice_hidden", false); }
    void hideSly3Notice() {
        config["sly3_notice_hidden"] = true;
        saveConfig();
    }
    // Manifests are parsed again only when they change (Sly 1's lists every file: the pages ask every second).
    Installation cachedInstallation(const fs::path& dir, GameId game) const {
        std::error_code ec;
        const auto file = dir / "installation.json";
        const auto stamp = fs::last_write_time(file, ec);
        const auto size = ec ? 0 : fs::file_size(file, ec);
        std::lock_guard lock(m_cacheMutex);
        for (auto& c : m_cache)
            if (c.file == file && c.stamp == stamp && c.size == size && c.value.game == game) return c.value;
        Installation in = readInstallation(dir, game);
        std::erase_if(m_cache, [&](const auto& c) { return c.file == file; });
        m_cache.push_back({file, stamp, size, in});
        return in;
    }
    struct CacheEntry {
        fs::path file;
        fs::file_time_type stamp;
        uintmax_t size;
        Installation value;
    };
    mutable std::mutex m_cacheMutex;
    mutable std::vector<CacheEntry> m_cache;
    // The game's original folder (installations/<game>): its first installed region.
    fs::path baseDir(GameId game) const { return installRoot() / cane::game(game).key; }
    fs::path legacyBaseDir(GameId game) const { return gameDataDirectory(locations, game); }
    // Every installed region of a game: the original folder, then installations/<game>-<region> in the order of
    // knownRegions(). A folder whose installation.json is missing is not an installation; one that cannot be read
    // still is (it needs repair). Staging / previous folders and locks use a '.' and are never listed.
    std::vector<Installation> installations(GameId game) const {
        std::vector<Installation> out;
        if (!installable(game)) return out;
        // Prefer the configured folder, but keep reading the original default
        // so switching drives never hides existing games or saves.
        const fs::path preferred = baseDir(game);
        const fs::path legacy = legacyBaseDir(game);
        auto collect = [&](const fs::path& base) {
            std::error_code ec;
            if (fs::is_regular_file(base / "installation.json", ec)) {
                auto in = cachedInstallation(base, game);
                if (std::none_of(out.begin(), out.end(), [&](const Installation& o) { return o.region == in.region; }))
                    out.push_back(std::move(in));
            }
            const std::string prefix = std::string(cane::game(game).key) + "-";
            for (const auto& r : knownRegions(game)) {
                const auto dir = base.parent_path() / (prefix + r.key);
                if (!fs::is_regular_file(dir / "installation.json", ec)) continue;
                Installation in = cachedInstallation(dir, game);
                if (in.region.empty()) in.region = r.key, in.release = r.name;
                if (std::none_of(out.begin(), out.end(), [&](const Installation& o) { return o.region == in.region; }))
                    out.push_back(std::move(in));
            }
        };
        collect(preferred);
        if (config.contains("install_roots") && config["install_roots"].is_array()) {
            for (const auto& stored : config["install_roots"]) {
                if (!stored.is_string()) continue;
                const auto root = path(stored.get<std::string>());
                if (!root.is_absolute()) continue;
                const auto previous = root / cane::game(game).key;
                if (previous != preferred && previous != legacy) collect(previous);
            }
        }
        if (preferred != legacy) collect(legacy);
        if (!out.empty() && out[0].region.empty()) out[0].release = "Unknown region";
        return out;
    }
    std::optional<Installation> installation(GameId game, const std::string& region) const {
        for (auto& in : installations(game))
            if (in.region == region) return in;
        return std::nullopt;
    }
    // The region the launcher plays: the one chosen last (launcher.json "regions"), else the first playable one.
    std::string selectedRegion(GameId game) const {
        const auto all = installations(game);
        const char* key = cane::game(game).key;
        if (config.contains("regions") && config.at("regions").contains(key) && config.at("regions").at(key).is_string()) {
            const auto chosen = config.at("regions").at(key).get<std::string>();
            for (const auto& in : all)
                if (in.region == chosen) return chosen;
        }
        for (const auto& in : all)
            if (in.current) return in.region;
        return all.empty() ? std::string{} : all[0].region;
    }
    void setSelectedRegion(GameId game, const std::string& region) {
        const char* key = cane::game(game).key;
        if (config.contains("regions") && config["regions"].contains(key) && config["regions"][key] == region) return;
        if (!config.contains("regions")) config["regions"] = json::object();
        config["regions"][cane::game(game).key] = region;
        saveConfig();
    }
    // Where a region installs: its existing folder, the original folder when the game has no installation there,
    // else installations/<game>-<region>.
    fs::path regionDir(GameId game, const std::string& region) const {
        if (auto in = installation(game, region)) return in->dir;
        std::error_code ec;
        const fs::path base = baseDir(game);
        if (!fs::exists(base / "installation.json", ec)) return base;
        return base.parent_path() / (std::string(cane::game(game).key) + "-" + region);
    }
    // The selected region's folder (the game's original folder when nothing is installed).
    fs::path dataDir(GameId game) const {
        const auto region = selectedRegion(game);
        if (auto in = installation(game, region)) return in->dir;
        return baseDir(game);
    }
    fs::path userDir(GameId game) const { return gameUserDirectory(locations, game); }
    // The game selected in the sidebar when the launcher last closed.
    GameId lastGame() const {
        if (config.contains("last_game"))
            if (auto id = gameFromKey(config.at("last_game").get<std::string>())) return *id;
        return GameId::Sly1;
    }
    void setLastGame(GameId game) {
        if (lastGame() == game && (config.contains("last_game") || game == GameId::Sly1)) return;
        config["last_game"] = cane::game(game).key;
        saveConfig();
    }
    // The game's runtime program is next to the launcher (or in the custom
    // runtime folder).
    bool runtimeAvailable(GameId game) const {
        const char* runtime = cane::game(game).runtime;
        std::error_code ec;
        return runtime && fs::is_regular_file(tools / executableName(runtime), ec);
    }
    // The installation manifest matches this launcher's asset format.
    bool assetsInstalled(GameId game) const {
        auto in = installation(game, selectedRegion(game));
        return in && in->current;
    }
    // Files from an older or damaged installation are present.
    bool needsRepair(GameId game) const {
        auto in = installation(game, selectedRegion(game));
        return in && !in->current;
    }
    // The selected installation is valid but in an older asset format: an update changed the game's file format.
    bool needsReextract(GameId game) const {
        auto in = installation(game, selectedRegion(game));
        return in && in->outdated;
    }
    fs::path binary(const std::string& name) const {
        auto p = tools / executableName(name);
        if (!fs::is_regular_file(p))
            throw std::runtime_error("Missing bundled program: " + utf8(p));
        return p;
    }
    // Ready to play: current assets and the runtime program. Sly 1 by default.
    bool installed(GameId game = GameId::Sly1) const {
        return assetsInstalled(game) && runtimeAvailable(game);
    }
    static const char* isoKey(GameId game) {
        return game == GameId::Sly1 ? "iso_path" : game == GameId::Sly3 ? "sly3_iso_path" : "sly2_iso_path";
    }
    // The disc image a region was installed from: launcher.json "region_isos", the pre-region keys (for the game's
    // original folder), then the installation's manifest. Region "" = the selected one.
    std::string rememberedIso(GameId game = GameId::Sly1, std::string region = {}) const {
        if (region.empty()) region = selectedRegion(game);
        const char* key = cane::game(game).key;
        if (config.contains("region_isos") && config.at("region_isos").contains(key) && config.at("region_isos").at(key).is_object() &&
            config.at("region_isos").at(key).contains(region) && config.at("region_isos").at(key).at(region).is_string())
            return config.at("region_isos").at(key).at(region).get<std::string>();
        const auto in = installation(game, region);
        if ((!in || in->dir == baseDir(game)) && config.contains(isoKey(game))) return config.at(isoKey(game)).get<std::string>();
        return in ? in->iso : std::string{};
    }
    void install(const std::string& iso, const std::atomic<bool>& cancel, const LogFunction& log) {
        install(iso, GameId::Sly1, cancel, log);
    }
    // Installs a disc image into its region's folder (regionDir): the disc decides the region, so another region's
    // disc installs beside the existing ones and the same region's disc replaces (re-extracts) that one.
    void install(const std::string& iso, GameId game, const std::atomic<bool>& cancel, const LogFunction& log) {
        if (!installable(game))
            throw std::runtime_error(std::string(cane::game(game).title) + " cannot be installed yet");
        const DiscCheck check = checkDisc(path(iso), game);
        if (!check.ok()) throw std::runtime_error(check.message);
        const std::string region = check.region->key;
        const auto target = regionDir(game, region);
        log("Disc: " + check.label() + " -> " + utf8(target));
        std::vector<std::string> arguments{utf8(binary("cane-extractor")), "--iso", iso, "--output", utf8(target),
                                           "--non-interactive", "--progress=jsonl"};
        if (game != GameId::Sly1) arguments.insert(arguments.begin() + 1, {"--game", cane::game(game).key});
        try {
            checkedProcess(arguments, tools, cancel, log);
        } catch (...) {
            // The extractor may be terminated without running destructors.
            // Acquire its released lock before discarding incomplete staging.
            try {
                InstallationTransaction cleanup(target);
            } catch (...) {
            }
            throw;
        }
        if (target == baseDir(game)) config[isoKey(game)] = iso;
        if (!config.contains("region_isos")) config["region_isos"] = json::object();
        if (!config["region_isos"].contains(cane::game(game).key) || !config["region_isos"][cane::game(game).key].is_object())
            config["region_isos"][cane::game(game).key] = json::object();
        config["region_isos"][cane::game(game).key][region] = iso;
        if (!config.contains("regions")) config["regions"] = json::object();
        config["regions"][cane::game(game).key] = region;
        saveConfig();
    }
    // Removes the selected region's installed files. Saves and settings are kept.
    void uninstall(GameId game) {
        const auto target = fs::absolute(dataDir(game));
        auto lock = target;
        lock += ".lock";
        FileLock guard(lock);
        for (const char* suffix : {"", ".previous", ".staging"}) {
            auto p = target;
            p += suffix;
            fs::remove_all(p);
        }
    }
    void play(const std::atomic<bool>& cancel, const LogFunction& log) { play(GameId::Sly1, cancel, log); }
    // Settings > Advanced > Debug mode: one Debug Mode.ini in the profile root (Sly 1's user folder). Sly 2 and Sly 3 run
    // with --user-dir <profile>/KEY, where cane::debugMode() looks, so each start copies the switch there (the root
    // file wins; a missing root file is off).
    fs::path debugModeFile() const { return locations.user / "Debug Mode.ini"; }
    void syncDebugMode(GameId game) const {
        const fs::path target = userDir(game) / "Debug Mode.ini";
        if (target == debugModeFile()) return;
        const bool enabled = debugModeIn(debugModeFile());
        std::ofstream(target) << "[Debug]\nDebug Mode=" << (enabled ? 1 : 0) << '\n';
    }
    // Runtime contract (all games): <runtime> --data-dir <installations/KEY>
    // --user-dir <profile>[/KEY], started in the runtime's own folder.
    // The region played is the selected one (`region`, when given, is selected first and so remembered).
    void play(GameId game, const std::atomic<bool>& cancel, const LogFunction& log, const std::string& region = {}) {
        if (!region.empty()) {
            if (!installation(game, region))
                throw std::runtime_error(std::string(cane::game(game).shortName) + " has no " + regionName(region) + " installation");
            setSelectedRegion(game, region);
        }
        if (needsReextract(game))
            throw std::runtime_error("An update changed the format of " + std::string(cane::game(game).shortName) +
                                     "'s game files. Open SafeHouse and re-extract your disc to keep playing");
        if (game == GameId::Sly1 && !installed())
            throw std::runtime_error("Install the game with this launcher version first");
        if (game != GameId::Sly1) {
            if (!assetsInstalled(game))
                throw std::runtime_error("Install the game with this launcher version first");
            if (!runtimeAvailable(game))
                throw std::runtime_error(std::string(cane::game(game).title) + " is still in development");
        }
        fs::create_directories(userDir(game));
        syncDebugMode(game);
        // Sly 2 runs the native port (Sly2-Release, from its boot sequence). The emulated runtime and its
        // "sly2_runtime" / "sly2_native_preview" options are gone; the keys are ignored if present. So is
        // "sly2_wide_backgrounds" (the "Widescreen backgrounds" toggle, removed 2026-11-05 as redundant: Sly2-Release's
        // widescreen backgrounds are always on; --wide-backgrounds 0 / SLY2N_WIDEBG=0 still turn them off by hand).
        const std::string runtime = cane::game(game).runtime;
        std::vector<std::string> arguments{utf8(binary(runtime)), "--data-dir", utf8(dataDir(game)),
                                           "--user-dir", utf8(userDir(game))};
        for (auto& a : ps3HdArguments(game)) arguments.push_back(a);
        checkedProcess(arguments, tools, cancel, log);
    }
    // Sly 2's texture dump (Textures/ in the installation, MODDING.md): the converter writes it at install since
    // 2026-11-04; an installation from before has none, and "Export textures" writes it in place (no re-extract).
    bool texturesMissing(GameId game) const {
        if (game != GameId::Sly2 || !assetsInstalled(game)) return false;
        std::error_code error;
        return !fs::is_directory(dataDir(game) / "Textures", error);
    }
    void exportTextures(GameId game, const std::atomic<bool>& cancel, const LogFunction& log) {
        if (game != GameId::Sly2 || !assetsInstalled(game))
            throw std::runtime_error("Install the game with this launcher version first");
        fs::create_directories(userDir(game));
        // (Sly2-Release --export-textures: Textures.partial, renamed when complete; "textures I N WORLD" lines)
        checkedProcess({utf8(binary(cane::game(game).runtime)), "--data-dir", utf8(dataDir(game)), "--user-dir",
                        utf8(userDir(game)), "--export-textures"},
                       tools, cancel, [&](const std::string& line) {
                           size_t done = 0, total = 0;
                           char item[256] = {};
                           if (std::sscanf(line.c_str(), "textures %zu %zu %255s", &done, &total, item) >= 2)
                               log(json({{"stage", "textures"}, {"completed", done}, {"total", total}, {"item", item}}).dump());
                           else
                               log(line);
                       });
    }
    // ---- The PS3 HD textures: The Sly Collection's (PS3) redrawn textures as a texture pack for
    // Sly 1 and Sly 2. The game program builds it (--ps3-hd-textures SOURCE OUT: the extracted disc, its .iso or a
    // .zip of that, streamed) into <user dir>/PS3 HD/Textures.pak (or, to edit them, PS3 HD/Textures, the PNG files:
    // --ps3-hd-png), named after the installed textures they replace. The game loads it under the player's own Mods
    // (--texture-pack) while it is turned on (launcher.json "ps3_hd").
    static bool ps3HdGame(GameId game) { return game == GameId::Sly1 || game == GameId::Sly2; }
    fs::path ps3HdDir(GameId game) const { return userDir(game) / "PS3 HD"; }
    struct Ps3HdPack {
        bool installed = false, enabled = false;
        bool png = false;   // (the PNG files in a Textures folder, not Textures.pak)
        int textures = 0;
        double megabytes = 0;
        std::string source, date;
    };
    Ps3HdPack ps3HdPack(GameId game) const {
        Ps3HdPack pack;
        if (!ps3HdGame(game)) return pack;
        std::error_code ec;
        // (a Textures folder: imported as PNG files, or before Textures.pak)
        pack.png = !fs::is_regular_file(ps3HdDir(game) / "Textures.pak", ec);
        if (pack.png && !fs::is_directory(ps3HdDir(game) / "Textures", ec)) return pack;
        pack.installed = true;
        try {
            const json m = readJson(ps3HdDir(game) / "pack.json");
            pack.textures = m.value("textures", 0);
            pack.megabytes = m.value("megabytes", 0.0);
            pack.source = m.value("source", std::string{});
            pack.date = m.value("date", std::string{});
        } catch (...) {
        }
        pack.enabled = !(config.contains("ps3_hd") && config.at("ps3_hd").is_object() && config.at("ps3_hd").contains(cane::game(game).key) &&
                         config.at("ps3_hd").at(cane::game(game).key) == false);
        return pack;
    }
    void setPs3HdEnabled(GameId game, bool enabled) {
        if (!config.contains("ps3_hd") || !config.at("ps3_hd").is_object()) config["ps3_hd"] = json::object();
        if (enabled) config["ps3_hd"].erase(cane::game(game).key);
        else config["ps3_hd"][cane::game(game).key] = false;
        saveConfig();
    }
    // The next import's form: the PNG files (launcher.json "ps3_hd_png"), else Textures.pak
    bool ps3HdPng(GameId game) const {
        const auto it = config.find("ps3_hd_png");
        return it != config.end() && it->is_object() && it->value(cane::game(game).key, false);
    }
    void setPs3HdPng(GameId game, bool png) {
        if (!config.contains("ps3_hd_png") || !config.at("ps3_hd_png").is_object()) config["ps3_hd_png"] = json::object();
        if (png) config["ps3_hd_png"][cane::game(game).key] = true;
        else config["ps3_hd_png"].erase(cane::game(game).key);
        saveConfig();
    }
    void removePs3Hd(GameId game) {
        std::error_code ec;
        for (const char* suffix : {"", ".staging", ".previous"}) {
            auto p = ps3HdDir(game);
            p += suffix;
            fs::remove_all(p, ec);
        }
    }
    // Imports the pack from a disc source (png: as PNG files); the old pack (if any) is replaced only when the new one is
    // complete. The game program's "ps3hd: error: ..." line becomes the error shown.
    void importPs3Hd(GameId game, const std::string& source, bool png, const std::atomic<bool>& cancel, const LogFunction& log) {
        if (!ps3HdGame(game)) throw std::runtime_error("The PS3 HD textures are for Sly 1 and Sly 2");
        if (!assetsInstalled(game)) throw std::runtime_error("Install " + std::string(cane::game(game).shortName) + " first: the HD textures replace its installed ones");
        if (!runtimeAvailable(game)) throw std::runtime_error(std::string(cane::game(game).title) + " is still in development");
        std::error_code ec;
        fs::create_directories(userDir(game), ec);
        if (const auto space = fs::space(userDir(game), ec); !ec && space.available < (2ull << 30))
            throw std::runtime_error("Not enough free disk space: the PS3 HD textures need about 1 GB (Sly 1: 0.6 GB, Sly 2: 0.3 GB)");
        auto staging = ps3HdDir(game);
        staging += ".staging";
        fs::remove_all(staging, ec);
        std::string error, summary, lastItem;
        auto run = [&] {
            std::vector<std::string> arguments{utf8(binary(cane::game(game).runtime)), "--data-dir", utf8(dataDir(game)), "--user-dir",
                                               utf8(userDir(game)), "--ps3-hd-textures", source, utf8(staging)};
            if (png) arguments.push_back("--ps3-hd-png");
            return runProcess(arguments, tools, cancel, [&](const std::string& line) {
                                  char what[32] = {}, item[256] = {};
                                  unsigned long long done = 0, total = 0;
                                  if (std::sscanf(line.c_str(), "ps3hd-progress %31s %llu %llu %255[^\n]", what, &done, &total, item) >= 3) {
                                      // one bar: the read of the disc (the worlds are matched as they are read; the
                                      // last one is the item), after Sly 1's look at its installed levels
                                      const std::string w = what;
                                      if (w == "textures") lastItem = item;
                                      else
                                          log(json({{"stage", w == "read" ? (lastItem.empty() ? "ps3-read" : "ps3-textures") : "ps3-prepare"},
                                                    {"completed", done}, {"total", total}, {"item", w == "read" ? lastItem : std::string(item)}})
                                                  .dump());
                                      return;
                                  }
                                  if (line.rfind("ps3hd: error: ", 0) == 0) error = line.substr(14);
                                  else if (line.find(" HD textures written") != std::string::npos) summary = line;
                                  log(line);
                              });
        };
        int code = 1;
        try {
            code = run();
        } catch (...) {
            fs::remove_all(staging, ec);
            throw;
        }
        if (code != 0 || !(png ? fs::is_directory(staging / "Textures", ec) : fs::is_regular_file(staging / "Textures.pak", ec))) {
            fs::remove_all(staging, ec);
            if (!error.empty()) {
                if (error.back() == '.') error.pop_back();
                throw std::runtime_error(error);
            }
            throw std::runtime_error(std::string(cane::game(game).runtime) + " could not import the PS3 HD textures (exit " +
                                     std::to_string(code) + "). Details are in the launcher output");
        }
        // pack.json: what the Settings page shows
        int textures = 0;
        double megabytes = 0;
        if (const auto at = summary.find(" levels, "); at != std::string::npos) std::sscanf(summary.c_str() + at + 9, "%d HD textures written (%lf MB", &textures, &megabytes);
        else if (const auto at2 = summary.find(" worlds, "); at2 != std::string::npos) std::sscanf(summary.c_str() + at2 + 9, "%d HD textures written (%lf MB", &textures, &megabytes);
        writeJson(staging / "pack.json", json{{"source", source}, {"textures", textures}, {"megabytes", megabytes},
                                              {"date", [] { auto t = std::time(nullptr); char b[32]; std::strftime(b, sizeof(b), "%Y-%m-%d", std::localtime(&t)); return std::string(b); }()},
                                              {"from", "The Sly Collection (PS3)"}});
        auto previous = ps3HdDir(game);
        previous += ".previous";
        fs::remove_all(previous, ec);
        if (fs::exists(ps3HdDir(game), ec)) fs::rename(ps3HdDir(game), previous);
        fs::rename(staging, ps3HdDir(game));
        fs::remove_all(previous, ec);
        setPs3HdEnabled(game, true);
        log(json({{"stage", "ps3-done"}, {"completed", 1}, {"total", 1}}).dump());
    }
    // The game's --texture-pack argument while the pack is installed and on.
    std::vector<std::string> ps3HdArguments(GameId game) const {
        const Ps3HdPack pack = ps3HdPack(game);
        if (!pack.installed || !pack.enabled) return {};
        return {"--texture-pack", utf8(ps3HdDir(game))};
    }
    void buildSource(const fs::path& checkout, const std::atomic<bool>& cancel,
                     const LogFunction& log) {
        auto source = fs::absolute(checkout);
        if (!fs::is_regular_file(source / "CMakeLists.txt") ||
            !fs::is_directory(source / "Sly1-Release"))
            throw std::runtime_error(
                "Select a SafeHouse checkout containing the portable CMake build");
        fs::create_directories(runtimeStorage(locations));
        FileLock buildLock(runtimeStorage(locations) / "source-build.lock");
        auto build = runtimeStorage(locations) / "source-build";
        std::ifstream cache(build / "CMakeCache.txt");
        std::string line;
        while (std::getline(cache, line)) {
            constexpr const char* prefix = "CMAKE_HOME_DIRECTORY:INTERNAL=";
            if (line.starts_with(prefix) &&
                fs::weakly_canonical(path(line.substr(std::char_traits<char>::length(prefix)))) !=
                    fs::weakly_canonical(source)) {
                cache.close();
                fs::remove_all(build);
                break;
            }
        }
        try {
            checkedProcess({"cmake", "--version"}, source, cancel, log);
        } catch (...) {
            if (cancel) throw;
            throw std::runtime_error(
                "CMake is missing or cannot run. Install CMake 3.22+ and add it to PATH, then "
                "restart the launcher.");
        }
        std::vector<std::string> configure = {"cmake",
                                              "-S",
                                              utf8(source),
                                              "-B",
                                              utf8(build),
                                              "-DCMAKE_BUILD_TYPE=Release",
                                              "-DCANE_BUILD_LAUNCHER=OFF",
                                              "-DBUILD_TESTING=OFF"};
        try {
            checkedProcess(configure, source, cancel, log);
        } catch (...) {
            if (cancel) throw;
#ifdef _WIN32
            throw std::runtime_error(
                "Configuration failed. Install Visual Studio 2022 with Desktop development with "
                "C++, or check the build log for missing dependencies.");
#else
            throw std::runtime_error(
                "Configuration failed. Install a C++20 compiler, make/Ninja, and the development "
                "packages listed in README.md. See the build log for the exact missing tool or "
                "library.");
#endif
        }
        checkedProcess({"cmake", "--build", utf8(build), "--config", "Release", "--parallel", "2"},
                       source, cancel, log);
        auto runtime = runtimeStorage(locations) / "runtime";
        InstallationTransaction transaction(runtime);
        checkedProcess({"cmake", "--install", utf8(build), "--config", "Release", "--prefix",
                        utf8(transaction.directory())},
                       source, cancel, log);
        for (auto name : {"cane-extractor", "Sly1-Release"})
            if (!fs::is_regular_file(transaction.directory() / "bin" / executableName(name)))
                throw std::runtime_error("Build did not install required programs");
        transaction.commit();
        tools = runtime / "bin";
        config["runtime_dir"] =
            utf8(locations.portable ? fs::relative(tools, locations.config) : tools);
        writeJson(locations.config / "launcher.json", config);
    }
};
}  // namespace cane
