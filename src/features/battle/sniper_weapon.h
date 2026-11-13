#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_SNIPER_WEAPON_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_SNIPER_WEAPON_H_

/**
 * @file sniper_weapon.h
 * @brief 从原生类别恢复狙击武器集合，用于开局与预设事务校验。
 */

#include <algorithm>
#include <array>
#include <cstdint>

namespace bf::domain {

// Generated from weapon_categories.json; do not replace with an ID range.
// Evidence SHA256:
// a58fc1203fbda76b6affccdac58e03bb147533260b9059b371da7174845e2bcd
/** @brief Match the exact inherited weaponCategory used by native
 * 8A22B0/7E7090. */
[[nodiscard]] constexpr bool IsNativeSniperWeapon(
    std::uint32_t weapon) noexcept {
  constexpr std::array<std::uint32_t, 148U> templates{
      4000U, 4001U, 4002U, 4004U, 4011U, 4012U, 4013U, 4014U, 4015U, 4021U,
      4022U, 4023U, 4024U, 4025U, 4026U, 4027U, 4032U, 4033U, 4034U, 4041U,
      4042U, 4043U, 4044U, 4045U, 4052U, 4053U, 4101U, 4107U, 4108U, 4109U,
      4110U, 4111U, 4112U, 4113U, 4114U, 4116U, 4117U, 4118U, 4119U, 4120U,
      4121U, 4123U, 4124U, 4125U, 4126U, 4127U, 4128U, 4131U, 4223U, 4227U,
      4328U, 4333U, 4335U, 4336U, 4338U, 4341U, 4342U, 4343U, 4345U, 4346U,
      4347U, 4348U, 4349U, 4350U, 4351U, 4352U, 4353U, 4354U, 4355U, 4356U,
      4357U, 4359U, 4360U, 4361U, 4364U, 4365U, 4367U, 4368U, 4369U, 4370U,
      4371U, 4372U, 4373U, 4374U, 4375U, 4377U, 4378U, 4379U, 4380U, 4381U,
      4382U, 4384U, 4385U, 4386U, 4388U, 4389U, 4390U, 4391U, 4392U, 4402U,
      4403U, 4404U, 4502U, 4503U, 4602U, 4702U, 4802U, 4902U, 4903U, 4904U,
      4905U, 4910U, 4911U, 4912U, 4915U, 4916U, 4917U, 4924U, 4927U, 4928U,
      4929U, 4930U, 4931U, 4932U, 4933U, 4934U, 4938U, 4939U, 4940U, 4941U,
      4942U, 4944U, 4947U, 4948U, 4949U, 4950U, 4951U, 4952U, 4953U, 4954U,
      4955U, 4956U, 4957U, 4958U, 4963U, 4965U, 4966U, 4968U,
  };
  return std::ranges::binary_search(templates, weapon);
}

/** @brief Both Sniper and SniperTeamSurvival require a SniperRifle primary. */
[[nodiscard]] constexpr bool ModeRequiresSniperPrimary(
    std::uint32_t mode) noexcept {
  return mode == 12U || mode == 18U;
}

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_SNIPER_WEAPON_H_
