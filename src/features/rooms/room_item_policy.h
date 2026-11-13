#ifndef BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_ITEM_POLICY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_ITEM_POLICY_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf::domain {
struct RoomItemEntry {
  std::uint32_t id, category;
  std::string_view name;
};

inline constexpr RoomItemEntry kRoomItemCatalog[] = {
#define BF_ROOM_ITEM(id, group, name) {id, group, name},
#include "features/rooms/generated/room_item_catalog.inc"
#undef BF_ROOM_ITEM
};
inline constexpr std::size_t kMaxRoomRestrictedItems = 256U;

inline const RoomItemEntry* RoomItem(std::uint32_t id) {
  const auto p =
      std::lower_bound(std::begin(kRoomItemCatalog), std::end(kRoomItemCatalog),
                       id, [](const auto& e, auto n) { return e.id < n; });
  return p != std::end(kRoomItemCatalog) && p->id == id ? p : nullptr;
}

inline bool ValidRoomItems(std::span<const std::uint32_t> items) {
  return items.size() <= kMaxRoomRestrictedItems &&
         std::ranges::all_of(items,
                             [](auto id) { return RoomItem(id) != nullptr; }) &&
         std::ranges::is_sorted(items) &&
         std::adjacent_find(items.begin(), items.end()) == items.end();
}

inline std::string RestrictedEquipmentNotice(
    std::span<const std::uint32_t> banned,
    std::span<const std::uint32_t> equipped) {
  std::string names;
  unsigned count = 0;
  for (auto id : banned)
    if (std::ranges::find(equipped, id) != equipped.end()) {
      if (count++ == 4U) {
        names += " 等";
        break;
      }
      if (!names.empty()) names += "、";
      const auto* item = RoomItem(id);
      names += item ? std::string(item->name) : std::to_string(id);
    }
  return names.empty()
             ? std::string{}
             : "房间已禁用：" + names + "。请卸下或更换后再准备／开局。";
}

// Implicit native defaults are effective equipment too. Check before mode and
// Ethi transformations.
inline void AppendDefaultRoomWeapons(std::vector<std::uint32_t>& ids,
                                     const std::array<bool, 4>& present) {
  constexpr std::array<std::uint32_t, 4> defaults{202004U, 201004U, 207001U,
                                                  205033U};
  for (std::size_t i = 0; i < 4; ++i)
    if (!present[i]) ids.push_back(defaults[i]);
}
}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_ITEM_POLICY_H_
