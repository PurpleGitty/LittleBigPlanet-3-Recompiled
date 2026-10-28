// PS4 NIDs: the 11-character hashed IDs that replace symbol names in import tables.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace core {

// NID for a function or variable name, e.g. Nid("scePadReadState") == "YndgXqQVV7c".
std::string Nid(std::string_view name);

// Decodes the library/module ID suffixes of "NID#L#M" symbol names.
uint16_t DecodeId(std::string_view encoded);

}  // namespace core
