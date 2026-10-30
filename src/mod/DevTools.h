#pragma once

#include "mod/Config.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <ctime>
#include <time.h>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <android/log.h>
#include <pl/ModMenu.hpp>

namespace clange_me::devtools {

inline constexpr std::string_view kModuleId = "vhs_shader";
inline constexpr std::size_t kMaxStoredLines = 96;
inline constexpr std::size_t kVisibleLines = 14;

using ToggleHandler = std::function<void(bool)>;
using ConfigHandler = std::function<void(std::string_view, std::string_view)>;

struct LogLine {
    std::string text;
    std::uint32_t color;
};

inline std::mutex gLogMutex;
inline std::deque<LogLine> gLogBuffer;
inline std::atomic_bool gEnabled{false};
inline std::atomic_bool gRegistered{false};
inline std::atomic_bool gVhsInfoEnabled{false};
inline std::atomic_bool gModuleEnabled{true};
inline std::chrono::steady_clock::time_point gLoadStart{};
inline std::atomic<std::int64_t> gLastOverlaySecond{-1};

inline void startInfoClock() {
    gLoadStart = std::chrono::steady_clock::now();
    gLastOverlaySecond.store(-1, std::memory_order_release);
}

inline std::string elapsedTimeString() {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - gLoadStart).count();
    // 60-hour wrap keeps every field in the 00..59 range.
    const std::int64_t wrapped = ((elapsed % 216000) + 216000) % 216000;
    const int hours = static_cast<int>(wrapped / 3600);
    const int minutes = static_cast<int>((wrapped / 60) % 60);
    const int seconds = static_cast<int>(wrapped % 60);
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hours, minutes, seconds);
    return buffer;
}

inline void localClockStrings(std::string& timeText, std::string& dateText) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr) {
        timeText = "--:--";
        dateText = "---. -- ----";
        return;
    }
    char timeBuffer[24]{};
    char dateBuffer[32]{};
    std::strftime(timeBuffer, sizeof(timeBuffer), "%p %I:%M", &local);
    std::strftime(dateBuffer, sizeof(dateBuffer), "%b. %d %Y", &local);
    timeText = timeBuffer;
    dateText = dateBuffer;
}

inline void refreshOverlay() {
    if (!gRegistered.load(std::memory_order_acquire))
        return;
    std::vector<pl::modmenu::DrawCommand> commands;
    if (gModuleEnabled.load(std::memory_order_acquire)) {
        const bool infoEnabled = gVhsInfoEnabled.load(std::memory_order_acquire);
        const auto surface = pl::modmenu::getHudSurfaceSize();
        const float width = surface.width > 0.0f ? surface.width : 1080.0f;
        const float height = surface.height > 0.0f ? surface.height : 1920.0f;

        auto addText = [&commands](float x, float y, float size,
                                   std::uint32_t color, std::string text) {
            pl::modmenu::DrawCommand command{};
            command.type = pl::modmenu::DrawCommandType::Text;
            command.x = x;
            command.y = y;
            command.size = size;
            command.color = color;
            command.text = std::move(text);
            commands.push_back(std::move(command));
        };

        if (infoEnabled) {
            constexpr float infoSize = 16.0f;
            constexpr float edgePadding = 14.0f;
            const std::uint32_t infoColor = 0xFFFFFFFF;
            std::string elapsed = elapsedTimeString();
            std::string localTime, localDate;
            localClockStrings(localTime, localDate);

            addText(edgePadding, 26.0f, infoSize, infoColor, "PLAY ▶");
            // DrawCommand has no text-anchor property, so estimate glyph width
            // to keep the right-side labels aligned to the screen edge.
            addText(width - edgePadding - elapsed.size() * 9.0f,
                26.0f, infoSize, infoColor, std::move(elapsed));
            addText(width - edgePadding - localTime.size() * 8.0f,
                height - 50.0f, 14.0f, infoColor, std::move(localTime));
            addText(width - edgePadding - localDate.size() * 8.0f,
                height - 29.0f, 14.0f, infoColor, std::move(localDate));
        }

        if (gEnabled.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(gLogMutex);
        const std::size_t count = std::min(gLogBuffer.size(), kVisibleLines);
        const std::size_t first = gLogBuffer.size() - count;
        commands.reserve(commands.size() + count);
        for (std::size_t i = 0; i < count; ++i) {
            const LogLine& line = gLogBuffer[first + i];
            pl::modmenu::DrawCommand command{};
            command.type = pl::modmenu::DrawCommandType::Text;
            command.x = 12.0f;
            command.y = (infoEnabled ? 54.0f : 26.0f) + static_cast<float>(i) * 17.0f;
            command.size = 14.0f;
            command.color = line.color;
            command.text = line.text;
            commands.push_back(std::move(command));
        }
        }
    }
    pl::modmenu::submitDrawCommands(
        kModuleId,
        std::span<const pl::modmenu::DrawCommand>(commands.data(), commands.size())
    );
}

inline void refreshOverlayIfNeeded() {
    if (!gRegistered.load(std::memory_order_acquire) ||
        !gModuleEnabled.load(std::memory_order_acquire) ||
        !gVhsInfoEnabled.load(std::memory_order_acquire))
        return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - gLoadStart).count();
    auto previous = gLastOverlaySecond.load(std::memory_order_acquire);
    if (elapsed != previous &&
        gLastOverlaySecond.compare_exchange_strong(previous, elapsed,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        refreshOverlay();
    }
}

inline void appendLine(std::string line, std::uint32_t color) {
    constexpr std::size_t maxLineLength = 180;
    if (line.size() > maxLineLength) {
        line.resize(maxLineLength - 3);
        line += "...";
    }
    {
        std::lock_guard<std::mutex> lock(gLogMutex);
        gLogBuffer.push_back({std::move(line), color});
        while (gLogBuffer.size() > kMaxStoredLines)
            gLogBuffer.pop_front();
    }
    if (gEnabled.load(std::memory_order_acquire))
        refreshOverlay();
}

inline void logf(int priority, const char* format, ...) {
    char message[1024]{};
    std::va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    __android_log_write(priority, "VHSShader", message);

    std::uint32_t color = 0xFFE8E8E8;
    std::string line = "[VHS] ";
    if (priority == ANDROID_LOG_ERROR) {
        color = 0xFFFF6B6B;
        line += "ERROR: ";
    } else if (priority == ANDROID_LOG_WARN) {
        color = 0xFFFFD166;
        line += "WARN: ";
    }
    line += message;
    appendLine(std::move(line), color);
}

inline bool parseBool(std::string_view value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized == "true" || normalized == "1" ||
           normalized == "on" || normalized == "yes";
}

inline std::string boolString(bool value) {
    return value ? "true" : "false";
}

inline std::string floatString(float value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(value));
    return buffer;
}

inline std::string buildV2ConfigSchema(const ModConfig& config) {
    using namespace pl::modmenu;
    const ModConfig defaults{};
    ConfigSchemaBuilder schema;
    schema.category("vhs", "VHS", "Camera and analog-video effects.")
          .category("other", "Other", "Lighting, utility, and debugging settings.")
          .defaultCategory("vhs");

    auto addToggle = [&schema](std::string id, std::string category,
                               std::string title, bool value,
                               bool defaultValue, std::string description,
                               std::string section) {
        ConfigNodeV2 node{};
        node.id = id;
        node.key = id;
        node.category = std::move(category);
        node.title = std::move(title);
        node.description = std::move(description);
        node.section = std::move(section);
        node.type = ConfigControlTypeV2::Toggle;
        node.defaultValue = boolString(defaultValue);
        node.currentValue = boolString(value);
        node.hasCurrentValue = true;
        schema.node(std::move(node));
    };

    auto addSlider = [&schema](std::string id, std::string category,
                               std::string title, float value, float minimum,
                               float defaultValue, float maximum, std::string description,
                               std::string section, std::string enabledBy = {}) {
        ConfigNodeV2 node{};
        node.id = id;
        node.key = id;
        node.category = std::move(category);
        node.title = std::move(title);
        node.description = std::move(description);
        node.section = std::move(section);
        node.type = ConfigControlTypeV2::SliderFloat;
        node.defaultValue = floatString(defaultValue);
        node.currentValue = floatString(value);
        node.hasCurrentValue = true;
        node.minValue = floatString(minimum);
        node.maxValue = floatString(maximum);
        node.step = "0.01";
        if (!enabledBy.empty()) {
            node.enabledWhen.push_back(ConfigConditionV2{
                .key = std::move(enabledBy),
                .op = ConfigConditionOpV2::Truthy,
                .value = ""
            });
        }
        schema.node(std::move(node));
    };

    addToggle("color_bleeding_enabled", "vhs", "Color Bleeding", config.color_bleeding_enabled, defaults.color_bleeding_enabled,
        "Horizontal analog colour smearing.", "Signal");
    addSlider("color_bleeding_intensity", "vhs", "Color Bleeding Strength", config.color_bleeding_intensity,
        0.0f, defaults.color_bleeding_intensity, 1.0f, "Amount of horizontal colour smearing.", "Signal", "color_bleeding_enabled");
    addToggle("distortion_enabled", "vhs", "Lens Distortion", config.distortion_enabled, defaults.distortion_enabled,
        "Curves the camera image toward the edges.", "Camera");
    addSlider("distortion_strength", "vhs", "Lens Distortion Strength", config.distortion_strength,
        0.0f, defaults.distortion_strength, 1.0f, "Normalized lens-warp amount.", "Camera", "distortion_enabled");
    addSlider("distortion_zoom", "vhs", "Lens Distortion Zoom", config.distortion_zoom,
        -1.0f, defaults.distortion_zoom, 1.0f, "Negative zooms out; positive zooms in.", "Camera", "distortion_enabled");
    addToggle("luma_noise_enabled", "vhs", "Luma Noise", config.luma_noise_enabled, defaults.luma_noise_enabled,
        "Animated noise in image brightness.", "Signal");
    addSlider("luma_noise_intensity", "vhs", "Luma Noise Strength", config.luma_noise_intensity,
        0.0f, defaults.luma_noise_intensity, 1.0f, "Brightness-channel noise amount.", "Signal", "luma_noise_enabled");
    addToggle("chroma_noise_enabled", "vhs", "Chroma Noise", config.chroma_noise_enabled, defaults.chroma_noise_enabled,
        "Animated noise in image colour.", "Signal");
    addSlider("chroma_noise_intensity", "vhs", "Chroma Noise Strength", config.chroma_noise_intensity,
        0.0f, defaults.chroma_noise_intensity, 1.0f, "Colour-channel noise amount.", "Signal", "chroma_noise_enabled");
    addToggle("scanlines_enabled", "vhs", "Scanline", config.scanlines_enabled, defaults.scanlines_enabled,
        "Horizontal analog display lines.", "Signal");
    addSlider("scanline_intensity", "vhs", "Scanline Strength", config.scanline_intensity,
        0.0f, defaults.scanline_intensity, 0.08f, "Scanline contrast.", "Signal", "scanlines_enabled");
    addToggle("aspect_ratio_4_3", "vhs", "Ratio 4:3", config.aspect_ratio_4_3, defaults.aspect_ratio_4_3,
        "Centres a 4:3 image without stretching.", "Camera");
    addToggle("vhs_info_enabled", "vhs", "VHS Info", config.vhs_info_enabled, defaults.vhs_info_enabled,
        "PLAY, elapsed time, and real device date/time.", "Overlay");
    addToggle("chromatic_enabled", "vhs", "Chromatic Aberration", config.chromatic_enabled, defaults.chromatic_enabled,
        "Separates RGB channels toward the frame edges.", "Camera");
    addSlider("chromatic_intensity", "vhs", "Chromatic Aberration Intensity", config.chromatic_intensity,
        0.0f, defaults.chromatic_intensity, 3.0f, "Amount of RGB channel separation.", "Camera", "chromatic_enabled");

    addToggle("flashlight_enabled", "other", "Flashlight", config.flashlight_enabled, defaults.flashlight_enabled,
        "Camera-centred screen-space light beam.", "Lighting");
    addSlider("flashlight_intensity", "other", "Flashlight Intensity", config.flashlight_intensity,
        3.0f, defaults.flashlight_intensity, 10.0f, "Brightness added by the beam.", "Lighting", "flashlight_enabled");
    addSlider("flashlight_range", "other", "Flashlight Range", config.flashlight_range,
        0.5f, defaults.flashlight_range, 2.0f, "Outer on-screen beam width.", "Lighting", "flashlight_enabled");
    addSlider("flashlight_donut_size", "other", "Light Donut Size", config.flashlight_donut_size,
        0.0f, defaults.flashlight_donut_size, 1.0f, "Size of the dark centre hole, not the outer beam radius.",
        "Lighting", "flashlight_enabled");
    addToggle("bloom_enabled", "other", "Bloom", config.bloom_enabled, defaults.bloom_enabled,
        "Glow around bright image areas.", "Lighting");
    addSlider("bloom_intensity", "other", "Bloom Intensity", config.bloom_intensity,
        0.0f, defaults.bloom_intensity, 1.0f, "Strength of the bright-area glow.", "Lighting", "bloom_enabled");
    addSlider("bloom_threshold", "other", "Bloom Threshold", config.bloom_threshold,
        0.0f, defaults.bloom_threshold, 1.0f, "Brightness required for a pixel to bloom.", "Lighting", "bloom_enabled");
    addSlider("shadow_darkness", "other", "Darkness", config.shadow_darkness,
        0.0f, defaults.shadow_darkness, 1.0f, "Darkens low-luminance areas.", "Lighting");
    addToggle("dev_tools", "other", "Dev Tools", false,
        false, "Show recent VHS Shader log messages.", "Debugging");
    addToggle("desaturation_enabled", "other", "Desaturation", config.desaturation_enabled, defaults.desaturation_enabled,
        "Reduces image colour intensity.", "Image");
    addSlider("desaturation_intensity", "other", "Desaturation Intensity", config.desaturation_intensity,
        0.0f, defaults.desaturation_intensity, 1.0f, "Amount of colour removed.", "Image", "desaturation_enabled");

    ConfigNodeV2 reset{};
    reset.id = "reset_all_settings";
    reset.key = "reset_all_settings";
    reset.category = "other";
    reset.title = "Reset All Settings";
    reset.description = "Restore every setting to its default value.";
    reset.section = "Utilities";
    reset.type = ConfigControlTypeV2::Button;
    reset.actionValue = "true";
    schema.node(std::move(reset));
    return schema.toJson();
}

inline bool registerMenu(
    const ModConfig& config,
    std::string modId,
    ToggleHandler toggleHandler,
    ConfigHandler configHandler
) {
    if (gRegistered.load(std::memory_order_acquire))
        return true;

    pl::modmenu::ModuleBuilder module(std::string(kModuleId), "VHS Shader");
    module
        .description("VHS camera effects, analog noise, flashlight overlays, and bloom controls.")
        .modId(std::move(modId))
        .defaultEnabled(config.enabled)
        .config("color_bleeding_enabled", "[VHS] Color Bleeding",
            pl::modmenu::ConfigType::Toggle, boolString(config.color_bleeding_enabled))
        .config("color_bleeding_intensity", "Color Bleeding Strength",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.color_bleeding_intensity),
            "0.000", "1.000", "color_bleeding_enabled")
        .config("distortion_enabled", "[VHS] Lens Distortion",
            pl::modmenu::ConfigType::Toggle, boolString(config.distortion_enabled))
        .config("distortion_strength", "Lens Distortion Strength",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.distortion_strength),
            "0.000", "1.000", "distortion_enabled")
        .config("distortion_zoom", "Lens Distortion Zoom",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.distortion_zoom),
            "-1.000", "1.000", "distortion_enabled")
        .config("luma_noise_enabled", "[VHS] Luma Noise",
            pl::modmenu::ConfigType::Toggle, boolString(config.luma_noise_enabled))
        .config("luma_noise_intensity", "Luma Noise Strength",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.luma_noise_intensity),
            "0.000", "1.000", "luma_noise_enabled")
        .config("chroma_noise_enabled", "[VHS] Chroma Noise",
            pl::modmenu::ConfigType::Toggle, boolString(config.chroma_noise_enabled))
        .config("chroma_noise_intensity", "Chroma Noise Strength",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.chroma_noise_intensity),
            "0.000", "1.000", "chroma_noise_enabled")
        .config("scanlines_enabled", "[VHS] Scanline",
            pl::modmenu::ConfigType::Toggle, boolString(config.scanlines_enabled))
        .config("scanline_intensity", "Scanline Strength",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.scanline_intensity),
            "0.000", "0.080", "scanlines_enabled")
        .config("aspect_ratio_4_3", "[VHS] Ratio 4:3",
            pl::modmenu::ConfigType::Toggle, boolString(config.aspect_ratio_4_3))
        .config("vhs_info_enabled", "[VHS] VHS Info",
            pl::modmenu::ConfigType::Toggle, boolString(config.vhs_info_enabled))
        .config("dev_tools", "[OTHER] Dev Tools",
            pl::modmenu::ConfigType::Toggle, "false")
        .config("chromatic_enabled", "[VHS] Chromatic Aberration",
            pl::modmenu::ConfigType::Toggle, boolString(config.chromatic_enabled))
        .config("chromatic_intensity", "Chromatic Aberration Intensity",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.chromatic_intensity),
            "0.000", "3.000", "chromatic_enabled")
        .config("desaturation_enabled", "[OTHER] Desaturation",
            pl::modmenu::ConfigType::Toggle, boolString(config.desaturation_enabled))
        .config("desaturation_intensity", "Desaturation Intensity",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.desaturation_intensity),
            "0.000", "1.000", "desaturation_enabled")
        .config("shadow_darkness", "[OTHER] Darkness",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.shadow_darkness),
            "0.000", "1.000")
        .config("flashlight_enabled", "[OTHER] Flashlight",
            pl::modmenu::ConfigType::Toggle, boolString(config.flashlight_enabled))
        .config("flashlight_intensity", "Flashlight Intensity",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.flashlight_intensity),
            "3.000", "10.000", "flashlight_enabled")
        .config("flashlight_range", "Flashlight Range",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.flashlight_range),
            "0.500", "2.000", "flashlight_enabled")
        .config("flashlight_donut_size", "Light Donut Size",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.flashlight_donut_size),
            "0.000", "1.000", "flashlight_enabled")
        .config("bloom_enabled", "[OTHER] Bloom",
            pl::modmenu::ConfigType::Toggle, boolString(config.bloom_enabled))
        .config("bloom_intensity", "Bloom Intensity",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.bloom_intensity),
            "0.000", "1.000", "bloom_enabled")
        .config("bloom_threshold", "Bloom Threshold",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.bloom_threshold),
            "0.000", "1.000", "bloom_enabled")
        .config("reset_all_settings", "[OTHER] Reset All Settings",
            pl::modmenu::ConfigType::Button, "")
        .onConfigChanged(
            [configHandler = std::move(configHandler)](
                std::string_view, std::string_view key, std::string_view value
            ) {
                if (key == "dev_tools") {
                    const bool enabled = parseBool(value);
                    gEnabled.store(enabled, std::memory_order_release);
                    if (enabled) logf(ANDROID_LOG_INFO, "Dev Tools enabled");
                    else refreshOverlay();
                    return;
                }
                if (key == "vhs_info_enabled") {
                    gVhsInfoEnabled.store(parseBool(value), std::memory_order_release);
                    gLastOverlaySecond.store(-1, std::memory_order_release);
                    refreshOverlay();
                    if (configHandler) configHandler(key, value);
                    return;
                }
                if (configHandler) configHandler(key, value);
                if (key == "reset_all_settings") {
                    gEnabled.store(false, std::memory_order_release);
                    gVhsInfoEnabled.store(false, std::memory_order_release);
                    gLastOverlaySecond.store(-1, std::memory_order_release);
                    refreshOverlay();
                    pl::modmenu::setConfigSchemaJson(kModuleId,
                        buildV2ConfigSchema(ModConfig{}));
                }
            }
        )
        .onToggle(
            [toggleHandler = std::move(toggleHandler)](
                std::string_view, bool enabled
            ) {
                gModuleEnabled.store(enabled, std::memory_order_release);
                if (toggleHandler) toggleHandler(enabled);
                if (enabled) refreshOverlay();
                else {
                    const std::vector<pl::modmenu::DrawCommand> empty;
                    pl::modmenu::submitDrawCommands(
                        kModuleId,
                        std::span<const pl::modmenu::DrawCommand>(empty.data(), empty.size())
                    );
                }
            }
        );

    if (!module.registerModule()) {
        logf(ANDROID_LOG_ERROR, "Failed to register the in-game Mod Menu entry");
        return false;
    }
    if (!pl::modmenu::setConfigSchemaJson(kModuleId, buildV2ConfigSchema(config)))
        logf(ANDROID_LOG_WARN, "Could not install categorized Mod Menu V2 schema; using flat fallback entries");
    gModuleEnabled.store(config.enabled, std::memory_order_release);
    gVhsInfoEnabled.store(config.vhs_info_enabled, std::memory_order_release);
    gRegistered.store(true, std::memory_order_release);
    logf(ANDROID_LOG_INFO, "In-game VHS effect controls registered");
    refreshOverlay();
    return true;
}

inline void unregisterMenu() {
    if (!gRegistered.exchange(false, std::memory_order_acq_rel))
        return;
    pl::modmenu::unregisterModule(kModuleId);
}

} // namespace clange_me::devtools
