#include "gx.hpp"
#include "__gx.h"

#include "../../gx/fifo.hpp"
#include "../../gx/command_processor.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static __GXData_struct sSavedGXData;

extern "C" {
void GXBeginDisplayList(void* list, u32 size) {
  CHECK(!aurora::gx::fifo::in_display_list(), "Display list began twice!");

  // Flush any pending dirty state before recording
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Save current shadow register state if requested
  if (__gx->dlSaveContext != 0) {
    std::memcpy(&sSavedGXData, __gx, sizeof(sSavedGXData));
  }

  __gx->inDispList = 1;

  // Redirect FIFO writes to the user-provided buffer
  aurora::gx::fifo::begin_display_list(static_cast<u8*>(list), size);
}

u32 GXEndDisplayList() {
  // Flush any pending dirty state into the display list
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // End FIFO redirection and get the byte count (ROUNDUP32)
  u32 bytesWritten = aurora::gx::fifo::end_display_list();

  // Restore saved shadow register state
  if (__gx->dlSaveContext != 0) {
    std::memcpy(__gx, &sSavedGXData, sizeof(*__gx));
  }

  __gx->inDispList = 0;

  return bytesWritten;
}

void GXCallDisplayList(const void* data, u32 nbytes) {
  // Flush any pending dirty state before calling
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Flush pending primitives
  if (*reinterpret_cast<u32*>(&__gx->vNum) != 0) {
    __GXSendFlushPrim();
  }

  /* TEMPORARY diagnostic (2026-08-11): record where each display list lands in
   * the FIFO, so an "unknown opcode at pos N" desync can be mapped back to the
   * display list (and therefore the caller/archive) that produced those bytes.
   * MELEE_PC_TRACE_DL=<count>. */
  {
    static int budget = -1;
    static int count = 0;
    if (budget < 0) {
      const char* env = std::getenv("MELEE_PC_TRACE_DL");
      budget = env != nullptr ? std::atoi(env) : 0;
    }
    if (count < budget) {
      ++count;
      std::fprintf(stderr, "PROGDBG CallDL #%d fifoPos=%u nbytes=%u data=%p\n", count,
                   aurora::gx::fifo::get_buffer_size(), nbytes, data);
    }
  }
  /* Always recorded (two stores), so a desync can be diffed against its source
   * display list even on a run with no tracing enabled. */
  aurora::gx::fifo::note_display_list(aurora::gx::fifo::get_buffer_size(), nbytes, data);

  // Write display list contents to the FIFO
  aurora::gx::fifo::write_data(data, nbytes);
  aurora::gx::fifo::publish();
}

}
