#include "features/characters/character.h"

namespace bf::domain {

std::optional<InitialFighter> InitialFighterFromProtocol(
    std::uint32_t protocol_value) noexcept {
  switch (protocol_value) {
    case 1U:
      return InitialFighter::kDao;
    case 2U:
      return InitialFighter::kDizni;
    case 11U:
      return InitialFighter::kReo;
    case 12U:
      return InitialFighter::kRabi;
    default:
      return std::nullopt;
  }
}

ItemId InitialFighterItem(InitialFighter fighter) noexcept {
  switch (fighter) {
    case InitialFighter::kDao:
      return ItemId{300'001U};
    case InitialFighter::kDizni:
      return ItemId{300'002U};
    case InitialFighter::kReo:
      return ItemId{300'011U};
    case InitialFighter::kRabi:
      return ItemId{300'012U};
  }
  return ItemId{};
}

std::string_view InitialFighterName(InitialFighter fighter) noexcept {
  switch (fighter) {
    case InitialFighter::kDao:
      return "dao";
    case InitialFighter::kDizni:
      return "dizni";
    case InitialFighter::kReo:
      return "reo";
    case InitialFighter::kRabi:
      return "rabi";
  }
  return "unknown";
}

StarterItemDefinitions GetStarterItemDefinitions(
    InitialFighter fighter) noexcept {
  return StarterItemDefinitions{{
      {.item_id = InitialFighterItem(fighter), .equip = true},
      {.item_id = ItemId{202'001U}, .equip = true},
      {.item_id = ItemId{201'001U}, .equip = true},
      {.item_id = ItemId{207'001U}, .equip = true},
      {.item_id = ItemId{202'004U}, .equip = false},
      {.item_id = ItemId{203'004U}, .equip = false},
      {.item_id = ItemId{204'004U}, .equip = false},
  }};
}

}  // namespace bf::domain
