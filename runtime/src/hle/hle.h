// Registers every HLE library with the linker.
#pragma once

namespace core { class Linker; }

namespace hle {

void RegisterAll(core::Linker& linker);

}  // namespace hle
