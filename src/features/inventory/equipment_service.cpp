#include "features/inventory/equipment_service.h"

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace bf::application {
namespace {

constexpr std::size_t kMaximumSlotCodeBytes = 64U;

ServiceError InvalidCommand() {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = "换装请求缺少有效的玩家或物品标识"};
}

ServiceError TransactionContractError(std::string_view detail) {
  return ServiceError{.code = ErrorCode::kInternal,
                      .safe_message = std::string{"换装事务返回了无效状态："} +
                                      std::string{detail}};
}

bool ValidItem(const ActiveEquipableItem& item,
               const ChangeEquipmentCommand& command) noexcept {
  return item.item_instance_id.IsValid() && item.item_id == command.item_id &&
         !item.slot_code.empty() &&
         item.slot_code.size() <= kMaximumSlotCodeBytes;
}

}  // namespace

ChangeEquipmentService::ChangeEquipmentService(
    const SystemClock& clock, ChangeEquipmentUnitOfWork& unit_of_work) noexcept
    : clock_(clock), unit_of_work_(unit_of_work) {}

ServiceResult<ChangeEquipmentResult> ChangeEquipmentService::Change(
    const ChangeEquipmentCommand& command) const {
  if (!command.player_id.IsValid() || !command.item_id.IsValid()) {
    return InvalidCommand();
  }

  auto begin_result = unit_of_work_.Begin();
  if (const auto* error = std::get_if<ServiceError>(&begin_result)) {
    return *error;
  }
  auto transaction = std::get<std::unique_ptr<ChangeEquipmentTransaction>>(
      std::move(begin_result));
  if (!transaction) {
    return TransactionContractError("事务工厂返回空对象");
  }

  auto item_result = transaction->LockActiveItem(command.player_id,
                                                 command.item_id, clock_.Now());
  if (const auto* error = std::get_if<ServiceError>(&item_result)) {
    return *error;
  }
  auto item = std::get<ActiveEquipableItem>(std::move(item_result));
  if (!ValidItem(item, command)) {
    return TransactionContractError("库存实例、模板或槽位不一致");
  }

  std::optional<ServiceError> mutation_error;
  if (command.action == EquipmentAction::kEquip) {
    mutation_error = transaction->Equip(command.player_id, command.loadout_id,
                                        item.slot_code, item.item_instance_id);
  } else {
    mutation_error =
        transaction->UnequipIfCurrent(command.player_id, command.loadout_id,
                                      item.slot_code, item.item_instance_id);
  }
  if (mutation_error.has_value()) {
    return *mutation_error;
  }
  if (auto error = transaction->Commit()) {
    return *error;
  }
  return ChangeEquipmentResult{.player_id = command.player_id,
                               .item_id = item.item_id,
                               .item_instance_id = item.item_instance_id,
                               .slot_code = std::move(item.slot_code),
                               .action = command.action};
}

std::optional<ServiceError> ChangeEquipmentService::SelectPreset(
    domain::PlayerId player_id, std::uint8_t preset,
    std::uint32_t combat_mode) const {
  if (!player_id.IsValid() || preset > 2U) {
    return InvalidCommand();
  }
  auto result = unit_of_work_.Begin();
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  auto transaction =
      std::get<std::unique_ptr<ChangeEquipmentTransaction>>(std::move(result));
  if (!transaction) {
    return TransactionContractError("预设事务工厂返回空对象");
  }
  if (auto error = transaction->SelectPreset(player_id, preset, clock_.Now(),
                                             combat_mode)) {
    return error;
  }
  return transaction->Commit();
}

}  // namespace bf::application
