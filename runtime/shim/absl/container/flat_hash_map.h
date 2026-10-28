// Stand-in for Abseil's flat_hash_map (used by shadPS4's texture cache).
// std::unordered_map keeps the same interface (iterator->second is assignable).
#pragma once
#include <unordered_map>

namespace absl {
template <typename K, typename V, typename Hash = std::hash<K>, typename Eq = std::equal_to<K>>
using flat_hash_map = std::unordered_map<K, V, Hash, Eq>;
}  // namespace absl
