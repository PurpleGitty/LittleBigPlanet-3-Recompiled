// Event queue entry points used by other HLE libraries (VideoOut flips, ...).
#pragma once
#include <cstdint>

namespace hle::kernel {

enum EventFilter : int16_t {
    kEvfiltTimer = -7,
    kEvfiltUser = -11,
    kEvfiltVideoOut = -13,
    kEvfiltGraphicsCore = -14,
};

// Registers interest in (ident, filter) on an equeue handle.
void AddEvent(void* equeue, uint64_t ident, int16_t filter, void* udata);
// Fires (ident, filter) on an equeue handle if it was registered there.
void TriggerEvent(void* equeue, uint64_t ident, int16_t filter, int64_t data);

}  // namespace hle::kernel
