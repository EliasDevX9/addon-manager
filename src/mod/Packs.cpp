#include "mod/Packs.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>

#include <nlohmann/json.hpp>

#include "mod/Zip.h"

namespace addon {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr std::uint64_t kMaxManifestBytes = 1024 * 1024;
constexpr std::uint64_t kMaxEntryBytes = 512ull * 1024 * 1024;
constexpr std::uint64_t kMaxTotalBytes = 2048ull * 1024 * 1024;
constexpr int kMaxNestedDepth = 2;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool endsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool startsWith(const std::string &s, const std::string &prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string getString(const json &j, const char *key) {
    if (!j.is_object() || !j.contains(key))
        return {};
    const json &v = j[key];
    return v.is_string() ? v.get<std::string>() : std::string{};
}

bool parseVersion(const json &v, std::array<int, 3> &out) {
    out = {1, 0, 0};
    if (v.is_array() && v.size() >= 3) {
        for (std::size_t i = 0; i < 3; ++i) {
            if (!v[i].is_number_integer())
                return false;
            out[i] = v[i].get<int>();
        }
        return true;
    }
    if (v.is_string()) {
        std::stringstream ss(v.get<std::string>());
        std::string part;
        std::size_t i = 0;
        while (i < 3 && std::getline(ss, part, '.')) {
            try {
                out[i++] = std::stoi(part);
            } catch (...) {
                return false;
            }
        }
        return i == 3;
    }
    return false;
}

bool readFile(const fs::path &path, std::string &out, std::uint64_t maxBytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return out.size() <= maxBytes;
}

bool writeFileAtomic(const fs::path &path, const std::string &content, std::string &error) {
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "no se pudo escribir " + tmp.string();
            return false;
        }
        out << content;
        if (!out) {
            error = "error escribiendo " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        // Some storage providers refuse rename-over; fall back to remove + rename.
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
        if (ec) {
            error = "no se pudo reemplazar " + path.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

std::string stripBom(std::string s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);
    return s;
}

// Resolves "pack.name" style lang keys through texts/en_US.lang (best effort).
std::string resolvePackName(const fs::path &dir, const std::string &raw,
                            const std::string &fallback) {
    if (!startsWith(raw, "pack.") && !raw.empty())
        return raw;
    std::string text;
    for (const char *lang : {"en_US.lang", "en_GB.lang"}) {
        if (!readFile(dir / "texts" / lang, text, kMaxManifestBytes))
            continue;
        std::stringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (startsWith(line, "pack.name=")) {
                std::string value = line.substr(10);
                const auto hash = value.find("\t#");
                if (hash != std::string::npos)
                    value.erase(hash);
                if (!value.empty())
                    return value;
            }
        }
    }
    return fallback;
}

struct ParsedManifest {
    PackInfo pack;
    bool skip = false;
    std::string skipReason;
};

bool parseManifest(const std::string &textIn, ParsedManifest &out) {
    const std::string text = stripBom(textIn);
    const json m = json::parse(text, nullptr, false, true);
    if (m.is_discarded() || !m.is_object() || !m.contains("header"))
        return false;
    const json &header = m["header"];
    if (!header.is_object())
        return false;

    PackInfo &pack = out.pack;
    pack.uuid = lower(getString(header, "uuid"));
    pack.name = getString(header, "name");
    if (pack.uuid.empty())
        return false;
    if (header.contains("version"))
        parseVersion(header["version"], pack.version);

    bool hasResources = false;
    bool hasBehavior = false;
    bool hasSkin = false;
    bool hasTemplate = false;
    if (m.contains("modules") && m["modules"].is_array()) {
        for (const json &module : m["modules"]) {
            const std::string type = lower(getString(module, "type"));
            if (type == "resources" || type == "interface")
                hasResources = true;
            else if (type == "skin_pack")
                hasSkin = true;
            else if (type == "world_template")
                hasTemplate = true;
            else
                hasBehavior = true;
        }
    }
    if (!hasResources && !hasBehavior && (hasSkin || hasTemplate)) {
        out.skip = true;
        out.skipReason = hasSkin ? "skin pack" : "world template";
    }
    pack.kind = (hasResources && !hasBehavior) ? PackKind::Resource : PackKind::Behavior;

    if (m.contains("dependencies") && m["dependencies"].is_array()) {
        for (const json &dep : m["dependencies"]) {
            const std::string uuid = lower(getString(dep, "uuid"));
            if (!uuid.empty())
                pack.dependencies.push_back(uuid);
        }
    }
    return true;
}

std::string sanitizeFolderName(const std::string &name, const std::string &uuid) {
    std::string clean;
    const std::string plain = stripFormatting(name);
    for (unsigned char c : plain) {
        if (std::isalnum(c) || c == '-' || c == '_')
            clean.push_back(static_cast<char>(c));
        else if (c == ' ')
            clean.push_back('_');
        if (clean.size() >= 40)
            break;
    }
    if (clean.empty())
        clean = "pack";
    return clean + "_" + uuid.substr(0, 8);
}

void scanPackRoot(const fs::path &root, std::vector<PackInfo> &out) {
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec))
            continue;
        const fs::path dir = it->path();
        std::string text;
        if (!readFile(dir / "manifest.json", text, kMaxManifestBytes))
            continue;
        ParsedManifest parsed;
        if (!parseManifest(text, parsed) || parsed.skip)
            continue;
        parsed.pack.dir = dir;
        parsed.pack.name = resolvePackName(dir, parsed.pack.name, dir.filename().string());
        if (parsed.pack.name.empty())
            parsed.pack.name = dir.filename().string();
        const bool duplicate = std::any_of(out.begin(), out.end(), [&](const PackInfo &p) {
            return p.uuid == parsed.pack.uuid && p.kind == parsed.pack.kind;
        });
        if (!duplicate)
            out.push_back(std::move(parsed.pack));
    }
}

bool isComMojang(const fs::path &p) {
    std::error_code ec;
    return !p.empty() && (fs::is_directory(p / "minecraftWorlds", ec) ||
                          fs::is_directory(p / "behavior_packs", ec) ||
                          fs::is_directory(p / "resource_packs", ec));
}

fs::file_time_type newestWrite(const fs::path &dir) {
    std::error_code ec;
    fs::file_time_type best = fs::file_time_type::min();
    auto consider = [&](const fs::path &p) {
        std::error_code e;
        const auto t = fs::last_write_time(p, e);
        if (!e && t > best)
            best = t;
    };
    consider(dir);
    consider(dir / "level.dat");
    for (fs::directory_iterator it(dir / "db", ec), end; !ec && it != end; it.increment(ec))
        consider(it->path());
    return best;
}

bool moveDirectory(const fs::path &from, const fs::path &to, std::string &error) {
    std::error_code ec;
    fs::remove_all(to, ec);
    ec.clear();
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    fs::rename(from, to, ec);
    if (!ec)
        return true;
    ec.clear();
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "no se pudo mover el pack: " + ec.message();
        return false;
    }
    fs::remove_all(from, ec);
    return true;
}

struct ImportContext {
    fs::path com;
    fs::path tmpRoot;
    std::vector<PackInfo> installed;
    ImportResult result;
    std::uint64_t totalBytes = 0;
    int counter = 0;
};

void importZip(ImportContext &ctx, const fs::path &zipPath, int depth) {
    zip::Reader reader;
    std::string error;
    if (!reader.open(zipPath, error)) {
        ctx.result.errors.push_back(zipPath.filename().string() + ": " + error);
        return;
    }

    // 1) manifest roots
    std::vector<std::string> roots;
    for (const auto &entry : reader.entries()) {
        if (entry.isDirectory())
            continue;
        const std::string name = lower(entry.name);
        if (name == "manifest.json")
            roots.push_back("");
        else if (endsWith(name, "/manifest.json"))
            roots.push_back(entry.name.substr(0, entry.name.size() - 13));
    }
    std::sort(roots.begin(), roots.end(),
              [](const std::string &a, const std::string &b) { return a.size() < b.size(); });
    std::vector<std::string> topRoots;
    for (const auto &r : roots) {
        const bool nested = std::any_of(topRoots.begin(), topRoots.end(), [&](const std::string &t) {
            return t.empty() || startsWith(r, t);
        });
        if (!nested)
            topRoots.push_back(r);
    }

    auto insideRoot = [&](const std::string &name) {
        return std::any_of(topRoots.begin(), topRoots.end(), [&](const std::string &t) {
            return t.empty() || startsWith(name, t);
        });
    };

    // 2) each pack
    for (const auto &root : topRoots) {
        const zip::Entry *manifestEntry = nullptr;
        for (const auto &entry : reader.entries()) {
            if (entry.name == root + "manifest.json" ||
                lower(entry.name) == lower(root + "manifest.json")) {
                manifestEntry = &entry;
                break;
            }
        }
        if (!manifestEntry)
            continue;
        std::string manifestText;
        if (!reader.readToMemory(*manifestEntry, manifestText, kMaxManifestBytes, error)) {
            ctx.result.errors.push_back(root + "manifest.json: " + error);
            continue;
        }
        ParsedManifest parsed;
        if (!parseManifest(manifestText, parsed)) {
            ctx.result.errors.push_back(root + "manifest.json: manifest invalido");
            continue;
        }
        if (parsed.skip) {
            ctx.result.errors.push_back("Ignorado (" + parsed.skipReason + "): " + parsed.pack.name);
            continue;
        }

        const bool behavior = parsed.pack.kind == PackKind::Behavior;

        // Extract to a staging folder first; the final folder name depends on the resolved name.
        const fs::path stage = ctx.tmpRoot / ("pack_" + std::to_string(++ctx.counter));
        std::error_code ec;
        fs::remove_all(stage, ec);
        bool ok = true;
        for (const auto &entry : reader.entries()) {
            if (entry.isDirectory() || !startsWith(entry.name, root))
                continue;
            const std::string rel = entry.name.substr(root.size());
            if (rel.empty() || !zip::isSafeRelativePath(rel))
                continue;
            if (entry.size > kMaxEntryBytes || ctx.totalBytes + entry.size > kMaxTotalBytes) {
                error = "el addon es demasiado grande";
                ok = false;
                break;
            }
            ctx.totalBytes += entry.size;
            if (!reader.extractToFile(entry, stage / rel, error)) {
                error = rel + ": " + error;
                ok = false;
                break;
            }
        }

        const std::string resolvedName =
            ok ? resolvePackName(stage, parsed.pack.name, "pack") : parsed.pack.name;
        parsed.pack.name = resolvedName;

        fs::path dest;
        for (const auto &p : ctx.installed) {
            if (p.uuid == parsed.pack.uuid && p.kind == parsed.pack.kind) {
                dest = p.dir; // update in place
                break;
            }
        }
        if (dest.empty()) {
            dest = ctx.com / (behavior ? "behavior_packs" : "resource_packs") /
                   sanitizeFolderName(resolvedName, parsed.pack.uuid);
        }
        if (ok && !moveDirectory(stage, dest, error))
            ok = false;
        fs::remove_all(stage, ec);

        if (!ok) {
            ctx.result.errors.push_back(parsed.pack.name + ": " + error);
            continue;
        }

        const std::string displayName = resolvedName;
        ctx.result.imported++;
        ctx.result.importedNames.push_back(stripFormatting(displayName) +
                                           (behavior ? " (BP)" : " (RP)"));
        parsed.pack.dir = dest;
        parsed.pack.name = displayName;
        const bool known = std::any_of(ctx.installed.begin(), ctx.installed.end(),
                                       [&](const PackInfo &p) {
                                           return p.uuid == parsed.pack.uuid &&
                                                  p.kind == parsed.pack.kind;
                                       });
        if (!known)
            ctx.installed.push_back(parsed.pack);
    }

    // 3) nested archives that are not part of a pack (.mcaddon containing .mcpack files)
    if (depth >= kMaxNestedDepth)
        return;
    for (const auto &entry : reader.entries()) {
        if (entry.isDirectory() || insideRoot(entry.name))
            continue;
        const std::string name = lower(entry.name);
        if (!(endsWith(name, ".mcpack") || endsWith(name, ".mcaddon") || endsWith(name, ".zip")))
            continue;
        if (!zip::isSafeRelativePath(entry.name) || entry.size > kMaxEntryBytes)
            continue;
        const fs::path nested = ctx.tmpRoot / ("nested_" + std::to_string(++ctx.counter) + ".zip");
        if (!reader.extractToFile(entry, nested, error)) {
            ctx.result.errors.push_back(entry.name + ": " + error);
            continue;
        }
        importZip(ctx, nested, depth + 1);
        std::error_code ec;
        fs::remove(nested, ec);
    }
}

fs::path worldPackFile(const fs::path &worldDir, PackKind kind) {
    return worldDir /
           (kind == PackKind::Behavior ? "world_behavior_packs.json" : "world_resource_packs.json");
}

json readPackArray(const fs::path &file) {
    std::string text;
    if (!readFile(file, text, kMaxManifestBytes))
        return json::array();
    const json parsed = json::parse(stripBom(text), nullptr, false, true);
    if (parsed.is_discarded() || !parsed.is_array())
        return json::array();
    return parsed;
}

} // namespace

std::string stripFormatting(const std::string &text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        // '§' is 0xC2 0xA7 in UTF-8; drop it and the following code character.
        if (i + 2 < text.size() && static_cast<unsigned char>(text[i]) == 0xC2 &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            i += 2;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

fs::path findComMojang(const fs::path &modDir, const std::string &overridePath) {
    std::error_code ec;
    if (!overridePath.empty()) {
        const fs::path p(overridePath);
        if (isComMojang(p))
            return p;
        if (isComMojang(p / "com.mojang"))
            return p / "com.mojang";
        if (isComMojang(p / "games" / "com.mojang"))
            return p / "games" / "com.mojang";
    }

    // Walk up from the mod directory.
    fs::path p = modDir;
    for (int i = 0; i < 10 && !p.empty(); ++i) {
        if (p.filename() == "com.mojang" && isComMojang(p))
            return p;
        for (const fs::path &candidate : {p / "games" / "com.mojang",
                                          p / "files" / "games" / "com.mojang"}) {
            if (isComMojang(candidate))
                return candidate;
        }
        const fs::path parent = p.parent_path();
        if (parent == p)
            break;
        p = parent;
    }

    // Well-known Android locations; pick the one with the most recent activity.
    std::vector<fs::path> candidates;
    for (const char *base : {"/storage/emulated/0", "/sdcard"}) {
        const fs::path data = fs::path(base) / "Android" / "data";
        for (fs::directory_iterator it(data, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path c = it->path() / "files" / "games" / "com.mojang";
            if (isComMojang(c))
                candidates.push_back(c);
        }
        ec.clear();
        const fs::path direct = fs::path(base) / "games" / "com.mojang";
        if (isComMojang(direct))
            candidates.push_back(direct);
    }
    fs::path best;
    fs::file_time_type bestTime = fs::file_time_type::min();
    for (const auto &c : candidates) {
        std::error_code e;
        const auto t = fs::last_write_time(c / "minecraftWorlds", e);
        if (best.empty() || (!e && t > bestTime)) {
            best = c;
            bestTime = e ? fs::file_time_type::min() : t;
        }
    }
    return best;
}

std::vector<WorldInfo> listWorlds(const fs::path &comMojang) {
    std::vector<WorldInfo> worlds;
    std::error_code ec;
    const fs::path root = comMojang / "minecraftWorlds";
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec))
            continue;
        WorldInfo w;
        w.dir = it->path();
        w.folder = w.dir.filename().string();
        std::string name;
        if (readFile(w.dir / "levelname.txt", name, 4096)) {
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
                name.pop_back();
        }
        w.name = name.empty() ? w.folder : stripFormatting(name);
        w.lastWrite = newestWrite(w.dir);
        worlds.push_back(std::move(w));
    }
    std::sort(worlds.begin(), worlds.end(),
              [](const WorldInfo &a, const WorldInfo &b) { return a.lastWrite > b.lastWrite; });
    return worlds;
}

std::vector<PackInfo> listInstalledPacks(const fs::path &comMojang) {
    std::vector<PackInfo> packs;
    for (const char *folder : {"behavior_packs", "resource_packs", "development_behavior_packs",
                               "development_resource_packs"}) {
        scanPackRoot(comMojang / folder, packs);
    }
    std::sort(packs.begin(), packs.end(), [](const PackInfo &a, const PackInfo &b) {
        return lower(a.name) < lower(b.name);
    });
    return packs;
}

ImportResult importArchive(const fs::path &comMojang, const fs::path &archive) {
    ImportContext ctx;
    ctx.com = comMojang;
    ctx.tmpRoot = comMojang / ".addon_manager_tmp";
    ctx.installed = listInstalledPacks(comMojang);

    std::error_code ec;
    fs::remove_all(ctx.tmpRoot, ec);
    fs::create_directories(ctx.tmpRoot, ec);
    if (ec) {
        ctx.result.errors.push_back("no se pudo crear carpeta temporal: " + ec.message());
        return ctx.result;
    }

    importZip(ctx, archive, 0);
    if (ctx.result.imported == 0 && ctx.result.errors.empty())
        ctx.result.errors.push_back("no encontre ningun pack (manifest.json) en el archivo");

    fs::remove_all(ctx.tmpRoot, ec);
    return ctx.result;
}

std::set<std::string> activePackUuids(const fs::path &worldDir, PackKind kind) {
    std::set<std::string> uuids;
    for (const json &entry : readPackArray(worldPackFile(worldDir, kind))) {
        const std::string id = lower(getString(entry, "pack_id"));
        if (!id.empty())
            uuids.insert(id);
    }
    return uuids;
}

namespace {

bool applyOne(const fs::path &worldDir, const PackInfo &pack, bool active, bool &changed,
              std::string &error) {
    const fs::path file = worldPackFile(worldDir, pack.kind);
    const json current = readPackArray(file);

    json next = json::array();
    bool found = false;
    for (const json &entry : current) {
        const std::string id = lower(getString(entry, "pack_id"));
        if (id == pack.uuid) {
            found = true;
            if (!active)
                continue; // removing
            next.push_back(json{{"pack_id", pack.uuid},
                                {"version", json::array({pack.version[0], pack.version[1],
                                                         pack.version[2]})}});
            continue;
        }
        next.push_back(entry);
    }
    if (active && !found) {
        next.push_back(json{{"pack_id", pack.uuid},
                            {"version", json::array({pack.version[0], pack.version[1],
                                                     pack.version[2]})}});
    }

    changed = active ? !found : found;
    if (!changed)
        return true;

    std::error_code ec;
    if (fs::exists(file, ec)) {
        const fs::path backup = file.string() + ".bak";
        if (!fs::exists(backup, ec))
            fs::copy_file(file, backup, ec);
    }
    return writeFileAtomic(file, next.dump(2) + "\n", error);
}

} // namespace

bool setPackActive(const fs::path &worldDir, const PackInfo &pack, bool active,
                   const std::vector<PackInfo> &installed, int &changed, std::string &error) {
    changed = 0;
    std::set<std::string> visited;
    std::function<bool(const PackInfo &)> apply = [&](const PackInfo &p) {
        if (!visited.insert(p.uuid + (p.kind == PackKind::Behavior ? ":b" : ":r")).second)
            return true;
        bool did = false;
        if (!applyOne(worldDir, p, active, did, error))
            return false;
        if (did)
            ++changed;
        if (active) {
            for (const auto &dep : p.dependencies) {
                for (const auto &candidate : installed) {
                    if (candidate.uuid == dep && !apply(candidate))
                        return false;
                }
            }
        }
        return true;
    };
    return apply(pack);
}

} // namespace addon
