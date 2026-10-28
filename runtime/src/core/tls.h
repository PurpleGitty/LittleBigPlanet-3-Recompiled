// Thread-local storage for game code.
//
// PS4 code finds its thread control block (TCB) with `mov reg, fs:[0]`. Windows
// doesn't let us point fs at our own memory, but a TEB TLS slot is reachable
// through gs with an instruction of the same length, so those instructions are
// rewritten in place to `mov reg, gs:[0x1480 + slot*8]`.
#pragma once
#include <cstdint>
#include <vector>

#include "core/module.h"

namespace core {

struct Tcb {
    Tcb* self;
    void* dtv;
    void* thread;
    void* spare[2];
    uint64_t canary;
    void* fiber;
};

// Allocates the TLS slot. Must run before any module is patched.
void InitTls(const std::vector<Module*>& modules);

// Rewrites fs:[0] reads in the module's code. Returns the number patched.
size_t PatchFsAccesses(Module& m);

// Creates this thread's TCB + static TLS blocks and makes it current.
Tcb* SetupThreadTls();
void FreeThreadTls();

// This thread's TCB (nullptr on threads that never ran game code).
Tcb* CurrentTcb();

// Address of a module's TLS variable for the current thread (__tls_get_addr).
void* TlsAddress(uint64_t moduleIndex, uint64_t offset);

}  // namespace core
