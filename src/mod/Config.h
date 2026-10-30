#pragma once

#include <string_view>
#include <pl/Config.hpp>

namespace clange_me {

struct ModConfig {
    int version = 2;
    bool enabled = true;

    bool distortion_enabled = true;
    float distortion_strength = 0.035f;
    // Signed zoom coefficient: -1 = zoom out, 0 = neutral, +1 = zoom in.
    float distortion_zoom = 0.0f;

    bool chromatic_enabled = true;
    float chromatic_intensity = 1.0f;

    bool scanlines_enabled = true;
    float scanline_intensity = 0.035f;

    bool desaturation_enabled = false;
    float desaturation_intensity = 0.15f;

    bool dark_noise_enabled = false;
    float dark_noise_intensity = 0.035f;

    bool aspect_ratio_4_3 = false;
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace clange_me

template <>
struct pl::config::Schema<clange_me::ModConfig> {
    static constexpr std::string_view title = "VHS Shader";
    static constexpr std::string_view description =
        "Screen-space VHS post-processing and camera controls.";

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Version", .readOnly = true};
        if (name == "enabled")
            return {
                .title = "VHS Effects",
                .description = "Enable or disable the complete post-processing pass."
            };
        if (name == "distortion_enabled")
            return {
                .title = "Screen Distortion",
                .description = "Enable the lens/screen distortion effect."
            };
        if (name == "distortion_strength")
            return {
                .title = "Distortion Strength",
                .description = "Intensity of the screen distortion.",
                .minimum = 0.0,
                .maximum = 0.08
            };
        if (name == "distortion_zoom")
            return {
                .title = "Distortion Zoom",
                .description = "Signed zoom: -1 zooms out, 0 is neutral, +1 zooms in.",
                .minimum = -1.0,
                .maximum = 1.0
            };
        if (name == "chromatic_enabled")
            return {
                .title = "Chromatic Aberration",
                .description = "Enable RGB channel separation across the screen."
            };
        if (name == "chromatic_intensity")
            return {
                .title = "Chromatic Aberration Intensity",
                .description = "Controls the full-screen RGB channel offset.",
                .minimum = 0.0,
                .maximum = 3.0
            };
        if (name == "scanlines_enabled")
            return {
                .title = "Scanlines",
                .description = "Enable the VHS scanline effect."
            };
        if (name == "scanline_intensity")
            return {
                .title = "Scanline Intensity",
                .description = "Controls scanline contrast.",
                .minimum = 0.0,
                .maximum = 0.08
            };
        if (name == "desaturation_enabled")
            return {
                .title = "Desaturation",
                .description = "Enable subtle color desaturation."
            };
        if (name == "desaturation_intensity")
            return {
                .title = "Desaturation Intensity",
                .description = "Controls how much color is removed.",
                .minimum = 0.0,
                .maximum = 1.0
            };
        if (name == "dark_noise_enabled")
            return {
                .title = "Dark Area Noise",
                .description = "Add stronger camera noise to darker image areas."
            };
        if (name == "dark_noise_intensity")
            return {
                .title = "Dark Area Noise Intensity",
                .description = "Controls the amount of noise in dark areas.",
                .minimum = 0.0,
                .maximum = 0.15
            };
        if (name == "aspect_ratio_4_3")
            return {
                .title = "4:3 Ratio",
                .description = "Display a centred 4:3 camera image with black bars; no stretching."
            };
        return {};
    }
};
