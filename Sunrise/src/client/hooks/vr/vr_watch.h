#pragma once

namespace sunrise::client::hooks::vr::watch {

/**
 * Polls the command file a few times a second and runs one access watch when asked.
 *
 * `SVR_Watch.txt` beside the executable holds one line, `<hex address> <len 1|2|4|8> <w|rw> <ms>`.
 * The file is consumed (deleted) at once; the sites that touched the address during the window
 * land in `SVR_Watch.log` beside it and as `ev=vr.watch` lines in the structured log.
 * Cheap when the file is absent: one failed open every thirty frames.
 */
void tick() noexcept;

} // namespace sunrise::client::hooks::vr::watch
