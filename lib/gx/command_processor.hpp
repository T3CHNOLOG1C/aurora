#pragma once

#include "../internal.hpp"

#include <cstdint>

namespace aurora::gx::fifo {

// Process a buffer of GX FIFO commands
void process(const uint8_t* data, uint32_t size) noexcept;
void clear_draw_cache() noexcept;
uint32_t current_draw_index() noexcept;

/* TEMPORARY diagnostic (2026-08-11): records that `nbytes` of display list from
 * `src` were copied into the FIFO starting at `fifoPos`. On a stream desync the
 * command processor uses this to find the display list covering the failing
 * offset and diff the FIFO bytes against the source, which distinguishes "we
 * copied it into the FIFO wrong" from "the archive data really says this".
 * See pc_port.md entry (35)/(36). */
void note_display_list(uint32_t fifoPos, uint32_t nbytes, const void* src) noexcept;

} // namespace aurora::gx::fifo
