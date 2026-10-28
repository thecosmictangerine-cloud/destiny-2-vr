#include "vr_game_check.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::vr::game_check {
namespace {

/** destiny2.exe as the Sunrise guide's depot manifest 7180122903232116872 delivers it. */
constexpr std::uint32_t kTimeDateStamp = 0x5F43138B;
constexpr std::uint32_t kSizeOfImage = 0x08A5EA00;

/** @return True when the main image's headers are the Sunrise build's. */
bool is_sunrise_build() noexcept {
    const auto* const base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return false;
    }
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == kTimeDateStamp
           && nt->OptionalHeader.SizeOfImage == kSizeOfImage;
}

} // namespace

void stop_unless_sunrise_build() noexcept {
    if (is_sunrise_build()) {
        return;
    }
    MessageBoxW(nullptr,
                L"This game folder is not a Project Sunrise install.\n\n"
                L"Destiny 2 VR only works with the old Destiny 2 build that the Sunrise Launcher downloads. "
                L"Never put its steam_api64.dll into your normal Steam copy of Destiny 2: the online game "
                L"has anti-cheat and modified files can get your account banned.\n\n"
                L"Nothing was changed. The game will close now. To fix it, put back the original "
                L"bin\\x64\\steam_api64.dll (Steam: Verify integrity of game files).",
                L"Destiny 2 VR", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    TerminateProcess(GetCurrentProcess(), 1);
}

} // namespace sunrise::client::hooks::vr::game_check
