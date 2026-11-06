#include "mod/AddonManagerMod.h"

#include <algorithm>
#include <filesystem>

#include <pl/Input.hpp>
#include <pl/ModMenu.hpp>

namespace addon_manager {

namespace {

constexpr const char *kModuleId = "addon_manager";
constexpr std::size_t kMaxListedPacks = 150;

std::string truncateUtf8(const std::string &text, std::size_t maxBytes) {
    if (text.size() <= maxBytes)
        return text;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    return text.substr(0, cut);
}

bool isOn(std::string_view value) { return value == "true" || value == "1" || value == "on"; }

} // namespace

AddonManagerMod &AddonManagerMod::instance() {
    static AddonManagerMod instance;
    return instance;
}

AddonManagerMod::AddonManagerMod() : mSelf(*ll::mod::NativeMod::current()) {}

bool AddonManagerMod::load() {
    auto &self = getSelf();
    self.getLogger().debug("Loading...");

    std::error_code ec;
    std::filesystem::create_directories(self.getDataDir(), ec);
    std::filesystem::create_directories(self.getConfigDir(), ec);

    mConfigFile.emplace();
    if (!mConfigFile->load()) {
        self.getLogger().warn("Failed to load typed config");
        return false;
    }
    mConfig = mConfigFile->value();
    return true;
}

bool AddonManagerMod::enable() {
    auto &self = getSelf();
    rescan();

    const bool ok = pl::modmenu::ModuleBuilder(kModuleId, "Addon Manager")
                        .description("Importa addons y activalos en tu mundo sin salir del juego.")
                        .defaultEnabled(true)
                        .onConfigChanged([this](std::string_view, std::string_view key,
                                                std::string_view value) {
                            onConfigChanged(key, value);
                        })
                        .registerModule();
    if (!ok) {
        self.getLogger().error("Could not register the Mod Menu module");
        return false;
    }
    mRegistered = true;

    if (mComMojang.empty()) {
        setStatus("No encontre la carpeta com.mojang. Escribe la ruta en la config del mod.");
    } else {
        setStatus("Listo. Mundo: " + (mSelectedWorld.empty() ? std::string("(ninguno)")
                                                              : mSelectedWorld));
    }
    publishMenu();
    self.getLogger().info("Addon Manager ready (com.mojang: {})", mComMojang.string());
    return true;
}

bool AddonManagerMod::disable() {
    getSelf().getLogger().debug("Disabling...");
    if (mWorker.joinable())
        mWorker.join();
    if (mRegistered.exchange(false)) {
        pl::modmenu::clearConfigSchema(kModuleId);
        pl::modmenu::unregisterModule(kModuleId);
    }
    return true;
}

bool AddonManagerMod::unload() {
    getSelf().getLogger().debug("Unloading...");
    mConfigFile.reset();
    return true;
}

void AddonManagerMod::setStatus(std::string text) {
    std::lock_guard<std::mutex> lock(mMutex);
    mStatus = truncateUtf8(text, 900);
}

void AddonManagerMod::rescan() {
    std::lock_guard<std::mutex> lock(mMutex);
    mComMojang = addon::findComMojang(getSelf().getModDir(), mConfig.gamePath);
    mWorlds.clear();
    mPacks.clear();
    if (mComMojang.empty())
        return;

    mWorlds = addon::listWorlds(mComMojang);
    mPacks = addon::listInstalledPacks(mComMojang);

    const bool stillExists = std::any_of(mWorlds.begin(), mWorlds.end(),
                                         [&](const addon::WorldInfo &w) {
                                             return w.folder == mSelectedWorld;
                                         });
    if (!stillExists)
        mSelectedWorld = mWorlds.empty() ? std::string{} : mWorlds.front().folder; // newest
}

void AddonManagerMod::onConfigChanged(std::string_view key, std::string_view value) {
    if (key == "import") {
        if (value != "false" && value != "0")
            startImport();
    } else if (key == "refresh") {
        if (value != "false" && value != "0") {
            rescan();
            setStatus("Lista actualizada.");
            publishMenu();
        }
    } else if (key == "world") {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mSelectedWorld = std::string(value);
        }
        setStatus("Mundo seleccionado: " + std::string(value));
        publishMenu();
    } else if (key.rfind("bp_", 0) == 0 || key.rfind("rp_", 0) == 0) {
        togglePack(std::string(key), isOn(value));
    }
}

void AddonManagerMod::startImport() {
    if (mImporting.exchange(true)) {
        setStatus("Ya hay una importacion en curso.");
        publishMenu();
        return;
    }
    if (mComMojang.empty()) {
        mImporting = false;
        setStatus("No encontre com.mojang; configura la ruta primero.");
        publishMenu();
        return;
    }

    const bool launched = pl::input::openDocument("*/*", [this](const pl::input::DocumentResult &r) {
        if (!r.success) {
            mImporting = false;
            setStatus(r.error.empty() ? "Importacion cancelada." : "Importacion cancelada: " + r.error);
            publishMenu();
            return;
        }
        std::string filePath = r.path;
        std::string fileName = r.displayName;
        // Run off the UI thread: unzipping can take a while.
        if (mWorker.joinable())
            mWorker.join();
        mWorker = std::thread(&AddonManagerMod::runImport, this, std::move(filePath),
                              std::move(fileName));
    });
    if (!launched) {
        mImporting = false;
        setStatus("No pude abrir el selector de archivos.");
        publishMenu();
    }
}

void AddonManagerMod::runImport(std::string path, std::string displayName) {
    std::filesystem::path com;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        com = mComMojang;
    }
    const addon::ImportResult result = addon::importArchive(com, path);

    // The launcher hands us a cache copy; drop it when it really is a cache file.
    if (path.find("/cache/") != std::string::npos) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    rescan();

    std::string status;
    if (result.imported > 0) {
        status = "Importado: ";
        for (std::size_t i = 0; i < result.importedNames.size(); ++i)
            status += (i ? ", " : "") + result.importedNames[i];
        status += ". Tocalo abajo para activarlo.";
    } else {
        status = "No se importo nada de " + displayName + ".";
    }
    if (!result.errors.empty())
        status += " Avisos: " + result.errors.front();
    getSelf().getLogger().info("Import finished: {}", status);
    setStatus(status);
    mImporting = false;
    publishMenu();
}

void AddonManagerMod::togglePack(const std::string &key, bool active) {
    const bool behavior = key.rfind("bp_", 0) == 0;
    const std::string uuid = key.substr(3);

    std::string status;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto world = std::find_if(mWorlds.begin(), mWorlds.end(),
                                        [&](const addon::WorldInfo &w) {
                                            return w.folder == mSelectedWorld;
                                        });
        const auto pack = std::find_if(mPacks.begin(), mPacks.end(), [&](const addon::PackInfo &p) {
            return p.uuid == uuid &&
                   (p.kind == addon::PackKind::Behavior) == behavior;
        });
        if (world == mWorlds.end()) {
            status = "Elige un mundo primero.";
        } else if (pack == mPacks.end()) {
            status = "Ese pack ya no esta instalado.";
        } else {
            int changed = 0;
            std::string error;
            if (addon::setPackActive(world->dir, *pack, active, mPacks, changed, error)) {
                status = addon::stripFormatting(pack->name) + (active ? " activado" : " desactivado") +
                         " en \"" + world->name + "\".";
                if (changed > 1)
                    status += " (+" + std::to_string(changed - 1) + " dependencia(s))";
                status += " Guarda y vuelve a entrar al mundo para aplicarlo.";
            } else {
                status = "Error: " + error;
            }
        }
    }
    setStatus(status);
    publishMenu(); // also reverts the switch when the write failed
}

std::string AddonManagerMod::buildSchema() {
    using namespace pl::modmenu;
    std::lock_guard<std::mutex> lock(mMutex);

    ConfigSchemaBuilder builder;
    builder.category("addons", "Addons").defaultCategory("addons");

    auto section = [&](const std::string &id, const std::string &title) {
        ConfigNodeV2 node;
        node.id = id;
        node.category = "addons";
        node.title = title;
        node.type = ConfigControlTypeV2::Section;
        builder.node(std::move(node));
    };
    auto info = [&](const std::string &id, const std::string &title, const std::string &text) {
        ConfigNodeV2 node;
        node.id = id;
        node.category = "addons";
        node.title = title;
        node.description = truncateUtf8(text, 1000);
        node.type = ConfigControlTypeV2::Info;
        builder.node(std::move(node));
    };
    auto button = [&](const std::string &key, const std::string &title, const std::string &desc,
                      bool disabled) {
        ConfigNodeV2 node;
        node.id = "btn_" + key;
        node.key = key;
        node.category = "addons";
        node.title = title;
        node.description = desc;
        node.type = ConfigControlTypeV2::Button;
        node.disabled = disabled;
        builder.node(std::move(node));
    };

    info("status", "Estado", mStatus);
    info("path", "Carpeta del juego",
         mComMojang.empty() ? std::string("No encontrada. ") + addon::lastComMojangSearchLog()
                            : mComMojang.string() + " (" + std::to_string(mWorlds.size()) +
                                  " mundos, " + std::to_string(mPacks.size()) + " packs)");

    section("sec_import", "Importar");
    button("import", "Importar addon", ".mcaddon, .mcpack o .zip desde tu telefono",
           mComMojang.empty() || mImporting.load());
    button("refresh", "Actualizar lista", "Vuelve a leer mundos y packs", false);

    section("sec_world", "Mundo");
    if (mWorlds.empty()) {
        info("no_worlds", "Sin mundos", "No se encontraron mundos en minecraftWorlds.");
    } else {
        ConfigNodeV2 node;
        node.id = "world";
        node.key = "world";
        node.category = "addons";
        node.title = "Mundo donde activar";
        node.description = "Por defecto el que jugaste mas recientemente.";
        node.type = ConfigControlTypeV2::Choice;
        node.choiceStyle = ConfigChoiceStyleV2::Dropdown;
        node.defaultValue = mWorlds.front().folder;
        node.currentValue = mSelectedWorld;
        node.hasCurrentValue = true;
        for (const auto &w : mWorlds) {
            ConfigOptionV2 option;
            option.value = w.folder;
            option.label = truncateUtf8(w.name, 80);
            node.options.push_back(std::move(option));
        }
        builder.node(std::move(node));
    }

    const auto worldIt = std::find_if(mWorlds.begin(), mWorlds.end(), [&](const addon::WorldInfo &w) {
        return w.folder == mSelectedWorld;
    });
    std::set<std::string> activeBp;
    std::set<std::string> activeRp;
    if (worldIt != mWorlds.end()) {
        activeBp = addon::activePackUuids(worldIt->dir, addon::PackKind::Behavior);
        activeRp = addon::activePackUuids(worldIt->dir, addon::PackKind::Resource);
    }

    auto addPacks = [&](addon::PackKind kind, const std::string &sectionId,
                        const std::string &sectionTitle, const std::string &prefix,
                        const std::set<std::string> &active) {
        section(sectionId, sectionTitle);
        std::size_t count = 0;
        for (const auto &pack : mPacks) {
            if (pack.kind != kind)
                continue;
            if (++count > kMaxListedPacks)
                break;
            ConfigNodeV2 node;
            node.id = prefix + pack.uuid;
            node.key = prefix + pack.uuid;
            node.category = "addons";
            node.title = truncateUtf8(addon::stripFormatting(pack.name), 100);
            node.description = "v" + std::to_string(pack.version[0]) + "." +
                               std::to_string(pack.version[1]) + "." + std::to_string(pack.version[2]);
            node.type = ConfigControlTypeV2::Toggle;
            node.defaultValue = "false";
            node.currentValue = active.count(pack.uuid) ? "true" : "false";
            node.hasCurrentValue = true;
            node.disabled = worldIt == mWorlds.end();
            builder.node(std::move(node));
        }
        if (count == 0)
            info(sectionId + "_empty", "Vacio", "No hay packs de este tipo instalados.");
    };
    addPacks(addon::PackKind::Behavior, "sec_bp", "Behavior packs", "bp_", activeBp);
    addPacks(addon::PackKind::Resource, "sec_rp", "Resource packs", "rp_", activeRp);

    info("hint", "Importante",
         "Minecraft carga los packs al entrar al mundo. Despues de activar o quitar uno, guarda y "
         "sal del mundo y vuelve a entrar. Si juegas con amigos, el anfitrion debe reentrar.");
    return builder.toJson();
}

void AddonManagerMod::publishMenu() {
    if (!mRegistered)
        return;
    const std::string schema = buildSchema();
    if (!pl::modmenu::setConfigSchemaJson(kModuleId, schema))
        getSelf().getLogger().warn("Mod Menu rejected the schema");
}

} // namespace addon_manager
