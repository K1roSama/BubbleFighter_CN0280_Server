#include <limits>

#include "features/inventory/item_use_service.h"

namespace bf::application {
ServiceResult<ExpiredItemDiscardResult> ItemUseService::DiscardExpired(
    const ExpiredItemDiscardCommand& command) const {
  // D20190 excludes native item kind6 (pets); D0EAE0's removal uses a
  // signed-positive key.
  if (!command.player_id.IsValid() || command.native_item_key == 0U ||
      command.native_item_key > static_cast<std::uint32_t>(
                                    std::numeric_limits<std::int32_t>::max()) ||
      (command.native_item_key >= 301'000U &&
       command.native_item_key < 400'000U))
    return ServiceError{.code = ErrorCode::kInvalidInput,
                        .safe_message = "原过期删弃键无效或是不可删弃宠物"};
  return repository_.DiscardExpired(command, clock_.Now());
}
}  // namespace bf::application
