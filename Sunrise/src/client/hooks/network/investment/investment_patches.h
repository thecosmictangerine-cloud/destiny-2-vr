#pragma once

namespace sunrise::client::hooks::network::investment {

/** Moves the four Arrivals leg mods into the leg plug set. Off unless `socket_menu_routing`. */
void apply_socket_menu_routing() noexcept;

/** Reserves low-address storage before retail content occupies the compatible address domain. */
void reserve_socket_menu_routing_storage() noexcept;

/** Restores owned category fields and plug-set descriptors without recycling published storage. */
void restore_socket_menu_routing() noexcept;

/** Clears the lore presentation gates. Off unless `reveal_lore_books` is set. */
void apply_lore_visibility() noexcept;

/** Puts every value the lore visibility patch replaced back. */
void restore_lore_visibility() noexcept;

} // namespace sunrise::client::hooks::network::investment
