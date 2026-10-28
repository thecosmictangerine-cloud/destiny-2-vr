#pragma once

/**
 * Bisection switches: words in `SVR_Debug.txt` beside the game, read once at the first question.
 * For experiments only -- nothing a player sets. `debug::has("exports keep")` is true when that
 * phrase appears anywhere in the file.
 */

#include <Windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace sunrise::client::hooks::vr::debug {

[[nodiscard]] inline const char* text() noexcept {
    static std::array<char, 1024> contents{};
    static bool read = false;
    if (!read) {
        read = true;
        std::array<wchar_t, MAX_PATH> path{};
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        wchar_t* const slash = length != 0 && length < path.size() ? std::wcsrchr(path.data(), L'\\') : nullptr;
        if (slash != nullptr
            && wcscpy_s(slash + 1, path.size() - static_cast<std::size_t>(slash + 1 - path.data()), L"SVR_Debug.txt") == 0) {
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

[[nodiscard]] inline bool has(const char* phrase) noexcept {
    return std::strstr(text(), phrase) != nullptr;
}

} // namespace sunrise::client::hooks::vr::debug
