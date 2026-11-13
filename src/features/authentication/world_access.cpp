#include "features/authentication/world_access.h"

#include <algorithm>
#include <string>
#include <variant>

namespace bf::application {

WorldAccessService::WorldAccessService(WorldAccessReader& reader) noexcept
    : reader_(reader) {}

ServiceResult<WorldAccessSnapshot> WorldAccessService::Load(
    domain::PlayerId player_id) const {
  if (!player_id.IsValid()) {
    return ServiceError{.code = ErrorCode::kInvalidInput,
                        .safe_message = "世界访问查询缺少有效玩家标识"};
  }
  auto result = reader_.Load(player_id);
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  auto snapshot =
      std::get<std::optional<WorldAccessSnapshot>>(std::move(result));
  if (!snapshot.has_value()) {
    return ServiceError{.code = ErrorCode::kInvalidState,
                        .safe_message = "当前玩家资料不存在"};
  }
  if (snapshot->player_id != player_id ||
      std::ranges::any_of(snapshot->chat_blocked_peers,
                          [player_id](domain::PlayerId peer) {
                            return !peer.IsValid() || peer == player_id;
                          }) ||
      !std::ranges::is_sorted(snapshot->chat_blocked_peers) ||
      std::ranges::adjacent_find(snapshot->chat_blocked_peers) !=
          snapshot->chat_blocked_peers.end()) {
    return ServiceError{
        .code = ErrorCode::kInternal,
        .safe_message = "世界访问快照不满足身份、排序或唯一性契约"};
  }
  return *snapshot;
}

}  // namespace bf::application
