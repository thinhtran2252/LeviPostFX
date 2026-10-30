#pragma once

#include "mod/Config.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
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
using ConfigHandler =
    std::function<void(std::string_view, std::string_view)>;

struct LogLine {
    std::string text;
    std::uint32_t color;
};

inline std::mutex gLogMutex;
inline std::deque<LogLine> gLogBuffer;
inline std::atomic_bool gEnabled{false};
inline std::atomic_bool gRegistered{false};

inline bool parseBool(std::string_view value) {
    std::string normalized(value);

    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );

    return normalized == "true" ||
           normalized == "1" ||
           normalized == "on" ||
           normalized == "yes";
}

inline std::string boolString(bool value) {
    return value ? "true" : "false";
}

inline std::string floatString(float value) {
    char buffer[32]{};

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%.3f",
        static_cast<double>(value)
    );

    return buffer;
}

inline void refreshOverlay() {
    if (!gRegistered.load(std::memory_order_acquire))
        return;

    std::vector<pl::modmenu::DrawCommand> commands;

    if (gEnabled.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(gLogMutex);

        const std::size_t count =
            std::min(gLogBuffer.size(), kVisibleLines);

        const std::size_t first =
            gLogBuffer.size() - count;

        commands.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            const LogLine& line = gLogBuffer[first + i];

            pl::modmenu::DrawCommand command{};

            command.type =
                pl::modmenu::DrawCommandType::Text;

            command.x = 12.0f;
            command.y =
                26.0f + static_cast<float>(i) * 17.0f;

            command.size = 14.0f;
            command.color = line.color;
            command.text = line.text;

            commands.push_back(std::move(command));
        }
    }

    pl::modmenu::submitDrawCommands(
        kModuleId,
        std::span<const pl::modmenu::DrawCommand>(
            commands.data(),
            commands.size()
        )
    );
}

inline void appendLine(
    std::string line,
    std::uint32_t color
) {
    constexpr std::size_t maxLineLength = 180;

    if (line.size() > maxLineLength) {
        line.resize(maxLineLength - 3);
        line += "...";
    }

    {
        std::lock_guard<std::mutex> lock(gLogMutex);

        gLogBuffer.push_back({
            std::move(line),
            color
        });

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

    std::vsnprintf(
        message,
        sizeof(message),
        format,
        args
    );

    va_end(args);

    __android_log_write(
        priority,
        "VHSShader",
        message
    );

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

inline bool registerMenu(
    const ModConfig& config,
    ToggleHandler toggleHandler,
    ConfigHandler configHandler
) {
    if (gRegistered.load(std::memory_order_acquire))
        return true;

    pl::modmenu::ModuleBuilder module(
        std::string(kModuleId),
        "VHS Shader"
    );

    module
        .description(
            "VHS camera effects and post-processing controls."
        )
        .modId("clange_me")
        .defaultEnabled(config.enabled)

        .config(
            "dev_tools",
            "Dev Tools Overlay",
            pl::modmenu::ConfigType::Toggle,
            "false"
        )

        .config(
            "distortion_enabled",
            "Screen Distortion",
            pl::modmenu::ConfigType::Toggle,
            boolString(config.distortion_enabled)
        )

        .config(
            "distortion_strength",
            "Distortion Strength",
            pl::modmenu::ConfigType::SliderFloat,
            floatString(config.distortion_strength),
            "0.000",
            "0.080"
        )

        .config(
            "distortion_zoom",
            "Distortion Border Zoom",
            pl::modmenu::ConfigType::SliderFloat,
            floatString(config.distortion_zoom),
            "1.000",
            "1.100"
        )

        .config(
            "chromatic_enabled",
            "Chromatic Aberration",
            pl::modmenu::ConfigType::Toggle,
            boolString(config.chromatic_enabled)
        )

        .config(
            "chromatic_intensity",
            "Chromatic Aberration Intensity",
            pl::modmenu::ConfigType::SliderFloat,
            floatString(config.chromatic_intensity),
            "0.000",
            "3.000",
            "chromatic_enabled"
        )

        .config(
            "scanlines_enabled",
            "Scanlines",
            pl::modmenu::ConfigType::Toggle,
            boolString(config.scanlines_enabled)
        )

        .config(
            "scanline_intensity",
            "Scanline Intensity",
            pl::modmenu::ConfigType::SliderFloat,
            floatString(config.scanline_intensity),
            "0.000",
            "0.080",
            "scanlines_enabled"
        )

        .config(
            "aspect_ratio_4_3",
            "4:3 Ratio",
            pl::modmenu::ConfigType::Toggle,
            boolString(config.aspect_ratio_4_3)
        )

        .onConfigChanged(
            [configHandler = std::move(configHandler)](
                std::string_view,
                std::string_view key,
                std::string_view value
            ) {
                if (key == "dev_tools") {
                    const bool enabled = parseBool(value);

                    gEnabled.store(
                        enabled,
                        std::memory_order_release
                    );

                    if (enabled) {
                        logf(
                            ANDROID_LOG_INFO,
                            "Dev Tools enabled"
                        );
                    } else {
                        refreshOverlay();
                    }

                    return;
                }

                if (configHandler)
                    configHandler(key, value);
            }
        )

        .onToggle(
            [toggleHandler = std::move(toggleHandler)](
                std::string_view,
                bool enabled
            ) {
                if (toggleHandler)
                    toggleHandler(enabled);

                if (enabled) {
                    refreshOverlay();
                } else {
                    const std::vector<
                        pl::modmenu::DrawCommand
                    > empty;

                    pl::modmenu::submitDrawCommands(
                        kModuleId,
                        std::span<
                            const pl::modmenu::DrawCommand
                        >(empty.data(), empty.size())
                    );
                }
            }
        );

    if (!module.registerModule()) {
        logf(
            ANDROID_LOG_ERROR,
            "Failed to register the in-game Mod Menu entry"
        );

        return false;
    }

    gRegistered.store(
        true,
        std::memory_order_release
    );

    logf(
        ANDROID_LOG_INFO,
        "In-game VHS effect controls registered"
    );

    refreshOverlay();

    return true;
}

inline void unregisterMenu() {
    if (!gRegistered.exchange(
        false,
        std::memory_order_acq_rel
    )) {
        return;
    }

    pl::modmenu::unregisterModule(kModuleId);
}

} // namespace clange_me::devtools