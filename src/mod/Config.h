#pragma once

#include <string_view>
#include <pl/Config.hpp>

namespace clange_me {

struct ModConfig {
    int version = 6;
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

    // Deepen low-luminance areas without crushing normally lit regions.
    float shadow_darkness = 0.85f;

    // Reference-style analog signal noise, applied in YUV space.
    bool luma_noise_enabled = true;
    float luma_noise_intensity = 0.05f;
    bool chroma_noise_enabled = true;
    float chroma_noise_intensity = 0.20f;

    // Full-screen camera-space flashlight approximation.
    bool flashlight_enabled = true;
    float flashlight_intensity = 3.0f;
    float flashlight_range = 1.0f;

    bool bloom_enabled = true;
    float bloom_intensity = 0.5f;
    float bloom_threshold = 0.35f;

    bool aspect_ratio_4_3 = false;
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace clange_me

template <>
struct pl::config::Schema<clange_me::ModConfig> {
    static constexpr std::string_view title = "VHS Shader";
    static constexpr std::string_view description =
        "Screen-space VHS post-processing, camera overlays, and image controls.";

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
        if (name == "shadow_darkness")
            return {
                .title = "Shadow Darkness",
                .description = "Darkens low-light areas strongly while preserving normally lit pixels.",
                .minimum = 0.0,
                .maximum = 1.0
            };
        if (name == "luma_noise_enabled")
            return {
                .title = "Luma Noise",
                .description = "Add animated analog-style noise to image brightness."
            };
        if (name == "luma_noise_intensity")
            return {
                .title = "Luma Noise Strength",
                .description = "Controls brightness-channel noise strength.",
                .minimum = 0.0,
                .maximum = 1.0
            };
        if (name == "chroma_noise_enabled")
            return {
                .title = "Chroma Noise",
                .description = "Add animated analog-style noise to image color channels."
            };
        if (name == "chroma_noise_intensity")
            return {
                .title = "Chroma Noise Strength",
                .description = "Controls color-channel noise strength.",
                .minimum = 0.0,
                .maximum = 1.0
            };
        if (name == "flashlight_enabled")
            return {
                .title = "Flashlight",
                .description = "Enable a camera-centred post-processing flashlight beam."
            };
        if (name == "flashlight_intensity")
            return {
                .title = "Flashlight Intensity",
                .description = "Controls the brightness added by the flashlight beam.",
                .minimum = 3.0,
                .maximum = 10.0
            };
        if (name == "flashlight_range")
            return {
                .title = "Flashlight Range",
                .description = "Controls the on-screen width of the flashlight beam.",
                .minimum = 0.5,
                .maximum = 2.0
            };
        if (name == "bloom_enabled")
            return {
                .title = "Bloom",
                .description = "Let bright pixels glow into nearby screen pixels."
            };
        if (name == "bloom_intensity")
            return {
                .title = "Bloom Intensity",
                .description = "Controls the strength of the bright-area glow.",
                .minimum = 0.0,
                .maximum = 1.0
            };
        if (name == "bloom_threshold")
            return {
                .title = "Bloom Threshold",
                .description = "Sets how bright a pixel must be before it blooms.",
                .minimum = 0.0,
                .maximum = 1.0
            };
        if (name == "aspect_ratio_4_3")
            return {
                .title = "4:3 Ratio",
                .description = "Display a centred 4:3 camera image with black bars; no stretching."
            };
        return {};
    }
};
