#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_TARGET_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_TARGET_H_

/**
 * @file social_target.h
 * @brief 按游戏昵称或稳定玩家编号定位社交对象。
 */

#include <charconv>
#include <limits>
#include <optional>
#include <string>

#include "application/identifier.h"
#include "features/authentication/account_credentials.h"
#include "features/characters/nickname.h"

namespace bf::domain {

// Text entry resolves a game nickname. Existing native messenger rows already
// contain a player ID: actions on a row use that stable identity after renames.
/** @brief 输入昵称或原生好友列表绑定的玩家编号；不查找登录账号。 */
class SocialTarget final {
 public:
  SocialTarget(const AccountName& legacy)
      : name_(legacy.DisplayName()), key_(legacy.Key()) {}

  [[nodiscard]] static std::optional<SocialTarget> Parse(
      std::string_view text) {
    if (text.starts_with('#')) {
      std::uint32_t value{};
      const auto parsed =
          std::from_chars(text.data() + 1, text.data() + text.size(), value);
      if (value == 0U || parsed.ec != std::errc{} ||
          parsed.ptr != text.data() + text.size())
        return std::nullopt;
      return SocialTarget{PlayerId{value}};
    }
    auto parsed = Nickname::Parse(text);
    if (const auto* name = std::get_if<Nickname>(&parsed))
      return SocialTarget{*name};
    // Imported characters can retain short/long ASCII names that predate the
    // current character-creation rules. This still searches nickname_key only.
    const auto legacy = AccountName::Parse(text);
    if (const auto* name = std::get_if<AccountName>(&legacy))
      return SocialTarget{*name};
    return std::nullopt;
  }

  [[nodiscard]] const std::string& Key() const noexcept { return key_; }

  [[nodiscard]] const std::string& DisplayName() const noexcept {
    return name_;
  }

  [[nodiscard]] domain::PlayerId GetPlayerId() const noexcept {
    return player_id_;
  }

  [[nodiscard]] bool Matches(PlayerId id, std::string_view nickname) const {
    if (player_id_.IsValid()) return player_id_ == id;
    const auto value = Parse(nickname);
    return value && !value->GetPlayerId().IsValid() && value->Key() == key_;
  }

 private:
  explicit SocialTarget(PlayerId id) : player_id_(id) {}

  explicit SocialTarget(const Nickname& name)
      : name_(name.DisplayName()), key_(name.Key()) {}

  PlayerId player_id_;
  std::string name_;
  std::string key_;
};

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_TARGET_H_
