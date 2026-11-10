#pragma once
// Upstream ProjectCane's level pipeline (File Extractor GUI/Services/*.cs, beta 0.5.0), ported to C++ for
// cane-extractor. The retail levels are rebuilt into PCBRX by the native tools that sit beside cane-extractor:
//   Sly1BrxRebuilder <level.brx> [ntsc|pal|japanese] [identity-map] -> <level>_Rebuilt.brx (PCBRX v17)
//   Sly1PalExtractor extract <iso> <output> [pal|japanese]          -> Worlds/<...>/<Level>/<Language>/...
// and each level's language payloads are packed into one PCBRX v19 "PUNI" container (PCBRX-v19.md).
// The runtime reads extraction_manifest.json (Sly1-Release/content.cpp) to learn the region and languages.

#include "../platform/install.h"
#include "../platform/cane_process.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace pcbrx {
namespace fs = std::filesystem;

constexpr uint32_t kMagic = 0x52424350u;          // "PCBR"
constexpr uint32_t kUnifiedMagic = 0x494E5550u;   // "PUNI"
constexpr uint32_t kLocalizationMagic = 0x434F4C50u; // "PLOC"
constexpr std::size_t kLevelCount = 46;

enum class Region { NtscU, Pal, Japanese, Korean };

inline Region RegionOf(const cane::Sly1Disc& disc) {
    const std::string r = disc.region;
    if (r == "PAL") return Region::Pal;
    if (r == "NTSC-J") return Region::Japanese;
    if (r == "NTSC-K") return Region::Korean;
    return Region::NtscU;
}

inline std::vector<uint8_t> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("Cannot read " + cane::utf8(path));
    std::vector<uint8_t> data(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    if (!data.empty() && !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("Cannot read " + cane::utf8(path));
    return data;
}

inline void WriteAll(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || (!data.empty() && !out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))))
        throw std::runtime_error("Cannot write " + cane::utf8(path));
    out.close();
    if (!out) throw std::runtime_error("Cannot write " + cane::utf8(path));
}

inline void TryRemove(const fs::path& path) {
    std::error_code error;
    fs::remove(path, error);
}

inline uint32_t Le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t Le16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

inline std::string UniqueStem() {
    static std::mt19937_64 random{std::random_device{}()};
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(random()));
    return text;
}

// A native tool beside cane-extractor (BundledToolService / FindRebuilder in the C# extractor).
inline fs::path Tool(const char* name) {
#ifdef _WIN32
    const fs::path file = cane::executableDirectory() / (std::string(name) + ".exe");
#else
    const fs::path file = cane::executableDirectory() / name;
#endif
    if (!fs::is_regular_file(file))
        throw std::runtime_error("The level tool " + cane::utf8(file.filename()) +
                                 " is missing beside cane-extractor. Reinstall SafeHouse.");
    return file;
}

// Runs a tool; returns its output. Throws with the tool's last lines when it fails.
inline std::string RunTool(const std::vector<std::string>& arguments, const cane::LogFunction& line = {}) {
    std::atomic<bool> cancelled{false};
    std::string output;
    const int code = cane::runProcess(arguments, {}, cancelled, [&](const std::string& text) {
        output += text;
        output += '\n';
        if (output.size() > (1u << 20)) output.erase(0, output.size() - (1u << 19));
        if (line) line(text);
    });
    if (code != 0) {
        std::string tail = output.size() > 2000 ? output.substr(output.size() - 2000) : output;
        throw std::runtime_error(cane::utf8(fs::path(arguments[0]).filename()) + " failed (exit " +
                                 std::to_string(code) + "): " + tail);
    }
    return output;
}

inline uint32_t Crc32(const uint8_t* data, std::size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline uint64_t GearValue(uint8_t value) {
    uint64_t state = value + 0x9E3779B97F4A7C15ull;
    state ^= state >> 30;
    state *= 0xBF58476D1CE4E5B9ull;
    state ^= state >> 27;
    state *= 0x94D049BB133111EBull;
    return state ^ (state >> 31);
}

// Out of line: inlined into the static's initializer, GCC 11 for aarch64 addresses the table with a huge constant
// offset that the linker can't encode ("relocation truncated to fit").
#if defined(__GNUC__)
__attribute__((noinline))
#endif
inline std::array<uint64_t, 256> MakeGearTable() {
    std::array<uint64_t, 256> table{};
    for (int i = 0; i < 256; ++i) table[i] = GearValue(static_cast<uint8_t>(i));
    return table;
}

// Content-defined chunk boundaries (BrxRebuildService.SplitContentDefinedChunks): [begin, end) pairs.
inline std::vector<std::pair<std::size_t, std::size_t>> SplitChunks(const std::vector<uint8_t>& payload) {
    constexpr std::size_t minimumSize = 16 * 1024, maximumSize = 256 * 1024;
    constexpr uint64_t boundaryMask = (1ull << 16) - 1;
    static const auto gear = MakeGearTable();
    std::vector<std::pair<std::size_t, std::size_t>> chunks;
    std::size_t start = 0;
    uint64_t hash = 0;
    for (std::size_t index = 0; index < payload.size(); ++index) {
        hash = (hash << 1) + gear[payload[index]];
        const std::size_t length = index - start + 1;
        if (length < minimumSize || (length < maximumSize && (hash & boundaryMask) != 0)) continue;
        chunks.emplace_back(start, index + 1);
        start = index + 1;
        hash = 0;
    }
    if (start < payload.size()) chunks.emplace_back(start, payload.size());
    return chunks;
}

// BrxRebuildService.WriteUnifiedContainerAsync: one PCBRX v19 holding every language payload, with chunks
// shared between languages stored once.
inline void WriteUnifiedContainer(const std::vector<fs::path>& payloadPaths, const fs::path& outputPath,
                                  const std::vector<uint32_t>& languageIds, uint32_t defaultLanguage) {
    if (payloadPaths.empty() || payloadPaths.size() != languageIds.size())
        throw std::runtime_error("A unified level requires one language ID per payload.");
    if (std::find(languageIds.begin(), languageIds.end(), defaultLanguage) == languageIds.end() ||
        std::set<uint32_t>(languageIds.begin(), languageIds.end()).size() != languageIds.size())
        throw std::runtime_error("The unified level language directory is invalid.");
    std::vector<std::vector<uint8_t>> payloads;
    for (const auto& path : payloadPaths) payloads.push_back(ReadAll(path));
    struct Chunk { std::size_t payload, begin, end; };
    std::vector<Chunk> chunks;
    std::unordered_map<std::string, uint32_t> chunkIds;
    std::vector<std::vector<uint32_t>> references(payloads.size());
    for (std::size_t p = 0; p < payloads.size(); ++p) {
        const auto& payload = payloads[p];
        for (const auto& [begin, end] : SplitChunks(payload)) {
            const std::string key = picosha2::hash256_hex_string(payload.begin() + static_cast<std::ptrdiff_t>(begin),
                                                                 payload.begin() + static_cast<std::ptrdiff_t>(end));
            auto [it, inserted] = chunkIds.emplace(key, static_cast<uint32_t>(chunks.size()));
            if (inserted) chunks.push_back({p, begin, end});
            else {
                const Chunk& c = chunks[it->second];
                if (c.end - c.begin != end - begin ||
                    std::memcmp(payloads[c.payload].data() + c.begin, payload.data() + begin, end - begin) != 0)
                    throw std::runtime_error("A content hash collision occurred while packing the level.");
            }
            references[p].push_back(it->second);
        }
    }
    // Reconstruct every payload from the map before writing (as upstream does).
    for (std::size_t p = 0; p < payloads.size(); ++p) {
        std::size_t offset = 0;
        for (uint32_t id : references[p]) {
            const Chunk& c = chunks[id];
            const std::size_t length = c.end - c.begin;
            if (offset + length > payloads[p].size() ||
                std::memcmp(payloads[p].data() + offset, payloads[c.payload].data() + c.begin, length) != 0)
                throw std::runtime_error("The unified level chunk map did not reconstruct its source payload.");
            offset += length;
        }
        if (offset != payloads[p].size()) throw std::runtime_error("The unified level chunk map has the wrong payload length.");
    }
    std::vector<uint8_t> out;
    auto put32 = [&](uint32_t v) { const auto* b = reinterpret_cast<const uint8_t*>(&v); out.insert(out.end(), b, b + 4); };
    auto put64 = [&](uint64_t v) { const auto* b = reinterpret_cast<const uint8_t*>(&v); out.insert(out.end(), b, b + 8); };
    put32(kMagic);
    put32(19);
    put32(kUnifiedMagic);
    put32(1);
    put32(static_cast<uint32_t>(payloads.size()));
    put32(defaultLanguage);
    put32(static_cast<uint32_t>(chunks.size()));
    for (std::size_t p = 0; p < payloads.size(); ++p) {
        put32(languageIds[p]);
        put64(payloads[p].size());
        put32(static_cast<uint32_t>(references[p].size()));
        for (uint32_t id : references[p]) put32(id);
    }
    for (const Chunk& c : chunks) {
        const uint8_t* data = payloads[c.payload].data() + c.begin;
        const std::size_t length = c.end - c.begin;
        put32(static_cast<uint32_t>(length));
        put32(Crc32(data, length));
        out.insert(out.end(), data, data + length);
    }
    const fs::path temporary = fs::path(outputPath).concat("." + UniqueStem() + ".tmp");
    try {
        WriteAll(temporary, out);
        fs::rename(temporary, outputPath);
    } catch (...) {
        TryRemove(temporary);
        throw;
    }
}

inline void ValidatePcBrx(const fs::path& path, bool requireLocalization, uint32_t expectedVersion) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("The BRX rebuilder did not produce an output file.");
    const auto size = static_cast<uint64_t>(in.tellg());
    uint8_t header[8]{};
    in.seekg(0);
    if (size < 8 || !in.read(reinterpret_cast<char*>(header), 8) || Le32(header) != kMagic ||
        Le32(header + 4) != expectedVersion)
        throw std::runtime_error("The BRX rebuilder returned success but did not produce a PCBRX v" +
                                 std::to_string(expectedVersion) + " file.");
    if (requireLocalization) {
        uint8_t footer[8]{};
        in.seekg(static_cast<std::streamoff>(size - 8));
        if (!in.read(reinterpret_cast<char*>(footer), 8) || Le32(footer) != kLocalizationMagic)
            throw std::runtime_error("The localized PCBRX has no localization table.");
    }
}

// The PLOC footer's payload (the level's localized strings).
inline std::vector<uint8_t> ReadLocalizationPayload(const fs::path& path) {
    const auto bytes = ReadAll(path);
    if (bytes.size() < 8 || Le32(bytes.data() + bytes.size() - 8) != kLocalizationMagic)
        throw std::runtime_error("'" + cane::utf8(path) + "' has no localization footer.");
    const uint32_t size = Le32(bytes.data() + bytes.size() - 4);
    if (size > bytes.size() - 8) throw std::runtime_error("'" + cane::utf8(path) + "' has an invalid localization footer.");
    const auto end = bytes.end() - 8;
    return std::vector<uint8_t>(end - size, end);
}

// The two UTF-16 string tables after a retail SCPS level's name table
// (BrxRebuildService.ReadJapaneseSourceLocalizationTablesAsync).
inline std::vector<uint8_t> ReadJapaneseSourceLocalizationTables(const fs::path& path) {
    const auto bytes = ReadAll(path);
    std::size_t offset = 8;
    auto need = [&](std::size_t n) {
        if (n > bytes.size() || offset > bytes.size() - n)
            throw std::runtime_error("Japanese level '" + cane::utf8(path) + "' has truncated localization data.");
    };
    need(7);
    const bool debugInfo = bytes[offset++] != 0;
    offset += 2; // Sound-bank selector.
    const uint32_t nameCount = Le32(bytes.data() + offset);
    offset += 4;
    if (debugInfo && nameCount > 1)
        for (uint32_t i = 1; i < nameCount; ++i) {
            need(2);
            const uint16_t n = Le16(bytes.data() + offset);
            offset += 2;
            need(n);
            offset += n;
        }
    const std::size_t start = offset;
    for (int table = 0; table < 2; ++table) {
        need(2);
        const uint16_t count = Le16(bytes.data() + offset);
        offset += 2;
        for (uint16_t i = 0; i < count; ++i) {
            need(2);
            const uint16_t characters = Le16(bytes.data() + offset);
            offset += 2;
            need(std::size_t(characters) * 2);
            offset += std::size_t(characters) * 2;
        }
    }
    return std::vector<uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Every "VAG_*.wav" a rebuilt payload names must exist in its language's Dialogs or Commentary directory.
inline void ValidateReferencedAudio(const fs::path& payloadPath, const fs::path& sourceLanguagePath) {
    const auto payload = ReadAll(payloadPath);
    const fs::path languageDirectory = sourceLanguagePath.parent_path();
    std::set<std::string> references;
    for (std::size_t offset = 0; offset + 4 <= payload.size(); ++offset) {
        if (std::memcmp(payload.data() + offset, "VAG_", 4) != 0) continue;
        std::size_t end = offset;
        const std::size_t limit = std::min(payload.size(), offset + 32);
        while (end < limit && payload[end] != 0) ++end;
        if (end == limit) continue;
        std::string name(reinterpret_cast<const char*>(payload.data() + offset), end - offset);
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".wav") == 0) references.insert(name);
        offset = end;
    }
    for (const auto& name : references)
        if (!fs::is_regular_file(languageDirectory / "Dialogs" / name) &&
            !fs::is_regular_file(languageDirectory / "Commentary" / name))
            throw std::runtime_error("Localized level '" + cane::utf8(sourceLanguagePath) +
                                     "' references missing audio '" + name + "'.");
}

// BrxRebuildService.RebuildAsync: one retail payload -> PCBRX v17 -> single-language v19.
inline void RebuildLevel(const std::vector<uint8_t>& decompressed, const fs::path& outputPath) {
    const fs::path directory = outputPath.parent_path();
    fs::create_directories(directory);
    const std::string stem = "." + cane::utf8(outputPath.stem()) + "." + UniqueStem();
    const fs::path temporary = directory / (stem + ".brx");
    const fs::path rebuilt = directory / (stem + "_Rebuilt.brx");
    try {
        WriteAll(temporary, decompressed);
        RunTool({cane::utf8(Tool("Sly1BrxRebuilder")), cane::utf8(temporary)});
        ValidatePcBrx(rebuilt, false, 17);
        WriteUnifiedContainer({rebuilt}, outputPath, {0}, 0);
    } catch (...) {
        TryRemove(temporary);
        TryRemove(rebuilt);
        throw;
    }
    TryRemove(temporary);
    TryRemove(rebuilt);
}

// BrxRebuildService.RebuildLocalizedCombinedAsync: the per-language retail levels of one level -> one v19.
inline void RebuildLocalizedCombined(const std::vector<fs::path>& languagePaths, const fs::path& outputPath,
                                     Region region, const fs::path& levelIdentityMap) {
    const char* regionArgument = region == Region::Japanese ? "japanese" : region == Region::Korean ? "korean" : "pal";
    const fs::path directory = outputPath.parent_path();
    fs::create_directories(directory);
    std::vector<fs::path> temporaries, rebuilt;
    auto cleanup = [&] {
        for (const auto& p : temporaries) TryRemove(p);
        for (const auto& p : rebuilt) TryRemove(p);
    };
    try {
        if (region == Region::Japanese) {
            if (languagePaths.size() != 2) throw std::runtime_error("Japanese extraction requires exactly two voice sources.");
            if (ReadJapaneseSourceLocalizationTables(languagePaths[0]) != ReadJapaneseSourceLocalizationTables(languagePaths[1]))
                throw std::runtime_error("Japanese voice slots contain different UI/subtitle reference tables; refusing to "
                                         "combine them as voice-only variants.");
        }
        for (const auto& source : languagePaths) {
            const std::string stem = "." + cane::utf8(outputPath.stem()) + "." + UniqueStem();
            temporaries.push_back(directory / (stem + ".brx"));
            fs::copy_file(source, temporaries.back(), fs::copy_options::overwrite_existing);
            rebuilt.push_back(directory / (stem + "_Rebuilt.brx"));
        }
        for (std::size_t i = 0; i < temporaries.size(); ++i) {
            std::vector<std::string> arguments = {cane::utf8(Tool("Sly1BrxRebuilder")), cane::utf8(temporaries[i]),
                                                  regionArgument};
            if (!levelIdentityMap.empty()) arguments.push_back(cane::utf8(levelIdentityMap));
            RunTool(arguments);
            ValidatePcBrx(rebuilt[i], true, 17);
        }
        for (std::size_t i = 0; i < rebuilt.size(); ++i) ValidateReferencedAudio(rebuilt[i], languagePaths[i]);
        if (region == Region::Japanese && ReadLocalizationPayload(rebuilt[0]) != ReadLocalizationPayload(rebuilt[1]))
            throw std::runtime_error("Japanese voice slots contain different localization tables; refusing to build a "
                                     "package whose subtitles could change with the voice selector.");
        std::vector<uint32_t> ids;
        for (uint32_t i = 0; i < rebuilt.size(); ++i) ids.push_back(i);
        WriteUnifiedContainer(rebuilt, outputPath, ids, 0);
        for (const auto& source : languagePaths) TryRemove(source);
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}

// ExtractionManifestService: the runtime's description of the package.
inline void WriteExtractionManifest(const fs::path& outputDirectory, const cane::Sly1Disc& disc) {
    cane::json languages, voices;
    const Region region = RegionOf(disc);
    std::string game;
    switch (region) {
    case Region::NtscU: game = "Sly Cooper and the Thievius Raccoonus"; languages = {"English"}; break;
    case Region::Pal: game = "Sly Raccoon"; languages = {"English", "French", "German", "Spanish", "Italian"}; break;
    case Region::Japanese: game = "Kaitou Sly Cooper"; languages = {"Japanese"}; voices = {"Japanese", "English"}; break;
    case Region::Korean: game = disc.title; languages = {"Korean"}; break; // Korean text and dub
    }
    cane::json manifest = {{"schemaVersion", 1},       {"game", game},
                           {"region", disc.region},    {"serial", disc.serial},
                           {"levelFormat", "PCBRX"},   {"levelFormatVersion", 19},
                           {"languages", languages}};
    if (!voices.is_null()) manifest["voiceLanguages"] = voices;
    std::ofstream out(outputDirectory / "extraction_manifest.json", std::ios::trunc);
    out << manifest.dump(2) << '\n';
    out.close();
    if (!out) throw std::runtime_error("Cannot write extraction_manifest.json");
}

// Sly1PalExtractionService.ExtractAsync: runs Sly1PalExtractor, then combines the per-language levels.
using ProgressFunction = std::function<void(const std::string& stage, std::size_t done, std::size_t total, const std::string& item)>;
inline void ExtractLocalized(const fs::path& iso, const fs::path& outputDirectory, Region region,
                             const ProgressFunction& progress) {
    const std::vector<std::string> languages = region == Region::Japanese ? std::vector<std::string>{"Japanese", "English"}
        : region == Region::Korean ? std::vector<std::string>{"Korean"}
        : std::vector<std::string>{"English", "French", "German", "Spanish", "Italian"};
    const char* regionArgument = region == Region::Japanese ? "japanese" : region == Region::Korean ? "korean" : "pal";
    const std::size_t extracted = languages.size() * kLevelCount;
    const std::size_t total = extracted + kLevelCount;
    std::size_t done = 0;
    const std::regex levelLine(R"(^\[(English|French|German|Spanish|Italian|Japanese|Korean)\s+(\d+)/46\])");
    RunTool({cane::utf8(Tool("Sly1PalExtractor")), "extract", cane::utf8(iso), cane::utf8(outputDirectory),
             regionArgument},
            [&](const std::string& line) {
                std::smatch match;
                if (std::regex_search(line, match, levelLine))
                    progress("levels", ++done, total, match[1].str() + " / level " + match[2].str() + " of 46");
            });
    const fs::path worlds = outputDirectory / "Worlds";
    const fs::path identityMap = outputDirectory / ".pal-level-identities.txt";
    const bool usesIdentityMap = region == Region::Pal || region == Region::Korean;
    if (usesIdentityMap && !fs::is_regular_file(identityMap))
        throw std::runtime_error("The PAL extractor did not produce its level identity map.");
    // Group <Level>/<Language>/<Level>.brx by <Level>/<Level>.brx.
    std::map<fs::path, std::map<std::string, fs::path>> groups;
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(worlds)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".brx") continue;
        const std::string parent = cane::utf8(entry.path().parent_path().filename());
        if (std::find(languages.begin(), languages.end(), parent) == languages.end()) continue;
        groups[entry.path().parent_path().parent_path() / entry.path().filename()][parent] = entry.path();
        ++count;
    }
    if (count != extracted)
        throw std::runtime_error("Expected " + std::to_string(extracted) + " level variants, but found " + std::to_string(count) + ".");
    if (groups.size() != kLevelCount)
        throw std::runtime_error("Expected 46 level groups, but found " + std::to_string(groups.size()) + ".");
    std::size_t index = 0;
    for (const auto& [output, files] : groups) {
        std::vector<fs::path> paths;
        for (const auto& language : languages) {
            auto it = files.find(language);
            if (it == files.end()) throw std::runtime_error("Level '" + cane::utf8(output) + "' is missing " + language + ".");
            paths.push_back(it->second);
        }
        progress("levels", extracted + index, total, cane::utf8(fs::relative(output, worlds)));
        RebuildLocalizedCombined(paths, output, region, usesIdentityMap ? identityMap : fs::path());
        ++index;
    }
    progress("levels", total, total, "");
}

} // namespace pcbrx
