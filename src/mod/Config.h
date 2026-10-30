#pragma once

#include <string_view>
#include <pl/Config.hpp>

namespace clange_me {

struct ModConfig {
    int version = 1;

    // Main Levi Mod Menu module switch.
    bool enabled = true;

    // Screen distortion and the post-distortion crop/zoom.
    bool distortion_enabled = true;
    float distortion_strength = 0.035f;
    float distortion_zoom = 1.025f;

    // Chromatic aberration: a non-zero baseline offset keeps it visible
    // in the centre, while the shader adds a stronger radial edge component.
    bool chromatic_enabled = true;
    float chromatic_intensity = 1.0f;

    // Scanline control.
    bool scanlines_enabled = true;
    float scanline_intensity = 0.035f;

    // Centre a 4:3 image inside the physical display, without stretching.
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
                .title = "Distortion Border Zoom",
                .description = "Small centred zoom applied after distortion to crop the warped outer edge.",
                .minimum = 1.0,
                .maximum = 1.10
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

        if (name == "aspect_ratio_4_3")
            return {
                .title = "4:3 Ratio",
                .description = "Display a centred 4:3 camera image with black side/top bars; no stretching."
            };

        return {};
    }
};
