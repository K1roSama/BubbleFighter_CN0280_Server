#include "features/shop/workshop_service.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace bf::application {
namespace {

ServiceError Invalid(std::string message) {
  return {.code = ErrorCode::kInvalidInput, .safe_message = std::move(message)};
}

const WorkshopItemRule* ItemRule(const WorkshopServiceRules& rules,
                                 domain::ItemId item_id) {
  const auto found = std::ranges::lower_bound(rules.item_rules, item_id, {},
                                              &WorkshopItemRule::item_id);
  return found != rules.item_rules.end() && found->item_id == item_id ? &*found
                                                                      : nullptr;
}

bool ActionAllowed(const WorkshopItemRule& rule,
                   WorkshopAction action) noexcept {
  switch (action) {
    case WorkshopAction::kCompound:
      return rule.compound;
    case WorkshopAction::kDecompose:
      return rule.decompose;
    case WorkshopAction::kPermanent:
      return rule.permanent;
  }
  return false;
}

}  // namespace

WorkshopService::WorkshopService(const SystemClock& clock,
                                 ServiceRulesLoader& rules,
                                 WorkshopRandomSource& random,
                                 WorkshopRepository& repository) noexcept
    : clock_(clock), rules_(rules), random_(random), repository_(repository) {}

ServiceResult<WorkshopCompoundConfiguration>
WorkshopService::CompoundConfiguration() const {
  auto loaded = rules_.Load();
  if (const auto* error = std::get_if<ServiceError>(&loaded)) {
    return *error;
  }
  const auto& rules = std::get<PublishedServiceRules>(loaded).workshop;
  if (!rules.compound_policy.has_value()) {
    return ServiceError{
        .code = ErrorCode::kInvalidState,
        .safe_message = "尚未发布按原版材料和期限规则恢复的合成政策"};
  }
  return WorkshopCompoundConfiguration{
      .compound_card_item_id = rules.compound_card_item_id,
      .protection_material_item_id =
          rules.compound_policy->protection_material_item_id,
      .minimum_remaining_days = rules.compound_policy->minimum_remaining_days};
}

ServiceResult<WorkshopResult> WorkshopService::Execute(
    domain::OperationId operation_id, domain::PlayerId player_id,
    WorkshopAction action, std::span<const std::uint32_t> input_item_ids,
    std::uint32_t option) const {
  if (!player_id.IsValid() || input_item_ids.empty() ||
      input_item_ids.size() > 32U) {
    return Invalid("请选择不同的有效装备");
  }
  std::vector<domain::ItemId> inputs;
  inputs.reserve(input_item_ids.size());
  std::set<std::uint64_t> unique;
  for (const auto raw : input_item_ids) {
    domain::ItemId item{raw};
    if (!item.IsValid() || !unique.insert(item.Value()).second) {
      return Invalid("请选择不同的有效装备");
    }
    inputs.push_back(item);
  }
  auto loaded = rules_.Load();
  if (const auto* error = std::get_if<ServiceError>(&loaded)) {
    return *error;
  }
  const auto& published = std::get<PublishedServiceRules>(loaded);
  const auto& rules = published.workshop;
  if (action == WorkshopAction::kCompound &&
      !rules.compound_policy.has_value()) {
    return ServiceError{
        .code = ErrorCode::kInvalidState,
        .safe_message =
            "尚未发布按原版材料和期限规则恢复的合成政策，材料未扣除"};
  }
  for (const auto input : inputs) {
    const auto* rule = ItemRule(rules, input);
    if (rule == nullptr || !ActionAllowed(*rule, action) ||
        input == rules.compound_card_item_id ||
        input == rules.decompose_card_item_id ||
        input == rules.permanent_material_item_id ||
        input == rules.decompose_reward_item_id) {
      return Invalid("所选装备不支持这项操作");
    }
  }

  domain::ItemId reward{0U};
  domain::ItemId material{0U};
  std::chrono::seconds reward_duration{0};
  std::uint32_t compound_percent = 0U;
  std::uint32_t protection_percent = 0U;
  if (action == WorkshopAction::kCompound) {
    const auto& policy = *rules.compound_policy;
    if (inputs.size() != 5U || inputs.size() != policy.input_count ||
        option > 5U) {
      return Invalid("合成需要五件不同装备，保护选项须为0或注册槽1到5");
    }
    if (std::ranges::any_of(inputs, [&](domain::ItemId input) {
          const auto* rule = ItemRule(rules, input);
          return rule->native_is_gashapon != 1U ||
                 rule->native_is_compound != 1U ||
                 rule->native_duration_type != "day";
        })) {
      return Invalid("请选择原版支持合成且带神秘泡泡标记的限时装备");
    }
    // confirmCompound原公开文案确定候选来自注册材料；均匀分布是明确发布的本服策略。
    auto choice = random_.Choose(inputs.size());
    if (const auto* error = std::get_if<ServiceError>(&choice)) {
      return *error;
    }
    const auto index = std::get<std::size_t>(choice);
    if (index >= inputs.size()) {
      return ServiceError{.code = ErrorCode::kInternal,
                          .safe_message = "工坊随机源返回越界索引"};
    }
    reward = inputs[index];
    const auto percent_count =
        policy.maximum_percent - policy.minimum_percent + 1U;
    auto percent = random_.Choose(percent_count);
    if (const auto* error = std::get_if<ServiceError>(&percent)) {
      return *error;
    }
    if (std::get<std::size_t>(percent) >= percent_count) {
      return ServiceError{.code = ErrorCode::kInternal,
                          .safe_message = "工坊随机源返回越界比例"};
    }
    compound_percent =
        policy.minimum_percent +
        static_cast<std::uint32_t>(std::get<std::size_t>(percent));
    if (option != 0U) {
      auto protected_percent = random_.Choose(percent_count);
      if (const auto* error = std::get_if<ServiceError>(&protected_percent)) {
        return *error;
      }
      if (std::get<std::size_t>(protected_percent) >= percent_count) {
        return ServiceError{.code = ErrorCode::kInternal,
                            .safe_message = "工坊随机源返回越界保护比例"};
      }
      protection_percent =
          policy.minimum_percent +
          static_cast<std::uint32_t>(std::get<std::size_t>(protected_percent));
    }
    material = rules.compound_card_item_id;
  } else if (action == WorkshopAction::kDecompose) {
    if (inputs.size() > rules.decompose_max || option != 0U) {
      return Invalid("分解参数无效");
    }
    reward = rules.decompose_reward_item_id;
    material = rules.decompose_card_item_id;
  } else if (action == WorkshopAction::kPermanent) {
    if (inputs.size() != 1U || option != 0U) {
      return Invalid("请选择一件限时装备");
    }
    reward = inputs.front();
    material = rules.permanent_material_item_id;
  } else {
    return Invalid("该操作的原版属性协议仍在接入，材料未扣除");
  }
  auto result = repository_.Execute(
      {.operation_id = operation_id,
       .player_id = player_id,
       .action = action,
       .inputs = std::move(inputs),
       .option = option,
       .rules_version_id = published.configuration_version_id,
       .reward_item_id = reward,
       .reward_duration = reward_duration,
       .required_material_id = material,
       .required_material_quantity = 1U,
       .now = clock_.Now(),
       .compound_policy = action == WorkshopAction::kCompound
                              ? rules.compound_policy
                              : std::nullopt,
       .compound_percent = compound_percent,
       .protection_percent = protection_percent});
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  const auto& completed = std::get<WorkshopResult>(result);
  if (completed.action != action || completed.rewards.empty()) {
    return ServiceError{.code = ErrorCode::kInternal,
                        .safe_message = "工坊事务返回无效结果"};
  }
  return result;
}

}  // namespace bf::application
