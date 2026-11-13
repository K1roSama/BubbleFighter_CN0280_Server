#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NAME_POLICY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NAME_POLICY_H_

/** @file name_policy.h
 *  @brief 新注册账号、角色昵称及改名共用的离线名称过滤规则。
 */
#include <string_view>

namespace bf::domain {
/** @brief 检查已通过格式校验的名称，不修改展示名或数据库唯一键。 */
[[nodiscard]] bool IsRestrictedName(std::string_view name);
}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NAME_POLICY_H_
