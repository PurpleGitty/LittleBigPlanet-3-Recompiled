// Stand-in for Abseil's flat_hash_set.
#pragma once
#include <unordered_set>

namespace absl {
template <typename K, typename Hash = std::hash<K>, typename Eq = std::equal_to<K>>
using flat_hash_set = std::unordered_set<K, Hash, Eq>;
}  // namespace absl
