#include "features/characters/character_creation.h"

#include <set>
#include <utility>
#include <vector>

#include "features/characters/name_policy.h"

namespace bf::application {
namespace {

ServiceError InvalidPlayerId() {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = "玩家标识必须是已分配的非零值"};
}

ServiceError InvalidNickname(domain::NicknameValidationError error) {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = std::string{
                          domain::NicknameValidationErrorMessage(error)}};
}

ServiceError InvalidFighter() {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = "请选择原版初始角色"};
}

ServiceError NicknameConflict() {
  return ServiceError{.code = ErrorCode::kConflict,
                      .safe_message = "该昵称已被使用，请换一个昵称"};
}

ServiceError AlreadyCreatedConflict() {
  return ServiceError{.code = ErrorCode::kConflict,
                      .safe_message = "此账号已经创建角色"};
}

ServiceError TransactionContractError(std::string_view detail) {
  return ServiceError{
      .code = ErrorCode::kInternal,
      .safe_message =
          std::string{"角色创建事务返回了无效状态："} + std::string{detail}};
}

ServiceResult<domain::Nickname> ParseNickname(std::string_view nickname) {
  auto parsed = domain::Nickname::Parse(nickname);
  if (const auto* error =
          std::get_if<domain::NicknameValidationError>(&parsed)) {
    return InvalidNickname(*error);
  }
  if (domain::IsRestrictedName(nickname)) {
    return ServiceError{.code = ErrorCode::kInvalidInput,
                        .safe_message = "昵称包含禁用词或保留名称，请更换昵称"};
  }
  return std::get<domain::Nickname>(std::move(parsed));
}

}  // namespace

CharacterCreationService::CharacterCreationService(
    CharacterCreationUnitOfWork& unit_of_work) noexcept
    : unit_of_work_(unit_of_work) {}

ServiceResult<NicknameCheckResult> CharacterCreationService::CheckNickname(
    const CheckNicknameCommand& command) const {
  if (!command.player_id.IsValid()) {
    return InvalidPlayerId();
  }
  auto nickname_result = ParseNickname(command.nickname);
  if (const auto* error = std::get_if<ServiceError>(&nickname_result)) {
    return *error;
  }
  auto nickname = std::get<domain::Nickname>(std::move(nickname_result));

  auto begin_result = unit_of_work_.Begin();
  if (const auto* error = std::get_if<ServiceError>(&begin_result)) {
    return *error;
  }
  auto transaction = std::get<std::unique_ptr<CharacterCreationTransaction>>(
      std::move(begin_result));
  if (!transaction) {
    return TransactionContractError("事务工厂返回空对象");
  }

  auto in_use_result =
      transaction->NicknameKeyInUse(nickname.Key(), command.player_id);
  if (const auto* error = std::get_if<ServiceError>(&in_use_result)) {
    return *error;
  }
  if (std::get<bool>(in_use_result)) {
    return NicknameConflict();
  }
  if (auto error = transaction->Commit()) {
    return *error;
  }
  return NicknameCheckResult{.nickname = nickname.DisplayName(),
                             .nickname_key = nickname.Key()};
}

ServiceResult<CharacterCreationResult>
CharacterCreationService::CreateCharacter(
    const CreateCharacterCommand& command) const {
  if (!command.player_id.IsValid()) {
    return InvalidPlayerId();
  }
  auto nickname_result = ParseNickname(command.nickname);
  if (const auto* error = std::get_if<ServiceError>(&nickname_result)) {
    return *error;
  }
  auto nickname = std::get<domain::Nickname>(std::move(nickname_result));

  const auto fighter = domain::InitialFighterFromProtocol(command.fighter_id);
  if (!fighter.has_value()) {
    return InvalidFighter();
  }

  auto begin_result = unit_of_work_.Begin();
  if (const auto* error = std::get_if<ServiceError>(&begin_result)) {
    return *error;
  }
  auto transaction = std::get<std::unique_ptr<CharacterCreationTransaction>>(
      std::move(begin_result));
  if (!transaction) {
    return TransactionContractError("事务工厂返回空对象");
  }

  auto profile_result = transaction->LockProfile(command.player_id);
  if (const auto* error = std::get_if<ServiceError>(&profile_result)) {
    return *error;
  }
  const auto& profile =
      std::get<CharacterCreationProfileSnapshot>(profile_result);
  if (profile.player_id != command.player_id) {
    return TransactionContractError("锁定资料与请求玩家不一致");
  }
  if (profile.is_character_created) {
    if (profile.nickname != nickname.DisplayName() ||
        profile.initial_fighter != fighter) {
      return AlreadyCreatedConflict();
    }
    if (auto error = transaction->Commit()) {
      return *error;
    }
    return CharacterCreationResult{
        .player_id = command.player_id,
        .nickname = nickname.DisplayName(),
        .fighter = *fighter,
        .starter_item_count =
            domain::GetStarterItemDefinitions(*fighter).size(),
        .already_completed = true};
  }

  auto in_use_result =
      transaction->NicknameKeyInUse(nickname.Key(), command.player_id);
  if (const auto* error = std::get_if<ServiceError>(&in_use_result)) {
    return *error;
  }
  if (std::get<bool>(in_use_result)) {
    return NicknameConflict();
  }

  const auto definitions = domain::GetStarterItemDefinitions(*fighter);
  std::vector<StarterCatalogItem> catalog_items;
  catalog_items.reserve(definitions.size());
  std::set<std::string> equipped_slots;
  for (const auto& definition : definitions) {
    auto item_result = transaction->LockEnabledStarterItem(definition.item_id);
    if (const auto* error = std::get_if<ServiceError>(&item_result)) {
      return *error;
    }
    auto item = std::get<StarterCatalogItem>(std::move(item_result));
    if (item.item_id != definition.item_id) {
      return TransactionContractError("目录模板编号与请求不一致");
    }
    if (definition.equip) {
      if (!item.slot_code.has_value() || item.slot_code->empty()) {
        return TransactionContractError("待装备初始物品没有槽位");
      }
      if (!equipped_slots.insert(*item.slot_code).second) {
        return TransactionContractError("多个初始物品占用同一装备槽");
      }
    }
    catalog_items.push_back(std::move(item));
  }

  for (std::size_t index = 0U; index < definitions.size(); ++index) {
    auto grant_result = transaction->EnsureStarterItem(
        command.player_id, definitions[index].item_id);
    if (const auto* error = std::get_if<ServiceError>(&grant_result)) {
      return *error;
    }
    const auto instance_id = std::get<domain::ItemInstanceId>(grant_result);
    if (!instance_id.IsValid()) {
      return TransactionContractError("初始物品实例标识为零");
    }
    if (definitions[index].equip) {
      if (auto error = transaction->EquipItem(command.player_id,
                                              *catalog_items[index].slot_code,
                                              instance_id)) {
        return *error;
      }
    }
  }

  if (auto error =
          transaction->CompleteProfile(command.player_id, nickname, *fighter)) {
    return *error;
  }
  if (auto error = transaction->Commit()) {
    return *error;
  }

  return CharacterCreationResult{.player_id = command.player_id,
                                 .nickname = nickname.DisplayName(),
                                 .fighter = *fighter,
                                 .starter_item_count = definitions.size(),
                                 .already_completed = false};
}

}  // namespace bf::application
