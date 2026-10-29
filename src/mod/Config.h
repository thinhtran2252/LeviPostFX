#pragma once

#include <string_view>
#include <pl/Config.hpp>

namespace clange_me {

struct ModConfig {
    int version = 1;
    bool enabled = true;
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace clange_me

template <>
struct pl::config::Schema<clange_me::ModConfig> {
    static constexpr std::string_view title = "VHS Shader";
    static constexpr std::string_view description =
        "VHS Shader post-processing settings.";

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Version", .readOnly = true};

        if (name == "enabled")
            return {
                .title = "Chromatic Aberration",
                .description = "Enable or disable RGB edge separation."
            };

        return {};
    }
};