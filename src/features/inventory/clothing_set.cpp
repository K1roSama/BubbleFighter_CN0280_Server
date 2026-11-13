#include "features/inventory/clothing_set.h"

#include <algorithm>
#include <array>

namespace bf::domain {
namespace {
#include "features/inventory/generated/clothing_sets_data.inc"
}  // namespace

std::span<const NativeClothingSet> NativeClothingSets() noexcept {
  return k_native_clothing_sets;
}

const NativeClothingSet* FindNativeClothingSet(std::uint32_t set_id) noexcept {
  const auto found = std::ranges::lower_bound(k_native_clothing_sets, set_id,
                                              {}, &NativeClothingSet::set_id);
  return found != k_native_clothing_sets.end() && found->set_id == set_id
             ? &*found
             : nullptr;
}
}  // namespace bf::domain
