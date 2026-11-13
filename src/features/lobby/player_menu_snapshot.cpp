#include "features/lobby/player_menu_snapshot.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "application/utf8.h"
#include "features/inventory/life_item.h"

namespace bf::application {
namespace {

constexpr std::size_t kMaximumSlotCodeBytes = 64U;

ServiceError InvalidPlayer() {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = "菜单快照请求缺少有效玩家标识"};
}

ServiceError MissingPlayer() {
  return ServiceError{.code = ErrorCode::kInvalidState,
                      .safe_message = "玩家尚未完成可进入菜单的角色资料"};
}

ServiceError ContractError(std::string_view detail) {
  return ServiceError{
      .code = ErrorCode::kInternal,
      .safe_message =
          std::string{"菜单快照仓储返回了无效状态："} + std::string{detail}};
}

bool ValidProgress(const PlayerProgressView& progress) noexcept {
  return progress.level > 0 && progress.experience >= 0 &&
         progress.level_base >= 0 && progress.level_next > 0 &&
         progress.level_base <= progress.experience &&
         progress.experience < progress.level_next;
}

bool ValidWallets(const PlayerWalletView& wallets) noexcept {
  return wallets.lucci >= 0 && wallets.bubble_cash >= 0 &&
         wallets.treasure_bubble >= 0;
}

std::optional<ServiceError> ValidateItems(
    const PlayerMenuSnapshot& snapshot,
    std::chrono::system_clock::time_point expected_observed_at) {
  std::unordered_set<std::uint64_t> instances;
  std::unordered_set<std::uint64_t> templates;
  std::unordered_set<std::string> equipped_slots;
  instances.reserve(snapshot.items.size());
  templates.reserve(snapshot.items.size());
  equipped_slots.reserve(snapshot.items.size());
  std::uint64_t previous_instance_id = 0U;

  for (const auto& item : snapshot.items) {
    const bool depleted_life =
        item.quantity == 0 && domain::IsLifeItem(item.item_id.Value()) &&
        !item.expires_at && !item.equipped && !item.pet_growth;
    if (!item.item_instance_id.IsValid() || !item.item_id.IsValid() ||
        (item.quantity <= 0 && !depleted_life)) {
      return ContractError("库存包含无效标识或非正数量");
    }
    if (item.item_instance_id.Value() <= previous_instance_id) {
      return ContractError("库存没有按实例标识严格升序返回");
    }
    previous_instance_id = item.item_instance_id.Value();
    if (!instances.insert(item.item_instance_id.Value()).second ||
        !templates.insert(item.item_id.Value()).second) {
      return ContractError("库存实例或模板重复");
    }
    if (item.expires_at.has_value() &&
        *item.expires_at <= expected_observed_at) {
      return ContractError("读取结果包含已过期物品");
    }
    if (item.slot_code.size() > kMaximumSlotCodeBytes) {
      return ContractError("装备槽代码超出数据库约束");
    }
    if (item.pet_growth.has_value() &&
        (item.slot_code != "pet" || item.pet_growth->level > 2U ||
         item.pet_growth->experience > 100U)) {
      return ContractError("宠物成长状态或所属物品类型无效");
    }
    if (!domain::ValidNativeItemAttributes(item.item_id.Value(),
                                           item.attributes) ||
        (!item.attributes.empty() &&
         (item.quantity != 1 || item.expires_at || item.pet_growth ||
          item.slot_code.empty()))) {
      return ContractError(
          "动态属性必须属于当前永久服装实例并具有已恢复的完整数值");
    }
    if (item.equipped) {
      if (item.slot_code.empty()) {
        return ContractError("已装备物品没有模板槽位");
      }
      if (!equipped_slots.insert(item.slot_code).second) {
        return ContractError("同一配装页存在重复装备槽");
      }
    }
  }
  previous_instance_id = 0U;
  for (const auto& item : snapshot.expired_items) {
    if (!item.item_instance_id.IsValid() || !item.item_id.IsValid() ||
        item.quantity <= 0 ||
        item.item_instance_id.Value() <= previous_instance_id ||
        !item.expires_at || *item.expires_at > expected_observed_at ||
        *item.expires_at <= item.acquired_at ||
        item.slot_code.size() > kMaximumSlotCodeBytes ||
        !item.attributes.empty() ||
        !instances.insert(item.item_instance_id.Value()).second ||
        !templates.insert(item.item_id.Value()).second)
      return ContractError(
          "过期仓库行必须具有独立实例、已到期时间和唯一原生键");
    previous_instance_id = item.item_instance_id.Value();
  }
  return std::nullopt;
}

}  // namespace

PlayerMenuSnapshotService::PlayerMenuSnapshotService(
    const SystemClock& clock, PlayerMenuSnapshotReader& reader) noexcept
    : clock_(clock), reader_(reader) {}

ServiceResult<PlayerMenuSnapshot> PlayerMenuSnapshotService::Load(
    domain::PlayerId player_id) const {
  if (!player_id.IsValid()) {
    return InvalidPlayer();
  }

  const auto now = clock_.Now();
  auto result = reader_.Load(player_id, now);
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  auto optional_snapshot =
      std::get<std::optional<PlayerMenuSnapshot>>(std::move(result));
  if (!optional_snapshot.has_value()) {
    return MissingPlayer();
  }

  auto snapshot = std::move(*optional_snapshot);
  if (snapshot.level_icon_type > 3U)
    return ContractError("等级图标偏好超出原生类型范围");
  if (snapshot.player_id != player_id) {
    return ContractError("玩家标识与查询条件不一致");
  }
  if (!domain::CountUtf8CodePoints(snapshot.nickname).has_value() ||
      snapshot.nickname.empty()) {
    return ContractError("昵称不是有效的非空 UTF-8");
  }
  if (!domain::InitialFighterItem(snapshot.initial_fighter).IsValid()) {
    return ContractError("回退角色编号无效");
  }
  if (!ValidProgress(snapshot.progress) || !ValidWallets(snapshot.wallets)) {
    return ContractError("经验等级或钱包余额违反约束");
  }
  if (!domain::CountUtf8CodePoints(snapshot.login_name).has_value() ||
      !domain::CountUtf8CodePoints(snapshot.clan_name).has_value()) {
    return ContractError("公开资料文字编码无效");
  }
  std::optional<std::uint32_t> previous_mode;
  for (const auto& statistics : snapshot.mode_statistics) {
    if ((previous_mode && statistics.mode_id <= *previous_mode) ||
        statistics.wins > statistics.matches ||
        statistics.losses > statistics.matches - statistics.wins ||
        statistics.draws >
            statistics.matches - statistics.wins - statistics.losses) {
      return ContractError("公开战绩包含重复模式或不一致胜负计数");
    }
    previous_mode = statistics.mode_id;
  }
  if (snapshot.medal_progress) {
    if (snapshot.medal_progress->size() > 4096U) {
      return ContractError("勋章账本超出原生列表上限");
    }
    std::uint32_t previous_key = 0U;
    for (const auto& medal : *snapshot.medal_progress) {
      if (medal.medal_key <= previous_key || medal.medal_key % 10'000U == 0U ||
          medal.count == 0U) {
        return ContractError("勋章账本含重复或无效键、非正计数");
      }
      previous_key = medal.medal_key;
    }
  }
  if (auto error = ValidateItems(snapshot, now)) {
    return *error;
  }
  snapshot.observed_at = now;
  return snapshot;
}

}  // namespace bf::application
