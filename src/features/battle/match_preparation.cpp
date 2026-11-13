#include "features/battle/match_preparation.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <variant>

#include "features/rooms/room_item_policy.h"

namespace bf::application {
namespace {

ServiceError MakeError(ErrorCode code, std::string message) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = false};
}

}  // namespace

MatchPreparationService::MatchPreparationService(
    MatchPreparationReader& reader) noexcept
    : reader_(reader) {}

ServiceResult<PreparedMatch> MatchPreparationService::Prepare(
    MatchPreparationRequest request) const {
  if (!domain::ValidRoomItems(request.restricted_items))
    return MakeError(ErrorCode::kInvalidInput, "房间禁用物品无效");
  const auto score_model = domain::GetMatchScoreModel(request.mode_id);
  if (!request.room_id.IsValid() || !score_model.has_value() ||
      request.participants.empty() || request.participants.size() > 16U ||
      (request.mode_id == 26U && (request.participants.size() < 6U ||
                                  request.participants.size() > 8U)) ||
      (request.mode_id == 24U && request.participants.size() < 2U)) {
    return MakeError(ErrorCode::kInvalidState,
                     "该房间模式尚未具备可持久化的普通对局开局规则");
  }

  std::set<domain::PlayerId> player_ids;
  std::set<std::uint8_t> seats;
  std::set<std::uint32_t> objects;
  for (const auto& participant : request.participants) {
    if (!participant.player_id.IsValid() || participant.seat_index >= 16U ||
        ((request.mode_id == 13U || request.mode_id == 23U)
             ? (participant.team_code < 1U || participant.team_code > 16U)
             : (participant.team_code != 1U && participant.team_code != 2U)) ||
        participant.actor_object_id == 0U ||
        !player_ids.insert(participant.player_id).second ||
        !seats.insert(participant.seat_index).second ||
        !objects.insert(participant.actor_object_id).second) {
      return MakeError(ErrorCode::kInvalidInput,
                       "对局开局成员身份、席位、队伍或对象非法");
    }
  }
  std::ranges::sort(request.participants, {},
                    &MatchPreparationParticipantInput::player_id);

  auto loaded = reader_.Load(request);
  if (const auto* error = std::get_if<ServiceError>(&loaded)) {
    return *error;
  }
  auto result = std::get<PreparedMatch>(std::move(loaded));
  if (result.room_id != request.room_id || result.mode_id != request.mode_id ||
      result.configuration_version_id <= 0 ||
      result.reward_rules_version_id <= 0 ||
      result.score_model != *score_model ||
      result.ordinary_respawn_allowed !=
          domain::OrdinaryMatchRespawn(request.mode_id) ||
      result.participants.size() != request.participants.size()) {
    return MakeError(ErrorCode::kInternal,
                     "开局准备仓储返回的版本或模式契约不一致");
  }
  for (std::size_t index = 0U; index < request.participants.size(); ++index) {
    if (result.participants[index].identity.player_id !=
            request.participants[index].player_id ||
        result.participants[index].identity.seat_index !=
            request.participants[index].seat_index ||
        result.participants[index].identity.team_code !=
            request.participants[index].team_code ||
        result.participants[index].identity.actor_object_id !=
            request.participants[index].actor_object_id) {
      return MakeError(ErrorCode::kInternal,
                       "开局准备仓储改变了房间冻结成员身份");
    }
    const auto& participant = result.participants[index];
    const auto restriction = domain::RestrictedEquipmentNotice(
        request.restricted_items, participant.equipment_items);
    if (!restriction.empty())
      return MakeError(ErrorCode::kConflict, restriction);
    std::set<std::uint32_t> attribute_owners;
    for (const auto& item : participant.equipment_attributes) {
      if (item.attributes.empty() ||
          !attribute_owners.insert(item.item_id).second ||
          std::ranges::find(participant.equipment_items, item.item_id) ==
              participant.equipment_items.end() ||
          !domain::ValidNativeItemAttributes(item.item_id, item.attributes)) {
        return MakeError(ErrorCode::kInternal,
                         "开局动态属性未绑定唯一有效穿戴物品");
      }
    }
  }
  return result;
}

}  // namespace bf::application
