#pragma once

#include <string>
#include <string_view>

#include <pl/Config.hpp>

namespace addon_manager {

struct ModConfig {
    int version = 1;
    // Leave empty for auto-detect. Example: /storage/emulated/0/Android/data/<pkg>/files/games/com.mojang
    std::string gamePath = "";
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace addon_manager

template <> struct pl::config::Schema<addon_manager::ModConfig> {
    static constexpr std::string_view title = "Addon Manager Config";
    static constexpr std::string_view description = {};

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Version", .readOnly = true};
        if (name == "gamePath")
            return {.title = "Ruta com.mojang",
                    .description = "Vacio = autodetectar. Carpeta games/com.mojang del juego."};
        return {};
    }
};
