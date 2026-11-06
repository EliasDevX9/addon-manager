#include "mod/Config.h"

namespace addon_manager {

nlohmann::json makeDefaultConfigJson() { return pl::config::defaultJson(ModConfig{}); }

nlohmann::json makeConfigSchemaJson() { return pl::config::schema(ModConfig{}); }

} // namespace addon_manager
