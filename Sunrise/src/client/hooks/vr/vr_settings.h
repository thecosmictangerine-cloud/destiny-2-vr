#pragma once

/**
 * The player's settings: `SVR_Settings.txt` beside the game, read once at start. One `key value`
 * per line, `#` starts a comment; anything missing keeps its default, so no file at all is the
 * normal case. Numbers are read with a dot whatever the locale (this machine writes commas).
 *
 *   start on|off          VR comes up with the game (default on); off = only when F9 is pressed
 *   runtime auto|meta|steamvr|<path to an OpenXR manifest>
 *                         auto: XR_RUNTIME_JSON, else Meta's runtime when it is installed, else the
 *                         system's active runtime (default auto)
 *   screen_size <deg>     the virtual screen's width as seen from where it is placed (default 62)
 *   screen_distance <m>   (default 2.4)
 *   keepalive off|on|auto  the headset fed by the module's own frame thread while the game stalls
 *                         (default off; auto: on for Meta's runtime and the mock; docs/PLAYABLE.md)
 *   display_shape eye|desktop  eye: the render takes the eye's shape and fills the headset, the desktop
 *                         window shows it squashed (default); desktop: the window's own 16:9, undistorted
 *                         on the PC for recording, with black bands in the headset (docs/DISPLAY.md §2)
 */

#include <Windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace sunrise::client::hooks::vr::settings {

[[nodiscard]] inline const char* text() noexcept {
    static std::array<char, 2048> contents{};
    static bool read = false;
    if (!read) {
        read = true;
        std::array<wchar_t, MAX_PATH> path{};
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        wchar_t* const slash = length != 0 && length < path.size() ? std::wcsrchr(path.data(), L'\\') : nullptr;
        if (slash != nullptr
            && wcscpy_s(slash + 1, path.size() - static_cast<std::size_t>(slash + 1 - path.data()), L"SVR_Settings.txt") == 0) {
            FILE* file = nullptr;
            if (_wfopen_s(&file, path.data(), L"rb") == 0 && file != nullptr) {
                const std::size_t got = std::fread(contents.data(), 1, contents.size() - 1, file);
                contents[got] = '\0';
                std::fclose(file);
            }
        }
    }
    return contents.data();
}

/**
 * The value of `key` (the rest of its line, trimmed), or `fallback`.
 * @return A pointer into a static copy, valid until the next call.
 */
[[nodiscard]] inline const char* value(const char* key, const char* fallback) noexcept {
    static std::array<char, 512> out{};
    const char* cursor = text();
    const std::size_t keyLength = std::strlen(key);
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
            ++cursor;
        }
        const char* line = cursor;
        while (*cursor != '\0' && *cursor != '\n') {
            ++cursor;
        }
        if (*line == '#' || std::strncmp(line, key, keyLength) != 0 || (line[keyLength] != ' ' && line[keyLength] != '\t')) {
            continue;
        }
        const char* v = line + keyLength;
        while (*v == ' ' || *v == '\t') {
            ++v;
        }
        std::size_t n = static_cast<std::size_t>(cursor - v);
        while (n > 0 && (v[n - 1] == '\r' || v[n - 1] == ' ' || v[n - 1] == '\t')) {
            --n;
        }
        n = n < out.size() - 1 ? n : out.size() - 1;
        std::memcpy(out.data(), v, n);
        out[n] = '\0';
        return out.data();
    }
    return fallback;
}

[[nodiscard]] inline bool flag(const char* key, bool fallback) noexcept {
    const char* v = value(key, nullptr);
    if (v == nullptr) {
        return fallback;
    }
    return std::strcmp(v, "on") == 0 || std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0;
}

[[nodiscard]] inline float number(const char* key, float fallback, float low, float high) noexcept {
    const char* v = value(key, nullptr);
    if (v == nullptr) {
        return fallback;
    }
    // Locale-independent: a comma decimal is taken as a dot.
    std::array<char, 64> copy{};
    std::size_t i = 0;
    for (; v[i] != '\0' && i + 1 < copy.size(); ++i) {
        copy[i] = v[i] == ',' ? '.' : v[i];
    }
    char* end = nullptr;
    const float parsed = std::strtof(copy.data(), &end);
    if (end == copy.data() || !(parsed >= low && parsed <= high)) {
        return fallback;
    }
    return parsed;
}

} // namespace sunrise::client::hooks::vr::settings
