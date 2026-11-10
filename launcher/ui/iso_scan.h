#pragma once
// Background search for recognizable Sly 1, 2 and 3 disc images (.iso, .chd).
// A configured library is searched exclusively, up to six folders deep; the
// automatic fallback searches usual folders up to two deep. Symlinks are not
// followed and the search is bounded to 200 results / 20000 entries.
// Unrelated or unreadable images are never shown in the automatic list.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include "disc_image.h"
#include "platform.h"

namespace cane::ui {
struct IsoCandidate {
    std::filesystem::path path;
    uintmax_t size = 0;
    std::filesystem::file_time_type modified{};
    DiscIdentity identity; // which game the image is (read from its SYSTEM.CNF)
};

class IsoScanner {
public:
    ~IsoScanner() { stopWorker(); }
    // onlyExtra keeps unrelated Downloads/Desktop images out of a configured
    // library. Explicit folder browsing uses the same behavior.
    void scan(std::vector<std::filesystem::path> extra = {}, bool onlyExtra = false) {
        stopWorker();
        {
            std::lock_guard lock(m_mutex);
            m_results.clear();  // never display stale results during a new scan
        }
        m_checkedImages = 0;
        m_limited = false;
        m_extra = std::move(extra);
        m_onlyExtra = onlyExtra;
        m_stop = false;
        m_scanning = true;
        m_worker = std::thread([this] { run(); });
    }
    bool scanning() const { return m_scanning; }
    size_t checkedImages() const { return m_checkedImages; }
    bool limited() const { return m_limited; }
    std::vector<IsoCandidate> results() {
        std::lock_guard lock(m_mutex);
        return m_results;
    }
    // Test fixtures replace the scan with a fixed list.
    void setResults(std::vector<IsoCandidate> results) {
        stopWorker();
        std::lock_guard lock(m_mutex);
        m_results = std::move(results);
    }
    static std::vector<std::filesystem::path> roots() {
        namespace fs = std::filesystem;
        std::vector<fs::path> result;
        std::error_code ec;
#ifdef _WIN32
        if (const wchar_t* profile = _wgetenv(L"USERPROFILE"); profile && *profile)
            for (auto name : { L"Downloads", L"Documents", L"Desktop" }) result.push_back(fs::path(profile) / name);
#else
        static constexpr const char* romFolders[] = { "Emulation/roms/ps2", "retrodeck/roms/ps2" };
        const char* home = std::getenv("HOME");
        std::vector<fs::path> general;
        if (home && *home) {
            const fs::path h(home);
            for (auto name : romFolders) result.push_back(h / name);
            for (auto name : { "Games", "Downloads", "Desktop", "Documents" }) general.push_back(h / name);
        }
        std::vector<fs::path> volumes;
#ifdef __APPLE__
        // Finder mounts removable drives and disc images below /Volumes.
        for (const auto& entry : fs::directory_iterator("/Volumes", fs::directory_options::skip_permission_denied, ec))
            if (std::error_code e; entry.is_directory(e)) volumes.push_back(entry.path());
#else
        // Removable media: /run/media/<user>/<volume> and /run/media/<volume>.
        for (const auto& first : fs::directory_iterator("/run/media", fs::directory_options::skip_permission_denied, ec)) {
            std::error_code inner;
            if (!first.is_directory(inner)) continue;
            volumes.push_back(first.path());
            for (const auto& second : fs::directory_iterator(first.path(), fs::directory_options::skip_permission_denied, inner))
                if (std::error_code e; second.is_directory(e)) volumes.push_back(second.path());
        }
#endif
        for (const auto& volume : volumes)
            for (auto name : romFolders)
                if (std::error_code e; fs::is_directory(volume / name, e)) result.push_back(volume / name);
        result.insert(result.end(), general.begin(), general.end());
        result.insert(result.end(), volumes.begin(), volumes.end());
#endif
        return result;
    }
    // A disc image the installers read: an .iso or a CHD.
    static bool isIso(const std::filesystem::path& file) {
        auto ext = utf8(file.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".iso" || ext == ".chd";
    }

private:
    static constexpr size_t kMaxResults = 200;
    static constexpr size_t kMaxEntries = 20000;
    static constexpr uintmax_t kMinSize = 64ull * 1024 * 1024;
    void stopWorker() {
        m_stop = true;
        if (m_worker.joinable()) m_worker.join();
        m_scanning = false;
    }
    // Compares on the native string: path::string() throws on Windows for
    // names that the ANSI code page cannot represent.
    static bool hidden(const std::filesystem::path& entry) {
        const auto& name = entry.filename().native();
        return name.empty() || name.front() == '.';
    }
    void visit(const std::filesystem::path& folder, int depth, int maxDepth, std::vector<IsoCandidate>& found, size_t& entries) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && !m_stop; it.increment(ec)) {
            if (++entries > kMaxEntries || found.size() >= kMaxResults) {
                m_limited = true;
                return;
            }
            std::error_code e;
            const auto status = it->symlink_status(e);
            if (e) continue;
            if (fs::is_directory(status)) {
                if (depth < maxDepth && !hidden(it->path()))
                    visit(it->path(), depth + 1, maxDepth, found, entries);
                continue;
            }
            if (!isIso(it->path())) continue;
            const auto size = it->file_size(e);
            if (e || size < kMinSize) continue;
            ++m_checkedImages;
            const auto identity = identifyDiscImage(it->path());
            // The automatic list is for Sly discs, not arbitrary ISO9660 files,
            // CD/DVD images or other PS2 titles.
            if (!identity.game || !installable(*identity.game) ||
                (identity.kind != DiscIdentity::Kind::Supported &&
                 identity.kind != DiscIdentity::Kind::OtherRegion)) continue;
            if (std::any_of(found.begin(), found.end(), [&](const auto& c) { return fs::equivalent(c.path, it->path(), e); })) continue;
            found.push_back({ it->path(), size, it->last_write_time(e), identity });
        }
    }
    void run() {
        std::vector<IsoCandidate> found;
        size_t entries = 0;
        // An exception must never escape the worker thread (std::terminate);
        // whatever was found before the failure is still shown.
        try {
            for (const auto& root : m_extra) {
                if (m_stop || m_limited) break;
                visit(root, 0, 6, found, entries);
            }
            if (!m_onlyExtra)
                for (const auto& root : roots()) {
                    if (m_stop || m_limited) break;
                    visit(root, 0, 2, found, entries);
                }
        } catch (...) {
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.modified > b.modified; });
        {
            std::lock_guard lock(m_mutex);
            m_results = std::move(found);
        }
        m_scanning = false;
    }
    std::thread m_worker;
    std::mutex m_mutex;
    std::vector<IsoCandidate> m_results;
    std::vector<std::filesystem::path> m_extra;
    bool m_onlyExtra = false;
    std::atomic<size_t> m_checkedImages{ 0 };
    std::atomic<bool> m_stop{ false }, m_scanning{ false }, m_limited{ false };
};
} // namespace cane::ui
