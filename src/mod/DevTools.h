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

inline float estimateTextWidth(std::string_view text, float fontSize) {
    float units = 0.0f;
    for (unsigned char ch : text) {
        if (ch == ' ') units += 0.28f;
        else if (ch == '.') units += 0.26f;
        else if (ch == 'I' || ch == 'i' || ch == 'l') units += 0.28f;
        else if (ch == 'M' || ch == 'W' || ch == 'm' || ch == 'w') units += 0.82f;
        else if (ch >= '0' && ch <= '9') units += 0.56f;
        else if (ch >= 'A' && ch <= 'Z') units += 0.62f;
        else units += 0.50f;
    }
    return units * fontSize;
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
            // Fixed 4x overlay scale, intentionally locked until a later update.
            constexpr float sizeMultiplier = 4.0f;
            const float topSize = 16.0f * sizeMultiplier;
            const float bottomSize = 14.0f * sizeMultiplier;
            const float edgePadding = std::max(14.0f, topSize * 0.70f);
            const float topY = edgePadding + topSize;
            const float bottomDateY = height - edgePadding;
            const float bottomTimeY = bottomDateY - bottomSize * 1.35f;
            const std::uint32_t infoColor = 0xFFFFFFFF;
            std::string elapsed = elapsedTimeString();
            std::string localTime, localDate;
            localClockStrings(localTime, localDate);

            addText(edgePadding, topY, topSize, infoColor, "PLAY ▶");
            // DrawCommand has no text-anchor property, so estimate glyph width
            // to keep the right-side labels aligned to the screen edge.
            const float rightEdge = width - edgePadding;
            addText(rightEdge - estimateTextWidth(elapsed, topSize),
                topY, topSize, infoColor, std::move(elapsed));
            addText(rightEdge - estimateTextWidth(localTime, bottomSize),
                bottomTimeY, bottomSize, infoColor, std::move(localTime));
            addText(rightEdge - estimateTextWidth(localDate, bottomSize),
                bottomDateY, bottomSize, infoColor, std::move(localDate));
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
        .config("color_bleeding_enabled", "Color Bleeding",
            pl::modmenu::ConfigType::Toggle, boolString(config.color_bleeding_enabled))
        .config("color_bleeding_intensity", "Color Bleeding Strength",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.color_bleeding_intensity),
            "0.000", "1.000", "color_bleeding_enabled")
        .config("distortion_enabled", "Lens Distortion",
            pl::modmenu::ConfigType::Toggle, boolString(config.distortion_enabled))
        .config("distortion_strength", "Lens Distortion Strength",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.distortion_strength),
            "0.000", "1.000", "distortion_enabled")
        .config("distortion_zoom", "Lens Distortion Zoom",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.distortion_zoom),
            "-1.000", "1.000", "distortion_enabled")
        .config("luma_noise_enabled", "Luma Noise",
            pl::modmenu::ConfigType::Toggle, boolString(config.luma_noise_enabled))
        .config("luma_noise_intensity", "Luma Noise Strength",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.luma_noise_intensity),
            "0.000", "1.000", "luma_noise_enabled")
        .config("chroma_noise_enabled", "Chroma Noise",
            pl::modmenu::ConfigType::Toggle, boolString(config.chroma_noise_enabled))
        .config("chroma_noise_intensity", "Chroma Noise Strength",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.chroma_noise_intensity),
            "0.000", "1.000", "chroma_noise_enabled")
        .config("scanlines_enabled", "Scanline",
            pl::modmenu::ConfigType::Toggle, boolString(config.scanlines_enabled))
        .config("scanline_intensity", "Scanline Strength",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.scanline_intensity),
            "0.000", "0.080", "scanlines_enabled")
        .config("aspect_ratio_4_3", "Ratio 4:3",
            pl::modmenu::ConfigType::Toggle, boolString(config.aspect_ratio_4_3))
        .config("vhs_info_enabled", "VHS Info",
            pl::modmenu::ConfigType::Toggle, boolString(config.vhs_info_enabled))
        .config("dev_tools", "Dev Tools",
            pl::modmenu::ConfigType::Toggle, "false")
        .config("chromatic_enabled", "Chromatic Aberration",
            pl::modmenu::ConfigType::Toggle, boolString(config.chromatic_enabled))
        .config("chromatic_intensity", "Chromatic Aberration Intensity",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.chromatic_intensity),
            "0.000", "3.000", "chromatic_enabled")
        .config("desaturation_enabled", "Desaturation",
            pl::modmenu::ConfigType::Toggle, boolString(config.desaturation_enabled))
        .config("desaturation_intensity", "Desaturation Intensity",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.desaturation_intensity),
            "0.000", "1.000", "desaturation_enabled")
        .config("shadow_darkness", "Darkness",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.shadow_darkness),
            "0.000", "1.000")
        .config("flashlight_enabled", "Flashlight",
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
        .config("bloom_enabled", "Bloom",
            pl::modmenu::ConfigType::Toggle, boolString(config.bloom_enabled))
        .config("bloom_intensity", "Bloom Intensity",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.bloom_intensity),
            "0.000", "1.000", "bloom_enabled")
        .config("bloom_threshold", "Bloom Threshold",
            pl::modmenu::ConfigType::SliderFloat, floatString(config.bloom_threshold),
            "0.000", "1.000", "bloom_enabled")
        .config("reset_all_settings", "Reset All Settings",
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
