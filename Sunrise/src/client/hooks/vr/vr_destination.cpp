/**
 * Persistent forced destination, fed from a text file. See vr_destination.h.
 *
 * Runs on the camera thread beside the other VR polls, which means it first runs in orbit: early
 * enough, since the forced selection is applied when the client asks the embedded server for an
 * activity, which is the Director launch that comes after.
 */

#include "vr_destination.h"

#include <Windows.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "../../../core/logging/log.h"
#include "../../../state/activity/forced/activity_forced_destination.h"

namespace sunrise::client::hooks::vr::destination {
namespace {

constexpr const wchar_t* kFileName = L"SVR_Destination.txt";
/** Camera frames between reads. The file is small and rarely changes. */
constexpr std::uint32_t kPollPeriod = 120;

namespace forced = ::sunrise::state::activity::forced;

std::uint32_t g_frame{};
/** Last line published, so an unchanged file costs one read and no log line. */
std::array<char, 160> g_lastLine{};

void reportf(const char* format, ...) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(line.data(), line.size(), format, args);
    va_end(args);
    if (written > 0) {
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Directory of the executable plus the given file name. */
bool game_path(const wchar_t* name, std::array<wchar_t, MAX_PATH>& out) noexcept {
    const DWORD length = GetModuleFileNameW(nullptr, out.data(), static_cast<DWORD>(out.size()));
    if (length == 0 || length >= out.size()) {
        return false;
    }
    wchar_t* const slash = std::wcsrchr(out.data(), L'\\');
    if (slash == nullptr) {
        return false;
    }
    const std::size_t dirLength = static_cast<std::size_t>(slash + 1 - out.data());
    return wcscpy_s(slash + 1, out.size() - dirLength, name) == 0;
}

/** Reads the first non-empty line of the file, or returns false when there is none. */
bool read_line(std::array<char, 160>& line) noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    if (!game_path(kFileName, path)) {
        return false;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.data(), L"r") != 0 || file == nullptr) {
        return false;
    }
    bool found = false;
    while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
        // Strip the newline and skip blank lines and comments.
        line[std::strcspn(line.data(), "\r\n")] = '\0';
        const char* cursor = line.data();
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '\0' || *cursor == '#') {
            continue;
        }
        found = true;
        break;
    }
    std::fclose(file);
    return found;
}

/** Parses one line and publishes it. */
void apply(const char* line) noexcept {
    char name[64] = {};
    unsigned bubble = 0;
    unsigned slice = 0;
    unsigned activity = 0;
    char spawn[24] = {};
    const int n = sscanf_s(line, "%63s %u %u %u %23s", name, static_cast<unsigned>(sizeof name), &bubble, &slice,
                           &activity, spawn, static_cast<unsigned>(sizeof spawn));
    if (n >= 1 && std::strcmp(name, "off") == 0) {
        forced::clear();
        reportf("ev=vr.dest forced result=cleared");
        return;
    }
    if (n < 3) {
        reportf("ev=vr.dest forced result=fail reason=parse line=%s", line);
        return;
    }
    forced::ForcedDestination value{};
    const std::size_t length = std::strlen(name);
    if (length == 0 || length > value.packageName.size()) {
        reportf("ev=vr.dest forced result=fail reason=name_length");
        return;
    }
    std::memcpy(value.packageName.data(), name, length);
    value.packageNameLength = static_cast<std::uint8_t>(length);
    value.bubble = static_cast<std::uint8_t>(bubble);
    value.hasBubble = true;
    value.sliceSet = static_cast<std::uint16_t>(slice);
    value.hasSliceSet = true;
    // The activity index picks the definition the package binds to. Without it the forced
    // package inherits whatever the Director node was (a raid node landed the ship in
    // `eden_freeroam` with a raid-type activity and it never reached physics_join, 2026-09-08).
    if (n >= 4) {
        value.activityIndex = static_cast<std::uint16_t>(activity);
        value.hasActivityIndex = true;
    }
    if (n >= 5) {
        value.spawnSetHash = static_cast<std::uint32_t>(std::strtoul(spawn, nullptr, 0));
        value.hasSpawnSetHash = true;
    }
    value.enabled = true;
    const bool ok = forced::publish(value);
    reportf("ev=vr.dest forced result=%s name=%s bubble=%u slice_set=%u activity=%d spawn=0x%X active=%d",
            ok ? "ok" : "fail", name, bubble, slice, value.hasActivityIndex ? static_cast<int>(activity) : -1,
            value.hasSpawnSetHash ? value.spawnSetHash : 0U, forced::override_active() ? 1 : 0);
}

} // namespace

void tick() noexcept {
    if ((g_frame++ % kPollPeriod) != 0) {
        return;
    }
    std::array<char, 160> line{};
    if (!read_line(line)) {
        return;
    }
    if (std::strcmp(line.data(), g_lastLine.data()) == 0) {
        return;
    }
    g_lastLine = line;
    apply(line.data());
}

} // namespace sunrise::client::hooks::vr::destination
