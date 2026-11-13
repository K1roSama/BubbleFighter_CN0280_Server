#include "features/characters/native_title_profile.h"

#include <algorithm>

namespace bf::application {
void SyncNativeTitleEquipmentFlags(PlayerMenuSnapshot& cached,
                                   const PlayerMenuSnapshot& fresh) noexcept {
  if (cached.player_id != fresh.player_id) return;
  for (auto& item : cached.items) {
    if (item.item_id.Value() < 400'000U || item.item_id.Value() >= 401'000U)
      continue;
    const auto found =
        std::ranges::find_if(fresh.items, [&item](const auto& value) {
          return value.item_instance_id == item.item_instance_id &&
                 value.item_id == item.item_id;
        });
    item.equipped = found != fresh.items.end() && found->equipped;
  }
}
}  // namespace bf::application
