#pragma once
#include <imgui.h>

namespace sunrise::client::ui::mission_launch::detail {
inline void heading(const char* text) noexcept {
    // PushFont takes an unscaled base size; passing GetFontSize would double-apply DPI.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.375F);
    ImGui::TextWrapped("%s", text);
    ImGui::PopFont();
}

/** @return True only when an enabled launcher button is pressed. */
[[nodiscard]] inline bool launch_button(bool busy, bool disabled, float height) noexcept {
    // The theme sets CheckMark, SliderGrabActive and ButtonActive to its three accent tones.
    const auto& colors = ImGui::GetStyle().Colors;
    if (!disabled) {
        ImGui::PushStyleColor(ImGuiCol_Button, colors[ImGuiCol_CheckMark]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_SliderGrabActive]);
        ImGui::PushStyleColor(ImGuiCol_Text, colors[ImGuiCol_WindowBg]);
    }
    ImGui::BeginDisabled(disabled);
    const bool clicked = ImGui::Button(busy ? "Launching..." : "Launch activity", {-1.0F, height});
    ImGui::EndDisabled();
    if (!disabled) {
        ImGui::PopStyleColor(3);
    }
    return clicked;
}
} // namespace sunrise::client::ui::mission_launch::detail
