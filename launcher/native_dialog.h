#pragma once
// The operating system's own file / folder picker, run off the render thread and polled each frame.
//
// Windows: IFileOpenDialog (COM, on a worker thread with its own STA), an .iso filter, FOS_PICKFOLDERS for folders.
// Linux: the XDG Desktop Portal (org.freedesktop.portal.FileChooser.OpenFile over the session bus; GNOME, KDE, the
// Steam Deck's desktop mode, Flatpak), through libdbus-1 loaded with dlopen (no build dependency: every desktop has
// libdbus-1.so.3). Without a portal: `zenity`, then `kdialog`. Without any of them the poll reports Unavailable and the
// caller opens the in-launcher ImGuiFileDialog instead.
//
// The in-launcher picker is also used when a native dialog would be awkward: in gamescope (Steam Deck Game Mode) and
// in Steam Big Picture (SteamGamepadUI / SteamTenfoot), where the launcher is driven with a pad. Pad users of the
// install page get the typed path entry anyway (install_page.h).
// SAFEHOUSE_FILE_DIALOG=launcher|native|portal|zenity|kdialog overrides the choice (portal/zenity/kdialog: that one only).
//
// Paths in and out are UTF-8 std::strings (platform.h's path()/utf8()).
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "platform.h"
#include "cane_process.h"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#else
#include <dlfcn.h>
#endif

namespace cane::filedialog {
// OpenIso: a PS2 disc image (.iso, .chd); OpenPs3: The Sly Collection (PS3) as an .iso or a .zip of one (Settings > Games).
enum class Kind { OpenIso, PickFolder, OpenPs3 };
enum class Status { Idle, Running, Chosen, Cancelled, Unavailable, Failed };

struct Result {
    Status status = Status::Idle;
    std::string path;     // UTF-8, when Chosen
    std::string backend;  // "portal", "zenity", "kdialog", "windows"
    std::string error;    // when Failed
};

namespace detail {
inline std::string env(const char* key) {
    const char* v = std::getenv(key);
    return v ? v : "";
}
inline std::string preference() { return env("SAFEHOUSE_FILE_DIALOG"); }
}  // namespace detail

// True when the native dialog should not be used (Game Mode, Big Picture, or the override says so).
inline bool preferLauncherPicker() {
    const std::string pref = detail::preference();
    if (pref == "launcher") return true;
    if (!pref.empty()) return false;
    if (!detail::env("SteamGamepadUI").empty() || !detail::env("SteamTenfoot").empty()) return true;
#ifndef _WIN32
    if (detail::env("XDG_CURRENT_DESKTOP") == "gamescope" || !detail::env("GAMESCOPE_WAYLAND_DISPLAY").empty()) return true;
#endif
    return false;
}

// The folder a picker opens in when there is no last-used folder: Downloads, else the home folder.
inline fs::path defaultFolder() {
    std::error_code ec;
#ifdef _WIN32
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &known)) && known) {
        fs::path p(known);
        CoTaskMemFree(known);
        if (fs::is_directory(p, ec)) return p;
    } else if (known)
        CoTaskMemFree(known);
    const fs::path home = environmentDirectory("USERPROFILE", fs::current_path(ec));
#else
    const fs::path home = environmentDirectory("HOME", fs::path("/"));
#endif
    if (fs::is_directory(home / "Downloads", ec)) return home / "Downloads";
    return home;
}
// The first of these that is an existing folder (a file's folder counts), else defaultFolder().
inline fs::path startFolder(std::initializer_list<std::string> candidates) {
    std::error_code ec;
    for (const auto& c : candidates) {
        if (c.empty()) continue;
        fs::path p = path(c);
        if (fs::is_regular_file(p, ec)) p = p.parent_path();
        if (!p.empty() && fs::is_directory(p, ec)) return p;
    }
    return defaultFolder();
}

namespace detail {
struct Shared {
    std::mutex mutex;
    Result result;
    std::atomic<bool> done{ false };
    std::atomic<bool> cancel{ false };
};

#ifdef _WIN32
inline Result runWindows(Kind kind, const std::string& title, const fs::path& start) {
    Result r;
    r.backend = "windows";
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                  reinterpret_cast<void**>(&dialog));
    if (FAILED(hr) || !dialog) {
        r.status = Status::Unavailable;
        if (SUCCEEDED(init)) CoUninitialize();
        return r;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    if (kind == Kind::PickFolder) options |= FOS_PICKFOLDERS;
    else options |= FOS_FILEMUSTEXIST;
    dialog->SetOptions(options);
    const std::wstring wtitle = path(title).wstring();
    dialog->SetTitle(wtitle.c_str());
    if (kind == Kind::OpenIso) {
        const COMDLG_FILTERSPEC filters[] = { { L"Disc images (*.iso, *.chd)", L"*.iso;*.chd" }, { L"All files", L"*.*" } };
        dialog->SetFileTypes(2, filters);
        dialog->SetFileTypeIndex(1);
    }
    if (kind == Kind::OpenPs3) {
        const COMDLG_FILTERSPEC filters[] = { { L"Disc images (*.iso, *.zip)", L"*.iso;*.zip" }, { L"All files", L"*.*" } };
        dialog->SetFileTypes(2, filters);
        dialog->SetFileTypeIndex(1);
    }
    if (!start.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.wstring().c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&folder))) &&
            folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    // No owner window: the launcher's window belongs to the render thread, which keeps drawing meanwhile.
    hr = dialog->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) r.status = Status::Cancelled;
    else if (FAILED(hr)) {
        r.status = Status::Failed;
        r.error = "The file dialog could not be shown";
    } else {
        IShellItem* item = nullptr;
        PWSTR name = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
            r.status = Status::Chosen;
            r.path = utf8(fs::path(name));
            CoTaskMemFree(name);
        } else
            r.status = Status::Cancelled;
        if (item) item->Release();
    }
    dialog->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return r;
}
#else
// ---- XDG Desktop Portal over libdbus-1 (dlopen'd; only the few calls needed, declared here) ----
struct DBusError {
    const char* name;
    const char* message;
    unsigned int dummy;
    void* padding1;
};
struct DBusMessageIter {
    alignas(void*) unsigned char data[128];  // libdbus's is 72 bytes on x86_64
};
using u32 = unsigned int;
struct DBusApi {
    void* lib = nullptr;
    void (*error_init)(DBusError*);
    u32 (*error_is_set)(const DBusError*);
    void (*error_free)(DBusError*);
    void* (*bus_get_private)(int, DBusError*);
    void (*connection_close)(void*);
    void (*connection_unref)(void*);
    void (*connection_set_exit_on_disconnect)(void*, u32);
    const char* (*bus_get_unique_name)(void*);
    void (*bus_add_match)(void*, const char*, DBusError*);
    void* (*message_new_method_call)(const char*, const char*, const char*, const char*);
    void (*message_unref)(void*);
    void (*iter_init_append)(void*, DBusMessageIter*);
    u32 (*iter_append_basic)(DBusMessageIter*, int, const void*);
    u32 (*iter_open_container)(DBusMessageIter*, int, const char*, DBusMessageIter*);
    u32 (*iter_close_container)(DBusMessageIter*, DBusMessageIter*);
    u32 (*iter_init)(void*, DBusMessageIter*);
    int (*iter_get_arg_type)(DBusMessageIter*);
    void (*iter_get_basic)(DBusMessageIter*, void*);
    u32 (*iter_next)(DBusMessageIter*);
    void (*iter_recurse)(DBusMessageIter*, DBusMessageIter*);
    void* (*send_with_reply_and_block)(void*, void*, int, DBusError*);
    u32 (*connection_send)(void*, void*, u32*);
    void (*connection_flush)(void*);
    u32 (*read_write)(void*, int);
    void* (*pop_message)(void*);
    u32 (*message_is_signal)(void*, const char*, const char*);
    const char* (*message_get_path)(void*);

    bool load() {
        lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            ok = ok && fn;
        };
        get(error_init, "dbus_error_init");
        get(error_is_set, "dbus_error_is_set");
        get(error_free, "dbus_error_free");
        get(bus_get_private, "dbus_bus_get_private");
        get(connection_close, "dbus_connection_close");
        get(connection_unref, "dbus_connection_unref");
        get(connection_set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
        get(bus_get_unique_name, "dbus_bus_get_unique_name");
        get(bus_add_match, "dbus_bus_add_match");
        get(message_new_method_call, "dbus_message_new_method_call");
        get(message_unref, "dbus_message_unref");
        get(iter_init_append, "dbus_message_iter_init_append");
        get(iter_append_basic, "dbus_message_iter_append_basic");
        get(iter_open_container, "dbus_message_iter_open_container");
        get(iter_close_container, "dbus_message_iter_close_container");
        get(iter_init, "dbus_message_iter_init");
        get(iter_get_arg_type, "dbus_message_iter_get_arg_type");
        get(iter_get_basic, "dbus_message_iter_get_basic");
        get(iter_next, "dbus_message_iter_next");
        get(iter_recurse, "dbus_message_iter_recurse");
        get(send_with_reply_and_block, "dbus_connection_send_with_reply_and_block");
        get(connection_send, "dbus_connection_send");
        get(connection_flush, "dbus_connection_flush");
        get(read_write, "dbus_connection_read_write");
        get(pop_message, "dbus_connection_pop_message");
        get(message_is_signal, "dbus_message_is_signal");
        get(message_get_path, "dbus_message_get_path");
        return ok;
    }
};
inline DBusApi* dbusApi() {
    static DBusApi* api = [] {
        static DBusApi a;
        return a.load() ? &a : nullptr;
    }();
    return api;
}
// file:///a%20b -> /a b (bytes; the portal's URIs are UTF-8).
inline std::string uriToPath(const std::string& uri) {
    if (uri.rfind("file://", 0) != 0) return {};
    std::string s = uri.substr(7), out;
    const auto slash = s.find('/');
    if (slash == std::string::npos) return {};
    s = s.substr(slash);  // drop an authority ("localhost")
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else
            out += s[i];
    }
    return out;
}

inline Result runPortal(Kind kind, const std::string& title, const fs::path& start, const std::atomic<bool>& cancel) {
    constexpr int TS = 's', TB = 'b', TV = 'v', TA = 'a', TU = 'u', TO = 'o', TY = 'y', TE = 'e', TR = 'r';
    Result r;
    r.backend = "portal";
    r.status = Status::Unavailable;
    DBusApi* d = dbusApi();
    if (!d || (detail::env("DBUS_SESSION_BUS_ADDRESS").empty() && detail::env("XDG_RUNTIME_DIR").empty())) return r;
    DBusError err;
    d->error_init(&err);
    void* conn = d->bus_get_private(0 /* DBUS_BUS_SESSION */, &err);
    if (!conn) {
        d->error_free(&err);
        return r;
    }
    d->connection_set_exit_on_disconnect(conn, 0);
    auto closeConn = [&] {
        d->connection_close(conn);
        d->connection_unref(conn);
    };
    // The request object path is predictable from our unique name and handle_token; subscribe before calling.
    static std::atomic<unsigned> counter{ 0 };
    const std::string token = "safehouse" + std::to_string(++counter);
    std::string sender = d->bus_get_unique_name(conn) ? d->bus_get_unique_name(conn) : "";
    if (!sender.empty() && sender[0] == ':') sender.erase(0, 1);
    for (auto& c : sender)
        if (c == '.') c = '_';
    std::string handle = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    auto addMatch = [&](const std::string& p) {
        const std::string rule = "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" + p + "'";
        d->bus_add_match(conn, rule.c_str(), &err);
        if (d->error_is_set(&err)) d->error_free(&err);
    };
    addMatch(handle);

    void* msg = d->message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                           "org.freedesktop.portal.FileChooser", "OpenFile");
    DBusMessageIter it, opts;
    d->iter_init_append(msg, &it);
    const char* parent = "";
    const char* ctitle = title.c_str();
    d->iter_append_basic(&it, TS, &parent);
    d->iter_append_basic(&it, TS, &ctitle);
    d->iter_open_container(&it, TA, "{sv}", &opts);
    auto entry = [&](const char* key, const char* sig, auto&& writeValue) {
        DBusMessageIter e, v;
        d->iter_open_container(&opts, TE, nullptr, &e);
        d->iter_append_basic(&e, TS, &key);
        d->iter_open_container(&e, TV, sig, &v);
        writeValue(v);
        d->iter_close_container(&e, &v);
        d->iter_close_container(&opts, &e);
    };
    const char* ctoken = token.c_str();
    entry("handle_token", "s", [&](DBusMessageIter& v) { d->iter_append_basic(&v, TS, &ctoken); });
    u32 yes = 1;
    entry("modal", "b", [&](DBusMessageIter& v) { d->iter_append_basic(&v, TB, &yes); });
    if (kind == Kind::PickFolder) entry("directory", "b", [&](DBusMessageIter& v) { d->iter_append_basic(&v, TB, &yes); });
    else {
        // filters a(sa(us)): ("Disc images", [(0, "*.iso"), (0, "*.ISO")]), ("All files", [(0, "*")])
        auto filterList = [&](DBusMessageIter& v) {
            DBusMessageIter arr;
            d->iter_open_container(&v, TA, "(sa(us))", &arr);
            auto one = [&](const char* name, std::initializer_list<const char*> globs) {
                DBusMessageIter st, list;
                d->iter_open_container(&arr, TR, nullptr, &st);
                d->iter_append_basic(&st, TS, &name);
                d->iter_open_container(&st, TA, "(us)", &list);
                for (const char* g : globs) {
                    DBusMessageIter pair;
                    u32 type = 0;
                    d->iter_open_container(&list, TR, nullptr, &pair);
                    d->iter_append_basic(&pair, TU, &type);
                    d->iter_append_basic(&pair, TS, &g);
                    d->iter_close_container(&list, &pair);
                }
                d->iter_close_container(&st, &list);
                d->iter_close_container(&arr, &st);
            };
            if (kind == Kind::OpenPs3) one("Disc images (*.iso, *.zip)", { "*.iso", "*.ISO", "*.zip", "*.ZIP" });
            else one("Disc images (*.iso, *.chd)", { "*.iso", "*.ISO", "*.chd", "*.CHD" });
            one("All files", { "*" });
            d->iter_close_container(&v, &arr);
        };
        entry("filters", "a(sa(us))", filterList);
    }
    const std::string folder = utf8(start);
    if (!folder.empty())
        entry("current_folder", "ay", [&](DBusMessageIter& v) {
            DBusMessageIter bytes;
            d->iter_open_container(&v, TA, "y", &bytes);
            for (size_t i = 0; i <= folder.size(); ++i) {  // NUL-terminated
                unsigned char b = i < folder.size() ? static_cast<unsigned char>(folder[i]) : 0;
                d->iter_append_basic(&bytes, TY, &b);
            }
            d->iter_close_container(&v, &bytes);
        });
    d->iter_close_container(&it, &opts);

    void* reply = d->send_with_reply_and_block(conn, msg, 10000, &err);
    d->message_unref(msg);
    if (!reply) {
        // No portal service (org.freedesktop.DBus.Error.ServiceUnknown) or no FileChooser backend.
        d->error_free(&err);
        closeConn();
        return r;
    }
    DBusMessageIter ri;
    if (d->iter_init(reply, &ri) && d->iter_get_arg_type(&ri) == TO) {
        const char* p = nullptr;
        d->iter_get_basic(&ri, &p);
        if (p && handle != p) {
            handle = p;  // old portals (before handle_token) pick their own path
            addMatch(handle);
        }
    }
    d->message_unref(reply);

    r.status = Status::Running;
    while (r.status == Status::Running) {
        if (cancel) {
            void* close = d->message_new_method_call("org.freedesktop.portal.Desktop", handle.c_str(),
                                                     "org.freedesktop.portal.Request", "Close");
            d->connection_send(conn, close, nullptr);
            d->connection_flush(conn);
            d->message_unref(close);
            r.status = Status::Cancelled;
            break;
        }
        if (!d->read_write(conn, 100)) {
            r.status = Status::Failed;
            r.error = "The desktop portal connection closed";
            break;
        }
        while (void* m = d->pop_message(conn)) {
            const char* mp = d->message_get_path(m);
            if (d->message_is_signal(m, "org.freedesktop.portal.Request", "Response") && mp && handle == mp) {
                DBusMessageIter mi;
                u32 code = 2;
                r.status = Status::Cancelled;
                if (d->iter_init(m, &mi) && d->iter_get_arg_type(&mi) == TU) {
                    d->iter_get_basic(&mi, &code);
                    if (code == 0 && d->iter_next(&mi) && d->iter_get_arg_type(&mi) == TA) {
                        DBusMessageIter dict;
                        d->iter_recurse(&mi, &dict);
                        while (d->iter_get_arg_type(&dict) == TE) {
                            DBusMessageIter e, v;
                            d->iter_recurse(&dict, &e);
                            const char* key = nullptr;
                            d->iter_get_basic(&e, &key);
                            d->iter_next(&e);
                            d->iter_recurse(&e, &v);
                            if (key && std::strcmp(key, "uris") == 0 && d->iter_get_arg_type(&v) == TA) {
                                DBusMessageIter uris;
                                d->iter_recurse(&v, &uris);
                                if (d->iter_get_arg_type(&uris) == TS) {
                                    const char* uri = nullptr;
                                    d->iter_get_basic(&uris, &uri);
                                    if (uri) r.path = uriToPath(uri);
                                }
                            }
                            d->iter_next(&dict);
                        }
                    }
                }
                if (code == 0 && !r.path.empty()) r.status = Status::Chosen;
                else if (code == 2) {
                    r.status = Status::Failed;
                    r.error = "The desktop portal could not show the file dialog";
                }
            }
            d->message_unref(m);
        }
    }
    closeConn();
    return r;
}

// zenity / kdialog: exit 0 with the path on stdout, 1 on cancel; 127 when the program is missing.
inline Result runHelper(const std::string& program, Kind kind, const std::string& title, const fs::path& start,
                        const std::atomic<bool>& cancel) {
    Result r;
    r.backend = program;
    std::string folder = utf8(start);
    if (!folder.empty() && folder.back() != '/') folder += '/';
    std::vector<std::string> args{ program };
    if (program == "zenity") {
        args.insert(args.end(), { "--file-selection", "--title=" + title });
        if (kind == Kind::PickFolder) args.push_back("--directory");
        else if (kind == Kind::OpenPs3) args.insert(args.end(), { "--file-filter=Disc images (*.iso, *.zip) | *.iso *.ISO *.zip *.ZIP", "--file-filter=All files | *" });
        else args.insert(args.end(), { "--file-filter=Disc images (*.iso, *.chd) | *.iso *.ISO *.chd *.CHD", "--file-filter=All files | *" });
        if (!folder.empty()) args.push_back("--filename=" + folder);
    } else {
        if (kind == Kind::PickFolder) args.insert(args.end(), { "--getexistingdirectory", folder.empty() ? "." : folder });
        else if (kind == Kind::OpenPs3) args.insert(args.end(), { "--getopenfilename", folder.empty() ? "." : folder, "*.iso *.ISO *.zip *.ZIP|Disc images (*.iso, *.zip)\n*|All files" });
        else args.insert(args.end(), { "--getopenfilename", folder.empty() ? "." : folder, "*.iso *.ISO *.chd *.CHD|Disc images (*.iso, *.chd)\n*|All files" });
        args.insert(args.end(), { "--title", title });
    }
    std::string chosen;
    int code = 127;
    try {
        code = runProcess(args, fs::path(), cancel, [&](const std::string& line) {
            // GTK / Qt warnings share the stream: the answer is the absolute path that exists.
            std::error_code ec;
            if (!line.empty() && line[0] == '/' && fs::exists(path(line), ec)) chosen = line;
        });
    } catch (const std::exception&) {
        r.status = Status::Cancelled;
        return r;
    }
    if (code == 0 && !chosen.empty()) r.status = Status::Chosen, r.path = chosen;
    else if (code == 1 || code == 0) r.status = Status::Cancelled;
    else r.status = Status::Unavailable;  // missing (127) or no display
    return r;
}
#endif

inline Result run(Kind kind, const std::string& title, const fs::path& start, const std::atomic<bool>& cancel) {
#ifdef _WIN32
    (void)cancel;
    return runWindows(kind, title, start);
#else
    const std::string pref = preference();
    std::vector<std::string> order;
    if (pref == "portal" || pref == "zenity" || pref == "kdialog") order = { pref };
    else order = { "portal", "zenity", "kdialog" };
    Result r;
    r.status = Status::Unavailable;
    for (const auto& b : order) {
        r = b == "portal" ? runPortal(kind, title, start, cancel) : runHelper(b, kind, title, start, cancel);
        if (r.status != Status::Unavailable) break;
    }
    return r;
#endif
}
}  // namespace detail

// One native dialog at a time per owner. start() returns false when the in-launcher picker should be used instead
// (preferLauncherPicker()); otherwise poll() each frame until it returns a finished Result (once). An Unavailable
// result means no native dialog exists here: open the in-launcher picker then.
class Async {
public:
    Async() = default;
    Async(const Async&) = delete;
    Async& operator=(const Async&) = delete;
    ~Async() {
        // A dialog still open when the launcher quits: ask it to close (the portal can; Windows' cannot) and leave the
        // thread to the process exit. The shared state outlives this object.
        if (m_shared) m_shared->cancel = true;
        if (m_thread.joinable()) m_thread.detach();
    }
    bool running() const { return m_shared && !m_shared->done; }
    bool start(Kind kind, const std::string& title, const fs::path& folder) {
        if (running()) return true;
        if (preferLauncherPicker()) return false;
        if (m_thread.joinable()) m_thread.join();
        m_shared = std::make_shared<detail::Shared>();
        m_thread = std::thread([s = m_shared, kind, title, folder] {
            Result r = detail::run(kind, title, folder, s->cancel);
            std::lock_guard lock(s->mutex);
            s->result = std::move(r);
            s->done = true;
        });
        return true;
    }
    std::optional<Result> poll() {
        if (!m_shared || !m_shared->done) return std::nullopt;
        if (m_thread.joinable()) m_thread.join();
        Result r;
        {
            std::lock_guard lock(m_shared->mutex);
            r = m_shared->result;
        }
        m_shared.reset();
        return r;
    }

private:
    std::shared_ptr<detail::Shared> m_shared;
    std::thread m_thread;
};

// A blocking pick for tests (`safehouse --pick-file` / `--pick-folder`).
inline Result pickBlocking(Kind kind, const std::string& title, const fs::path& folder) {
    if (preferLauncherPicker()) return { Status::Unavailable, {}, "launcher", {} };
    std::atomic<bool> cancel{ false };
    return detail::run(kind, title, folder, cancel);
}
}  // namespace cane::filedialog
