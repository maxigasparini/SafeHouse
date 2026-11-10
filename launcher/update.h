#pragma once
// The update check and the in-place update of the launcher's own package.
//
// Check: GitHub's API for the latest release of FridiNaTor1/SafeHouse (a generic User-Agent, nothing else is sent).
// Every release carries SafeHouse-update.json (scripts/release-manifest.py): its version, commit, UTC build date and
// each package's file name, size and SHA-256, plus which package updates which kind of installation.
//
// Update: the package for this installation is downloaded next to it, checked against the manifest's SHA-256 and
// unpacked (stage()). When the launcher exits, its files are swapped (apply()): every file of the new package replaces
// the old one, the old one is kept aside until the whole swap succeeded, and any failure puts the old files back.
// Saves, settings and installed games are never in the package folder's swap set (data/ and portable.txt are
// skipped), so an update cannot touch them.
//   Windows (setup or portable): a copy of the new safehouse.exe runs from <folder>\.update\helper, waits for the
//     launcher to exit, swaps the files (retrying files another program holds open), updates the version shown in
//     Apps & features when setup installed this folder, and starts SafeHouse.exe.
//   Linux AppImage: the new AppImage is renamed over $APPIMAGE (one atomic rename, the old file's permissions).
//   Linux folder (the tar.gz): the files are swapped in place; running programs keep their old files open.
//   macOS: the new SafeHouse.app replaces the old bundle (two renames in the same folder).
// On Linux and macOS the launcher swaps the files itself after its window closed, then starts the new version.
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "install.h"
#include "cane_process.h"
#include "safehouse_version.h"
#ifdef _WIN32
#include <winhttp.h>
#include <miniz.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

namespace cane::update {
inline constexpr const char* defaultApiUrl = "https://api.github.com/repos/FridiNaTor1/SafeHouse/releases/latest";
inline constexpr const char* releasesPage = "https://github.com/FridiNaTor1/SafeHouse/releases/latest";
inline constexpr const char* manifestName = "SafeHouse-update.json";
// Test hooks: another API address (a local server or, on Linux / macOS, a file:// URL), and with it a pretended
// version / build date of this build (so a development build can be tested). Never used without the address.
inline constexpr const char* apiUrlEnv = "SAFEHOUSE_UPDATE_URL";
inline constexpr const char* testVersionEnv = "SAFEHOUSE_UPDATE_TEST_VERSION";
inline constexpr const char* testDateEnv = "SAFEHOUSE_UPDATE_TEST_DATE";

inline std::string environment(const char* key) {
#ifdef _WIN32
    const auto value = environmentDirectory(key, {});
    return utf8(value);
#else
    const char* value = std::getenv(key);
    return value ? value : "";
#endif
}

// ------------------------------------------------------------ this build
struct BuildInfo {
    std::string version;  // "0.1.42"; empty: a development build
    std::string commit;   // short hash
    std::string date;     // "2026-11-05T12:34:56Z"
};
inline BuildInfo current() {
    BuildInfo b{SAFEHOUSE_VERSION, SAFEHOUSE_COMMIT, SAFEHOUSE_BUILD_DATE};
    if (!environment(apiUrlEnv).empty()) {
        if (auto v = environment(testVersionEnv); !v.empty()) b.version = v;
        if (auto d = environment(testDateEnv); !d.empty()) b.date = d;
    }
    return b;
}
inline std::string describe(const BuildInfo& b) {
    std::string text = b.version.empty() ? std::string("development build") : b.version;
    if (!b.commit.empty()) text += " (" + b.commit + ")";
    if (b.date.size() >= 10) text += ", built " + b.date.substr(0, 10);
    return text;
}
// Release builds with a version check for updates; debug and development builds never do (unless a test points
// the check at its own server).
inline bool checksAllowed() {
    const bool test = !environment(apiUrlEnv).empty();
#ifndef NDEBUG
    if (!test) return false;
#endif
    return !current().version.empty() || test;
}

// "v0.1.42" / "0.1.42" -> {0, 1, 42}; empty when it is not a version.
inline std::vector<int> versionParts(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<int> parts;
    size_t i = 0;
    while (i < v.size()) {
        if (!std::isdigit(static_cast<unsigned char>(v[i]))) return {};
        long n = 0;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) n = std::min(n * 10 + (v[i++] - '0'), 1000000000L);
        parts.push_back(static_cast<int>(n));
        if (i == v.size()) break;
        if (v[i] != '.') break;  // "0.1.42-rc1": the numbers before the suffix
        ++i;
    }
    return parts;
}
// <0, 0, >0 like strcmp; missing parts count as 0.
inline int compareVersions(const std::string& a, const std::string& b) {
    const auto x = versionParts(a), y = versionParts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}
// Days since 1970-01-01 of an ISO date ("2026-11-05..."), or nothing.
inline std::optional<long> dayNumber(const std::string& iso) {
    int y = 0, m = 0, d = 0;
    if (iso.size() < 10 || std::sscanf(iso.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return std::nullopt;
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
// How many days older this build is than the release (0 when unknown or not older).
inline int daysBehind(const std::string& buildDate, const std::string& releaseDate) {
    const auto a = dayNumber(buildDate), b = dayNumber(releaseDate);
    return a && b && *b > *a ? static_cast<int>(*b - *a) : 0;
}

// ------------------------------------------------------------ HTTP
using Progress = std::function<void(uint64_t done, uint64_t total)>;
struct Request {
    std::string url;
    std::string* body = nullptr;  // the response in memory (at most maxBody bytes) ...
    fs::path file;                // ... or written to this file
    size_t maxBody = 4u << 20;
    bool api = false;             // GitHub API: JSON Accept header and a total time limit
    Progress progress;
    const std::atomic<bool>* cancel = nullptr;
};
inline std::string userAgent() {
    const auto v = std::string(SAFEHOUSE_VERSION);
    return std::string("SafeHouse-Launcher/") + (v.empty() ? "dev" : v);
}
inline bool cancelled(const Request& r) { return r.cancel && r.cancel->load(); }

#ifdef _WIN32
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
struct WinHttpHandle {
    HINTERNET h = nullptr;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    ~WinHttpHandle() {
        if (h) WinHttpCloseHandle(h);
    }
};
inline std::runtime_error winHttpError(const char* what) {
    const DWORD code = GetLastError();
    const char* reason = code == 12007 ? "the server name was not found"
                         : code == 12029 || code == 12030 ? "no connection"
                         : code == 12002 ? "the connection timed out"
                         : code == 12175 || code == 12045 || code == 12038 || code == 12037 ? "the secure connection failed"
                                                                                               : nullptr;
    return std::runtime_error(std::string(what) + ": " + (reason ? reason : "network error " + std::to_string(code)));
}
inline void httpGet(const Request& r) {
    const std::wstring url = path(r.url).wstring();
    URL_COMPONENTS c{};
    c.dwStructSize = sizeof c;
    c.dwSchemeLength = c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &c) || (c.nScheme != INTERNET_SCHEME_HTTPS && c.nScheme != INTERNET_SCHEME_HTTP))
        throw std::runtime_error("Unsupported address: " + r.url);
    const std::wstring host(c.lpszHostName, c.dwHostNameLength);
    std::wstring object(c.lpszUrlPath, c.dwUrlPathLength + c.dwExtraInfoLength);
    if (object.empty()) object = L"/";
    const std::wstring userAgentText = path(userAgent()).wstring();
    WinHttpHandle session(WinHttpOpen(userAgentText.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) session.h = WinHttpOpen(userAgentText.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) throw winHttpError("Cannot start the download");
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, r.api ? 15000 : 30000);
    WinHttpHandle connection(WinHttpConnect(session.h, host.c_str(), c.nPort, 0));
    if (!connection.h) throw winHttpError("Cannot connect");
    WinHttpHandle request(WinHttpOpenRequest(connection.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             c.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request.h) throw winHttpError("Cannot connect");
    const wchar_t* headers = r.api ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (!WinHttpSendRequest(request.h, headers, r.api ? static_cast<DWORD>(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        throw winHttpError("Cannot reach the update server");
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) throw std::runtime_error("The update server answered " + std::to_string(status));
    uint64_t total = 0;
    wchar_t lengthText[32] = {};
    size = sizeof lengthText;
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &size, WINHTTP_NO_HEADER_INDEX))
        total = std::wcstoull(lengthText, nullptr, 10);
    FILE* out = nullptr;
    if (!r.file.empty() && !(out = _wfopen(r.file.c_str(), L"wb"))) throw std::runtime_error("Cannot write " + utf8(r.file));
    std::unique_ptr<FILE, int (*)(FILE*)> closer(out, [](FILE* f) { return f ? std::fclose(f) : 0; });
    std::vector<char> buffer(1 << 16);
    uint64_t done = 0;
    for (;;) {
        if (cancelled(r)) throw std::runtime_error("Cancelled");
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) throw winHttpError("The download was interrupted");
        if (got == 0) break;
        done += got;
        if (out) {
            if (std::fwrite(buffer.data(), 1, got, out) != got) throw std::runtime_error("Cannot write " + utf8(r.file) + " (is the disk full?)");
        } else if (r.body) {
            if (r.body->size() + got > r.maxBody) throw std::runtime_error("The update server's answer is too large");
            r.body->append(buffer.data(), got);
        }
        if (r.progress) r.progress(done, total);
    }
    if (total && done != total) throw std::runtime_error("The download was interrupted");
    if (out && std::fflush(out) != 0) throw std::runtime_error("Cannot write " + utf8(r.file));
}
#else
// libcurl, loaded at run time from the system (every Linux distribution and macOS has it), so the packages bundle
// no TLS library. Without it the curl program is used.
struct Curl {
    void* (*easyInit)() = nullptr;
    int (*easySetopt)(void*, int, ...) = nullptr;
    int (*easyPerform)(void*) = nullptr;
    void (*easyCleanup)(void*) = nullptr;
    int (*easyGetinfo)(void*, int, ...) = nullptr;
    const char* (*easyStrerror)(int) = nullptr;
    void* (*slistAppend)(void*, const char*) = nullptr;
    void (*slistFreeAll)(void*) = nullptr;
    bool ok = false;
    static Curl& get() {
        static Curl curl = load();
        return curl;
    }
    static Curl load() {
        Curl c;
        if (!environment("SAFEHOUSE_UPDATE_NO_LIBCURL").empty()) return c;  // tests: the curl program path
#ifdef __APPLE__
        const char* names[] = {"/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib"};
#else
        const char* names[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
#endif
        void* lib = nullptr;
        for (const char* name : names)
            if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
        if (!lib) return c;
        auto sym = [&](auto& target, const char* name) { target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(dlsym(lib, name)); };
        sym(c.easyInit, "curl_easy_init");
        sym(c.easySetopt, "curl_easy_setopt");
        sym(c.easyPerform, "curl_easy_perform");
        sym(c.easyCleanup, "curl_easy_cleanup");
        sym(c.easyGetinfo, "curl_easy_getinfo");
        sym(c.easyStrerror, "curl_easy_strerror");
        sym(c.slistAppend, "curl_slist_append");
        sym(c.slistFreeAll, "curl_slist_free_all");
        int (*globalInit)(long) = nullptr;
        sym(globalInit, "curl_global_init");
        c.ok = c.easyInit && c.easySetopt && c.easyPerform && c.easyCleanup && c.easyGetinfo && c.easyStrerror && c.slistAppend &&
               c.slistFreeAll && globalInit && globalInit(3) == 0;
        return c;
    }
};
struct CurlTransfer {
    const Request* request;
    FILE* out = nullptr;
    std::string error;
    static size_t write(char* data, size_t size, size_t count, void* user) {
        auto* t = static_cast<CurlTransfer*>(user);
        const size_t n = size * count;
        if (cancelled(*t->request)) return 0;
        if (t->out) {
            if (std::fwrite(data, 1, n, t->out) != n) {
                t->error = "Cannot write " + utf8(t->request->file) + " (is the disk full?)";
                return 0;
            }
        } else if (t->request->body) {
            if (t->request->body->size() + n > t->request->maxBody) {
                t->error = "The update server's answer is too large";
                return 0;
            }
            t->request->body->append(data, n);
        }
        return n;
    }
    static int progress(void* user, int64_t total, int64_t now, int64_t, int64_t) {
        auto* t = static_cast<CurlTransfer*>(user);
        if (t->request->progress && now > 0) t->request->progress(static_cast<uint64_t>(now), static_cast<uint64_t>(std::max<int64_t>(total, 0)));
        return cancelled(*t->request) ? 1 : 0;
    }
};
inline void curlProgram(const Request& r) {
    fs::path target = r.file;
    if (target.empty()) target = fs::temp_directory_path() / ("safehouse-update-" + std::to_string(processId()) + ".json");
    std::vector<std::string> args{"curl", "-fsSL", "--proto", "=https,http,file", "--proto-redir", "=https,http", "--connect-timeout", "10",
                                  "--speed-limit", "1", "--speed-time", "30", "-A", userAgent(), "-o", utf8(target)};
    if (r.api) args.insert(args.end(), {"--max-time", "30", "-H", "Accept: application/vnd.github+json"});
    args.push_back(r.url);
    std::atomic<bool> stop{false};
    std::string output;
    int code = 127;
    try {
        code = runProcess(args, {}, r.cancel ? *r.cancel : stop, [&](const std::string& line) { output += line + "\n"; });
    } catch (const std::exception&) {
        if (cancelled(r)) throw std::runtime_error("Cancelled");
        code = 127;
    }
    if (code == 127) throw std::runtime_error("Cannot download: neither libcurl nor the curl program was found");
    if (code != 0) throw std::runtime_error(code == 6 ? "Cannot reach the update server: the server name was not found"
                                           : code == 7 ? "Cannot reach the update server: no connection"
                                           : code == 22 ? "The update server refused the request"
                                           : code == 28 ? "The connection timed out"
                                           : code == 18 || code == 56 ? "The download was interrupted"
                                                                      : "The download failed (curl " + std::to_string(code) + ")");
    if (r.body) {
        std::ifstream in(target, std::ios::binary);
        r.body->assign(std::istreambuf_iterator<char>(in), {});
        std::error_code ec;
        fs::remove(target, ec);
        if (r.body->size() > r.maxBody) throw std::runtime_error("The update server's answer is too large");
    }
    if (r.progress) {
        std::error_code ec;
        const auto size = fs::file_size(target, ec);
        if (!ec) r.progress(size, size);
    }
}
inline void httpGet(const Request& r) {
    Curl& c = Curl::get();
    if (!c.ok) return curlProgram(r);
    CurlTransfer t{&r};
    if (!r.file.empty() && !(t.out = std::fopen(utf8(r.file).c_str(), "wb"))) throw std::runtime_error("Cannot write " + utf8(r.file));
    std::unique_ptr<FILE, int (*)(FILE*)> closer(t.out, [](FILE* f) { return f ? std::fclose(f) : 0; });
    std::unique_ptr<void, void (*)(void*)> handle(c.easyInit(), c.easyCleanup);
    if (!handle) throw std::runtime_error("Cannot start the download");
    void* h = handle.get();
    void* headers = r.api ? c.slistAppend(nullptr, "Accept: application/vnd.github+json") : nullptr;
    std::unique_ptr<void, void (*)(void*)> headerList(headers, c.slistFreeAll);
    const std::string userAgentText = userAgent();
    c.easySetopt(h, 10002 /*URL*/, r.url.c_str());
    c.easySetopt(h, 10018 /*USERAGENT*/, userAgentText.c_str());
    c.easySetopt(h, 181 /*PROTOCOLS*/, 1L | 2L | 1024L);  // http, https, file
    c.easySetopt(h, 182 /*REDIR_PROTOCOLS*/, 1L | 2L);
    c.easySetopt(h, 52 /*FOLLOWLOCATION*/, 1L);
    c.easySetopt(h, 68 /*MAXREDIRS*/, 10L);
    c.easySetopt(h, 78 /*CONNECTTIMEOUT*/, 10L);
    c.easySetopt(h, 19 /*LOW_SPEED_LIMIT*/, 1L);
    c.easySetopt(h, 20 /*LOW_SPEED_TIME*/, 30L);
    if (r.api) c.easySetopt(h, 13 /*TIMEOUT*/, 30L);
    c.easySetopt(h, 99 /*NOSIGNAL*/, 1L);
    c.easySetopt(h, 45 /*FAILONERROR*/, 1L);
    if (headers) c.easySetopt(h, 10023 /*HTTPHEADER*/, headers);
    c.easySetopt(h, 20011 /*WRITEFUNCTION*/, &CurlTransfer::write);
    c.easySetopt(h, 10001 /*WRITEDATA*/, &t);
    c.easySetopt(h, 43 /*NOPROGRESS*/, 0L);
    c.easySetopt(h, 20219 /*XFERINFOFUNCTION*/, &CurlTransfer::progress);
    c.easySetopt(h, 10057 /*XFERINFODATA*/, &t);
    const int code = c.easyPerform(h);
    if (cancelled(r)) throw std::runtime_error("Cancelled");
    if (!t.error.empty()) throw std::runtime_error(t.error);
    if (code != 0) {
        long status = 0;
        c.easyGetinfo(h, 0x200002 /*RESPONSE_CODE*/, &status);
        throw std::runtime_error(code == 6 ? "Cannot reach the update server: the server name was not found"
                                 : code == 7 ? "Cannot reach the update server: no connection"
                                 : code == 22 ? "The update server answered " + std::to_string(status)
                                 : code == 28 ? std::string("The connection timed out")
                                 : code == 18 || code == 56 ? std::string("The download was interrupted")
                                                            : std::string("The download failed: ") + c.easyStrerror(code));
    }
    if (t.out && std::fflush(t.out) != 0) throw std::runtime_error("Cannot write " + utf8(r.file));
}
#endif

// ------------------------------------------------------------ releases
struct Package {
    std::string key, file, sha256, url;
    uint64_t size = 0;
};
struct Release {
    std::string tag, version, commit, date, notes, page;
    std::map<std::string, Package> packages;      // by key ("linux-appimage", "windows-portable", ...)
    std::map<std::string, std::string> updater;   // installation kind -> package key
};
inline std::string text(const json& j, const char* key) {
    return j.is_object() && j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>() : std::string{};
}
inline std::string apiUrl() {
    auto url = environment(apiUrlEnv);
    return url.empty() ? defaultApiUrl : url;
}
// The latest release and its manifest. Throws with a short reason (offline, no manifest, ...).
inline Release fetchLatest(const std::atomic<bool>* cancel = nullptr) {
    std::string body;
    httpGet({.url = apiUrl(), .body = &body, .api = true, .cancel = cancel});
    const json release = json::parse(body, nullptr, false);
    if (!release.is_object()) throw std::runtime_error("The update server sent something unexpected");
    Release r;
    r.tag = text(release, "tag_name");
    r.notes = text(release, "body");
    r.page = text(release, "html_url");
    r.date = text(release, "published_at");
    std::map<std::string, std::string> assets;
    if (release.contains("assets") && release.at("assets").is_array())
        for (const auto& a : release.at("assets"))
            if (!text(a, "name").empty()) assets[text(a, "name")] = text(a, "browser_download_url");
    const auto manifestUrl = assets.find(manifestName);
    if (manifestUrl == assets.end() || manifestUrl->second.empty()) throw std::runtime_error("The latest release has no update information");
    body.clear();
    httpGet({.url = manifestUrl->second, .body = &body, .api = false, .cancel = cancel});
    const json m = json::parse(body, nullptr, false);
    if (!m.is_object() || versionParts(text(m, "version")).empty()) throw std::runtime_error("The latest release's update information is damaged");
    r.version = text(m, "version");
    r.commit = text(m, "commit");
    if (!text(m, "date").empty()) r.date = text(m, "date");
    if (r.tag.empty()) r.tag = "v" + r.version;
    if (m.contains("packages") && m.at("packages").is_object())
        for (const auto& [key, p] : m.at("packages").items()) {
            Package package{key, text(p, "file"), text(p, "sha256"), {}, 0};
            if (p.is_object() && p.contains("size") && p.at("size").is_number_unsigned()) package.size = p.at("size").get<uint64_t>();
            if (auto a = assets.find(package.file); a != assets.end()) package.url = a->second;
            r.packages[key] = package;
        }
    if (m.contains("updater") && m.at("updater").is_object())
        for (const auto& [kind, key] : m.at("updater").items())
            if (key.is_string()) r.updater[kind] = key.get<std::string>();
    return r;
}

// ------------------------------------------------------------ this installation
enum class Kind { Unsupported, AppImage, LinuxFolder, WindowsPortable, WindowsInstalled, MacApp };
inline const char* kindKey(Kind k) {
    switch (k) {
#if defined(__aarch64__) || defined(__arm64__)
    case Kind::AppImage: return "linux-aarch64-appimage";
    case Kind::LinuxFolder: return "linux-aarch64-tar";
    case Kind::WindowsPortable: return "windows-arm64-portable";
    case Kind::WindowsInstalled: return "windows-arm64-installed";
    case Kind::MacApp: return "macos-arm64";
#else
    case Kind::AppImage: return "linux-appimage";
    case Kind::LinuxFolder: return "linux-tar";
    case Kind::WindowsPortable: return "windows-portable";
    case Kind::WindowsInstalled: return "windows-installed";
    case Kind::MacApp: return "macos-x86_64";
#endif
    default: return "";
    }
}
inline Kind kindFromKey(const std::string& key) {
    for (Kind k : {Kind::AppImage, Kind::LinuxFolder, Kind::WindowsPortable, Kind::WindowsInstalled, Kind::MacApp})
        if (key == kindKey(k)) return k;
    return Kind::Unsupported;
}
struct Install {
    Kind kind = Kind::Unsupported;
    fs::path target;   // the folder, the AppImage file or the .app bundle that is replaced
    fs::path staging;  // where the download is unpacked: beside the target, on the same drive
    std::string reason;  // why this copy cannot update itself
};
inline Install detectInstall() {
    Install in;
    const fs::path exe = executableDirectory();
    std::error_code ec;
#ifdef _WIN32
    const fs::path root = exe.parent_path();
    if (exe.filename() != "bin" || !fs::is_regular_file(root / "SafeHouse.exe", ec)) {
        in.reason = "This copy of SafeHouse was not installed from a release package.";
        return in;
    }
    in.kind = fs::is_regular_file(root / "portable.txt", ec) || !fs::is_regular_file(root / "Uninstall.exe", ec) ? Kind::WindowsPortable
                                                                                                               : Kind::WindowsInstalled;
    in.target = root;
    in.staging = root / ".update";
#elif defined(__APPLE__)
    const fs::path app = exe.parent_path().parent_path();
    if (exe.filename() != "MacOS" || app.extension() != ".app") {
        in.reason = "This copy of SafeHouse is not an app bundle.";
        return in;
    }
    if (utf8(app).find("/AppTranslocation/") != std::string::npos) {
        in.reason = "Move SafeHouse to your Applications folder first; macOS runs it from a temporary copy until then.";
        return in;
    }
    in.kind = Kind::MacApp;
    in.target = app;
    in.staging = app.parent_path() / ".safehouse-update";
#else
    if (const auto appimage = environment("APPIMAGE"); !appimage.empty() && fs::is_regular_file(path(appimage), ec)) {
        in.kind = Kind::AppImage;
        in.target = path(appimage);
        in.staging = in.target.parent_path() / ".safehouse-update";
        return in;
    }
    const fs::path root = exe.parent_path();
    if (exe.filename() != "bin" || !fs::is_symlink(root / "SafeHouse", ec)) {
        in.reason = "This copy of SafeHouse was not installed from a release package (for example a distribution package or "
                    "a build from source). Update it the way you installed it.";
        return in;
    }
    in.kind = Kind::LinuxFolder;
    in.target = root;
    in.staging = root / ".update";
#endif
    return in;
}
// The package a release offers for this installation, or nothing.
inline std::optional<Package> packageFor(const Release& r, const Install& in) {
    if (in.kind == Kind::Unsupported) return std::nullopt;
    std::string key = kindKey(in.kind);
    if (auto u = r.updater.find(key); u != r.updater.end()) key = u->second;
    else if (in.kind == Kind::WindowsInstalled) key = kindKey(Kind::WindowsPortable);
    auto p = r.packages.find(key);
    if (p == r.packages.end() || p->second.url.empty() || p->second.sha256.size() != 64) return std::nullopt;
    return p->second;
}

// ------------------------------------------------------------ staging
struct Plan {
    Kind kind = Kind::Unsupported;
    fs::path source;   // the unpacked package folder, the new AppImage or the new .app
    fs::path target;
    fs::path staging;
    std::string version, fromVersion;
};
inline json toJson(const Plan& p) {
    return {{"kind", kindKey(p.kind)}, {"source", utf8(p.source)}, {"target", utf8(p.target)}, {"staging", utf8(p.staging)},
            {"version", p.version}, {"from_version", p.fromVersion}};
}
inline Plan planFrom(const json& j) {
    Plan p;
    p.kind = kindFromKey(text(j, "kind"));
    p.source = path(text(j, "source"));
    p.target = path(text(j, "target"));
    p.staging = path(text(j, "staging"));
    p.version = text(j, "version");
    p.fromVersion = text(j, "from_version");
    if (p.kind == Kind::Unsupported || p.source.empty() || p.target.empty() || p.staging.empty()) throw std::runtime_error("Invalid update plan");
    return p;
}
inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// The one folder an archive unpacked into (SafeHouse-Launcher-<platform>/ or SafeHouse.app/).
inline fs::path onlyFolder(const fs::path& dir) {
    fs::path found;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_directory() || !found.empty()) throw std::runtime_error("The update package has an unexpected layout");
        found = e.path();
    }
    if (found.empty()) throw std::runtime_error("The update package is empty");
    return found;
}
#ifdef _WIN32
inline void unzip(const fs::path& zip, const fs::path& dir, const std::atomic<bool>* cancel) {
    FILE* file = _wfopen(zip.c_str(), L"rb");
    if (!file) throw std::runtime_error("Cannot open the update package");
    std::unique_ptr<FILE, int (*)(FILE*)> closer(file, std::fclose);
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!mz_zip_reader_init_cfile(&archive, file, fs::file_size(zip), 0)) throw std::runtime_error("The update package is damaged");
    std::unique_ptr<mz_zip_archive, mz_bool (*)(mz_zip_archive*)> end(&archive, mz_zip_reader_end);
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
        if (cancel && *cancel) throw std::runtime_error("Cancelled");
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&archive, i, &st)) throw std::runtime_error("The update package is damaged");
        std::string name = st.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        while (!name.empty() && name.back() == '/') name.pop_back();
        if (name.empty()) continue;
        const fs::path out = safeAssetPath(dir, name);
        if (st.m_is_directory) {
            fs::create_directories(out);
            continue;
        }
        fs::create_directories(out.parent_path());
        FILE* target = _wfopen(out.c_str(), L"wb");
        if (!target) throw std::runtime_error("Cannot unpack " + name);
        const bool ok = mz_zip_reader_extract_to_cfile(&archive, i, target, 0);
        const bool closed = std::fclose(target) == 0;
        if (!ok || !closed) throw std::runtime_error("Cannot unpack " + name + " (the package is damaged or the disk is full)");
    }
}
#endif
// Downloads, checks and unpacks the package beside the installation. Nothing of the installation changes.
inline Plan stage(const Release& r, const Install& in, const Progress& progress, const std::atomic<bool>* cancel, const LogFunction& log) {
    const auto package = packageFor(r, in);
    if (!package) throw std::runtime_error("The latest release has no package for this kind of installation");
    std::error_code ec;
    fs::remove_all(in.staging, ec);
    fs::create_directories(in.staging, ec);
    if (ec) throw std::runtime_error("SafeHouse cannot write to its folder (" + utf8(in.target.parent_path()) + "), so it cannot update itself there");
    const fs::path download = in.staging / package->file, part = in.staging / (package->file + ".part");
    log("Downloading " + package->file + " (" + r.version + ")");
    httpGet({.url = package->url, .file = part, .progress = progress, .cancel = cancel});
    if (package->size && fs::file_size(part, ec) != package->size)
        throw std::runtime_error("The download is incomplete. Nothing was changed; try again later.");
    log("Checking SHA-256");
    if (lower(hashFile(part, [&] { return cancel && cancel->load(); })) != lower(package->sha256)) {
        fs::remove(part, ec);
        throw std::runtime_error("The download is damaged (its SHA-256 does not match the release). Nothing was changed.");
    }
    fs::rename(part, download);
    Plan plan{in.kind, {}, in.target, in.staging, r.version, current().version};
    const fs::path unpacked = in.staging / "new";
    switch (in.kind) {
    case Kind::AppImage: {
        plan.source = download;
        fs::permissions(download, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                                      fs::perms::others_exec);
        break;
    }
#ifdef _WIN32
    case Kind::WindowsPortable:
    case Kind::WindowsInstalled:
        log("Unpacking");
        unzip(download, unpacked, cancel);
        plan.source = onlyFolder(unpacked);
        if (!fs::is_regular_file(plan.source / "bin" / "safehouse.exe") || !fs::is_regular_file(plan.source / "SafeHouse.exe"))
            throw std::runtime_error("The update package has an unexpected layout");
        break;
#else
    case Kind::LinuxFolder:
    case Kind::MacApp: {
        log("Unpacking");
        fs::create_directories(unpacked);
        std::atomic<bool> stop{false};
        std::string output;
#ifdef __APPLE__
        const std::vector<std::string> command{"/usr/bin/ditto", "-x", "-k", utf8(download), utf8(unpacked)};
#else
        const std::vector<std::string> command{"tar", "-xzf", utf8(download), "-C", utf8(unpacked)};
#endif
        if (runProcess(command, {}, cancel ? *cancel : stop, [&](const std::string& line) { output += line + "\n"; }) != 0)
            throw std::runtime_error("Cannot unpack the update package: " + output);
        plan.source = onlyFolder(unpacked);
#ifdef __APPLE__
        if (plan.source.extension() != ".app" || !fs::is_regular_file(plan.source / "Contents/MacOS/safehouse"))
#else
        if (!fs::is_regular_file(plan.source / "bin/safehouse"))
#endif
            throw std::runtime_error("The update package has an unexpected layout");
        break;
    }
#endif
    default: throw std::runtime_error("This installation cannot update itself");
    }
    if (in.kind != Kind::AppImage) fs::remove(download, ec);
    writeJson(in.staging / "plan.json", toJson(plan));
    log("Ready: " + utf8(plan.source));
    return plan;
}

// ------------------------------------------------------------ the swap
// A rename that waits for files another program holds open (Windows: antivirus scanners, a game still closing).
inline bool move(const fs::path& from, const fs::path& to, std::string& error, int attempts = 40) {
    for (int i = 0;; ++i) {
#ifdef _WIN32
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        const DWORD code = GetLastError();
        const bool locked = code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == ERROR_LOCK_VIOLATION;
        error = utf8(to.filename()) + (locked ? " is in use by another program" : " cannot be replaced (error " + std::to_string(code) + ")");
        if (!locked || i + 1 >= attempts) return false;
#else
        std::error_code ec;
        fs::rename(from, to, ec);
        if (!ec) return true;
        error = utf8(to.filename()) + " cannot be replaced: " + ec.message();
        if (ec != std::errc::device_or_resource_busy || i + 1 >= attempts) return false;
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}
inline bool skipped(const fs::path& relative) {
    const auto top = utf8(*relative.begin());
    const auto low = lower(top);
    return low == "data" || low == "portable.txt" || low == "uninstall.exe" || low == ".update";
}
// Every file of the new package into the folder; the replaced ones go to backup first. On a failure everything done
// is undone, in reverse.
inline void swapFolder(const Plan& plan, const fs::path& backup, const LogFunction& log, int attempts) {
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(plan.source); it != fs::recursive_directory_iterator(); ++it) {
        const fs::path relative = it->path().lexically_relative(plan.source);
        std::error_code ec;
        const bool link = it->is_symlink(ec), dir = !link && it->is_directory(ec);
        if (skipped(relative)) {
            if (dir) it.disable_recursion_pending();
            continue;
        }
        if (!dir) files.push_back(relative);
    }
    std::sort(files.begin(), files.end());
    struct Step {
        fs::path relative;
        bool replaced;
    };
    std::vector<Step> done;
    std::vector<fs::path> createdDirs;
    std::string error;
    auto ensureDir = [&](const fs::path& dir) {
        std::vector<fs::path> missing;
        for (fs::path d = dir; !d.empty() && !fs::exists(fs::symlink_status(d)); d = d.parent_path()) missing.push_back(d);
        fs::create_directories(dir);
        createdDirs.insert(createdDirs.end(), missing.begin(), missing.end());
    };
    try {
        for (const auto& relative : files) {
            const fs::path oldFile = plan.target / relative, newFile = plan.source / relative, saved = backup / relative;
            ensureDir(oldFile.parent_path());
            const bool replaced = fs::exists(fs::symlink_status(oldFile));
            if (replaced) {
                fs::create_directories(saved.parent_path());
                if (!move(oldFile, saved, error, attempts)) throw std::runtime_error(error);
            }
            if (!move(newFile, oldFile, error, attempts)) {
                std::string ignored;
                if (replaced) move(saved, oldFile, ignored, attempts);
                throw std::runtime_error(error);
            }
            done.push_back({relative, replaced});
        }
    } catch (const std::exception& e) {
        log(std::string("Update failed: ") + e.what() + "; restoring the previous version");
        bool restored = true;
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            std::string ignored;
            restored = move(plan.target / it->relative, plan.source / it->relative, ignored, attempts) && restored;
            if (it->replaced) restored = move(backup / it->relative, plan.target / it->relative, ignored, attempts) && restored;
        }
        std::error_code ec;
        for (const auto& d : createdDirs) fs::remove(d, ec);  // (only when empty)
        throw std::runtime_error(std::string(e.what()) +
                                 (restored ? ". The previous version was kept." : ". Some files could not be restored: download SafeHouse again."));
    }
    log("Replaced " + std::to_string(done.size()) + " files");
}
// One file or bundle replaced by the new one: AppImage (atomic rename over it, its permissions kept) and the .app.
inline void swapWhole(const Plan& plan, const fs::path& backup, const LogFunction& log) {
    std::error_code ec;
    if (plan.kind == Kind::AppImage) {
        const auto perms = fs::status(plan.target, ec).permissions();
        if (!ec) fs::permissions(plan.source, perms | fs::perms::owner_exec, ec);
        std::string error;
        if (!move(plan.source, plan.target, error, 1)) throw std::runtime_error(error + ". The previous version was kept.");
        log("Replaced " + utf8(plan.target));
        return;
    }
    fs::create_directories(backup);
    const fs::path saved = backup / plan.target.filename();
    std::string error;
    if (!move(plan.target, saved, error, 1)) throw std::runtime_error(error + ". The previous version was kept.");
    if (!move(plan.source, plan.target, error, 1)) {
        std::string ignored;
        const bool restored = move(saved, plan.target, ignored, 1);
        throw std::runtime_error(error + (restored ? ". The previous version was kept." : ". Download SafeHouse again."));
    }
    log("Replaced " + utf8(plan.target));
}
#ifdef _WIN32
// Apps & features shows the version setup wrote; keep it current when setup installed this folder.
inline void updateUninstallEntry(const Plan& plan, const LogFunction& log) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SafeHouse", 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return;
    wchar_t location[4096] = {};
    DWORD size = sizeof location - sizeof(wchar_t), type = 0;
    std::error_code ec;
    if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(location), &size) == ERROR_SUCCESS && type == REG_SZ &&
        fs::equivalent(fs::path(location), plan.target, ec)) {
        const std::wstring version = path(plan.version).wstring();
        RegSetValueExW(key, L"DisplayVersion", 0, REG_SZ, reinterpret_cast<const BYTE*>(version.c_str()),
                       static_cast<DWORD>((version.size() + 1) * sizeof(wchar_t)));
        log("Apps & features: version " + plan.version);
    }
    RegCloseKey(key);
}
#endif
// Swaps the files. Throws with a message for the player; the previous version stays in place on failure.
inline void apply(const Plan& plan, const LogFunction& log, int attempts = 40) {
    const fs::path backup = plan.staging / "previous";
    std::error_code ec;
    fs::remove_all(backup, ec);
    if (plan.kind == Kind::AppImage || plan.kind == Kind::MacApp) swapWhole(plan, backup, log);
    else swapFolder(plan, backup, log, attempts);
#ifdef _WIN32
    if (plan.kind == Kind::WindowsInstalled) updateUninstallEntry(plan, log);
#endif
}
// The launcher of the updated installation, started on its own (it outlives this process).
inline void startLauncher(const Plan& plan) {
#ifdef _WIN32
    fs::path exe = plan.target / "SafeHouse.exe";
    if (!fs::is_regular_file(exe)) exe = plan.target / "bin" / "safehouse.exe";
    std::wstring line = quoteWindows(exe.wstring());
    STARTUPINFOW startup{sizeof startup};
    PROCESS_INFORMATION process{};
    const std::wstring dir = plan.target.wstring();
    if (CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
#else
    std::vector<std::string> args;
    if (plan.kind == Kind::MacApp) args = {"/usr/bin/open", "-n", utf8(plan.target)};
    else if (plan.kind == Kind::AppImage) args = {utf8(plan.target)};
    else args = {utf8(plan.target / "bin" / "safehouse")};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    const std::string dir = utf8(plan.target.parent_path());
    const pid_t pid = fork();
    if (pid == 0) {
        if (fork() != 0) _exit(0);
        setsid();
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        if (chdir(dir.c_str()) != 0) {
        }
        execv(argv[0], argv.data());
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
#endif
}
// The result the next launcher shows ("SafeHouse was updated to ..."), in the profile's config folder.
inline fs::path resultFile(const fs::path& configDir) { return configDir / "update-result.json"; }
inline void writeResult(const fs::path& configDir, const Plan& plan, bool ok, const std::string& message) {
    try {
        writeJson(resultFile(configDir), {{"ok", ok}, {"version", plan.version}, {"from_version", plan.fromVersion}, {"message", message}});
    } catch (...) {
    }
}
// Leftovers of an earlier update (the old files, the helper, a download that was not used).
inline void cleanup(const Install& in) {
    if (in.staging.empty()) return;
    std::error_code ec;
    if (fs::exists(in.staging, ec)) fs::remove_all(in.staging, ec);
}
#ifdef _WIN32
// Windows: the launcher cannot replace its own files while it runs. A copy of the new safehouse.exe (with the
// C++ runtime DLLs it needs) runs from .update\helper, waits for this process to end, swaps the files and starts
// the launcher again.
inline void launchHelper(const Plan& plan, bool restart) {
    const fs::path helper = plan.staging / "helper";
    std::error_code ec;
    fs::remove_all(helper, ec);
    fs::create_directories(helper);
    const fs::path bin = plan.source / "bin";
    for (const auto& e : fs::directory_iterator(bin)) {
        const auto name = lower(utf8(e.path().filename()));
        if (name == "safehouse.exe" || (name.rfind("lib", 0) == 0 && e.path().extension() == ".dll"))
            fs::copy_file(e.path(), helper / e.path().filename(), fs::copy_options::overwrite_existing);
    }
    const fs::path exe = helper / "safehouse.exe";
    std::wstring line = quoteWindows(exe.wstring()) + L" --apply-update " + quoteWindows((plan.staging / "plan.json").wstring()) +
                        L" --wait-pid " + std::to_wstring(GetCurrentProcessId()) + (restart ? L" --restart" : L"");
    STARTUPINFOW startup{sizeof startup};
    PROCESS_INFORMATION process{};
    const std::wstring dir = helper.wstring();
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &startup, &process))
        throw std::runtime_error("Cannot start the updater");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
#endif
// --apply-update PLAN [--wait-pid PID] [--restart] [--attempts N]: the swap, run by the helper (Windows) or by
// tests. Returns the exit code.
inline int applyMain(const std::vector<std::string>& args) {
    Plan plan;
    uint64_t waitPid = 0;
    bool restart = false;
    int attempts = 40;
    try {
        plan = planFrom(readJson(path(args.at(1))));
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--wait-pid" && i + 1 < args.size()) waitPid = std::stoull(args[++i]);
            else if (args[i] == "--restart") restart = true;
            else if (args[i] == "--attempts" && i + 1 < args.size()) attempts = std::max(1, std::stoi(args[++i]));
        }
    } catch (const std::exception& e) {
        std::cerr << "--apply-update: " << e.what() << "\n";
        return 2;
    }
    std::ofstream logFile(plan.staging / "update.log", std::ios::app);
    const LogFunction log = [&](const std::string& line) {
        logFile << line << std::endl;
        std::cout << line << std::endl;
    };
    // The profile of the updated installation (portable: its data folder).
    const fs::path config = userPaths(plan.kind == Kind::MacApp ? plan.target / "Contents/MacOS" : plan.target / "bin").config;
    for (int i = 0; waitPid && processAlive(waitPid) && i < 600; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool ok = true;
    std::string message;
    if (waitPid && processAlive(waitPid)) {
        ok = false;
        message = "SafeHouse did not close, so it was not updated.";
    } else {
        try {
            log("Updating " + utf8(plan.target) + " to " + plan.version);
            apply(plan, log, attempts);
            log("Updated");
        } catch (const std::exception& e) {
            ok = false;
            message = e.what();
            log(message);
        }
    }
    writeResult(config, plan, ok, message);
#ifdef _WIN32
    if (!ok && restart)
        MessageBoxW(nullptr, path("SafeHouse could not be updated: " + message).c_str(), L"SafeHouse", MB_OK | MB_ICONWARNING);
#endif
    if (restart) startLauncher(plan);
    return ok ? 0 : 1;
}
}  // namespace cane::update
