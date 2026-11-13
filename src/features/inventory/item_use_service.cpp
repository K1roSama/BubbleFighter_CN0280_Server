#include "features/inventory/item_use_service.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>
#include <utility>

#include "features/characters/name_policy.h"
#include "features/chat/megaphone_event.h"
#include "features/chat/user_message.h"
#include "features/inventory/life_item.h"

namespace bf::application {
namespace {
// Build 752 ItemInfo.useType=1，颜色参数来自对象 +300。
constexpr std::array<std::pair<std::uint64_t, std::uint32_t>, 12> kDyes{
    {{100064U, 1U},
     {100065U, 2U},
     {100066U, 3U},
     {100067U, 4U},
     {100079U, 5U},
     {100080U, 6U},
     {100081U, 7U},
     {100082U, 8U},
     {100178U, 12U},
     {100179U, 13U},
     {100180U, 14U},
     {102086U, 15U}}};
}  // namespace

ItemUseService::ItemUseService(const SystemClock& clock,
                               ItemUseRepository& repository) noexcept
    : clock_(clock), repository_(repository) {}

ServiceResult<MegaphoneReceipt> ItemUseService::Megaphone(
    const MegaphoneCommand& command) const {
  if (!command.player_id.IsValid() ||
      (command.channel != 60U && command.channel != 80U) ||
      !domain::ValidMegaphoneSelection(command.type, command.selection)) {
    return ServiceError{.code = ErrorCode::kInvalidInput,
                        .safe_message = "喇叭类型或频道无效"};
  }
  const auto message = domain::UserMessage::Parse(command.text);
  if (const auto* error =
          std::get_if<domain::UserMessageValidationError>(&message)) {
    return ServiceError{.code = ErrorCode::kInvalidInput,
                        .safe_message = std::string{
                            domain::UserMessageValidationErrorMessage(*error)}};
  }
  return repository_.Megaphone(command, clock_.Now());
}

ServiceResult<std::vector<MegaphoneReceipt>> ItemUseService::PendingMegaphones()
    const {
  return repository_.PendingMegaphones();
}

std::optional<ServiceError> ItemUseService::AcknowledgeMegaphone(
    std::string_view id) const {
  return repository_.AcknowledgeMegaphone(id);
}

ServiceResult<UseItemResult> ItemUseService::Use(
    const UseItemCommand& command) const {
  const auto invalid = [] {
    return ServiceError{.code = ErrorCode::kInvalidInput,
                        .safe_message = "道具用途或使用参数尚不支持",
                        .correlation_id = {},
                        .is_retryable = false};
  };
  if (!command.player_id.IsValid() || command.quantity != 1U) {
    return invalid();
  }
  // Native CDCF70 -> D0EEB0 sends one syringe and an empty target, for ALL Life
  // items.
  if (command.item_id.Value() == domain::kLifeSyringe) {
    if (command.operation != ItemUseOperation::kUse || !command.target.empty())
      return invalid();
    return repository_.ReplenishLife(command.player_id, clock_.Now());
  }
  if (command.operation == ItemUseOperation::kUse &&
      command.item_id.Value() == 100104U) {
    const auto parsed = domain::Nickname::Parse(command.target);
    if (const auto* error =
            std::get_if<domain::NicknameValidationError>(&parsed)) {
      return ServiceError{.code = ErrorCode::kInvalidInput,
                          .safe_message = std::string{
                              domain::NicknameValidationErrorMessage(*error)}};
    }
    if (domain::IsRestrictedName(command.target)) {
      return ServiceError{
          .code = ErrorCode::kInvalidInput,
          .safe_message = "昵称包含禁用词或保留名称，请更换昵称"};
    }
    return repository_.Rename(command.player_id,
                              std::get<domain::Nickname>(parsed), clock_.Now());
  }
  // historical-export/current-bytes: ItemInfo function 24, 717020 passes one
  // card and "10". This is a fixed increment, not a client-selected capacity or
  // an Nx1102 recruitment mode.
  if (command.operation == ItemUseOperation::kUse &&
      command.item_id.Value() == 101062U) {
    if (command.target != "10") return invalid();
    return repository_.ExpandClan(command.player_id, clock_.Now());
  }
  if (command.operation == ItemUseOperation::kInstantRespawn) {
    if (command.item_id.Value() != 100443U || !command.target.empty() ||
        !command.match_id || command.life_generation == 0U) {
      return invalid();
    }
    return repository_.InstantRespawn(command, clock_.Now());
  }
  const bool food = command.item_id.Value() == 100643U ||
                    command.item_id.Value() == 100644U ||
                    command.item_id.Value() == 101605U;
  if (command.operation == ItemUseOperation::kPetLevelUp || food) {
    std::uint32_t pet = 0U;
    if (command.operation == ItemUseOperation::kPetLevelUp) {
      if (!command.target.empty() || command.item_id.Value() > 301021U) {
        return invalid();
      }
      pet = static_cast<std::uint32_t>(command.item_id.Value());
    } else {
      const auto converted =
          std::from_chars(command.target.data(),
                          command.target.data() + command.target.size(), pet);
      if (converted.ec != std::errc{} ||
          converted.ptr != command.target.data() + command.target.size()) {
        return invalid();
      }
    }
    if (pet < 301001U || pet > 301021U) {
      return invalid();
    }
    return repository_.GrowPet(
        {.player_id = command.player_id,
         .pet_id = domain::ItemId{pet},
         .food_id = command.operation == ItemUseOperation::kPetLevelUp
                        ? domain::ItemId{}
                        : command.item_id,
         .maximum_level = pet == 301015U ? 1U : 2U},
        clock_.Now());
  }
  const auto dye = std::ranges::find_if(kDyes, [&command](const auto& entry) {
    return entry.first == command.item_id.Value();
  });
  if (!command.target.empty() || dye == kDyes.end()) {
    return invalid();
  }
  return repository_.Dye(command.player_id, command.item_id,
                         static_cast<std::uint16_t>(dye->second), clock_.Now());
}
}  // namespace bf::application
