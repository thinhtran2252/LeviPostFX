#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <deque>
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

struct LogLine {
    std::string text;
    std::uint32_t color;
};

inline std::mutex gLogMutex;
inline std::deque<LogLine> gLogBuffer;
inline std::atomic_bool gEnabled{false};
inline std::atomic_bool gRegistered{false};

inline void refreshOverlay() {
    if (!gRegistered.load(std::memory_order_acquire))
        return;

    std::vector<pl::modmenu::DrawCommand> commands;

    if (gEnabled.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(gLogMutex);
        const std::size_t count = std::min(gLogBuffer.size(), kVisibleLines);
        const std::size_t first = gLogBuffer.size() - count;
        commands.reserve(count);

        // Submit only the newest visible lines. The newest line is always at
        // the bottom, so adding a new log automatically scrolls the view.
        for (std::size_t i = 0; i < count; ++i) {
            const LogLine& line = gLogBuffer[first + i];
            pl::modmenu::DrawCommand command{};
            command.type = pl::modmenu::DrawCommandType::Text;
            command.x = 12.0f;
            command.y = 26.0f + static_cast<float>(i) * 17.0f;
            command.size = 14.0f;
            command.color = line.color;
            command.text = line.text;
            commands.push_back(std::move(command));
        }
    }

    pl::modmenu::submitDrawCommands(
        kModuleId,
        std::span<const pl::modmenu::DrawCommand>(
            commands.data(), commands.size()
        )
    );
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
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }
    );

    return normalized == "true" || normalized == "1" ||
           normalized == "on" || normalized == "yes";
}

inline bool registerMenu() {
    if (gRegistered.load(std::memory_order_acquire))
        return true;

    pl::modmenu::ModuleBuilder module(
        std::string(kModuleId),
        "VHS Shader"
    );

    module
        .description("VHS Shader runtime diagnostics.")
        .modId("clange_me")
        .defaultEnabled(true)
        .config(
            "dev_tools",
            "Dev Tools",
            pl::modmenu::ConfigType::Toggle,
            "false"
        )
        .onConfigChanged(
            [](std::string_view, std::string_view key, std::string_view value) {
                if (key != "dev_tools")
                    return;

                const bool enabled = parseBool(value);
                gEnabled.store(enabled, std::memory_order_release);

                if (enabled)
                    logf(ANDROID_LOG_INFO, "Dev Tools enabled");
                else
                    refreshOverlay();
            }
        )
        .onToggle(
            [](std::string_view, bool enabled) {
                if (enabled) {
                    refreshOverlay();
                } else {
                    const std::vector<pl::modmenu::DrawCommand> empty;
                    pl::modmenu::submitDrawCommands(
                        kModuleId,
                        std::span<const pl::modmenu::DrawCommand>(
                            empty.data(), empty.size()
                        )
                    );
                }
            }
        );

    if (!module.registerModule()) {
        logf(ANDROID_LOG_ERROR, "Failed to register the in-game Mod Menu entry");
        return false;
    }

    gRegistered.store(true, std::memory_order_release);
    logf(ANDROID_LOG_INFO, "In-game Mod Menu entry registered");
    refreshOverlay();
    return true;
}

inline void unregisterMenu() {
    if (!gRegistered.exchange(false, std::memory_order_acq_rel))
        return;

    pl::modmenu::unregisterModule(kModuleId);
}

} // namespace clange_me::devtools
