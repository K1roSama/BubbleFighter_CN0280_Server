#include "network/gateway_opcode_routes.h"

#include "features/battle/match_objectives.h"
#include "features/battle/pirate_map_agents.h"
#include "features/battle/pirate_match.h"
#include "network/gift_handler.h"
#include "protocol/battle/combat_messages.h"
#include "protocol/battle/special_mode_messages.h"
#include "protocol/lobby/lobby_minigame_messages.h"

namespace bf::infrastructure::network {

bool IsWorldOpcode(std::uint16_t opcode) noexcept {
  switch (opcode) {
    case 108U:
    case 109U:
    case 110U:
    case 111U:
    case 112U:
    case 202U:
    case 302U:
    case 303U:
    case 305U:
    case 516U:
    case 2028U:  // 8A4FA0 empty user-host start
    case 2029U:  // B4F4A0 action + amount
    case 2021U:  // Captain restore receipt (55DD20 -> 569370)
    case 2031U:
    case 114U:
    case 141U:
    case 53U:
    case 105U:
    case 106U:
    case 107U:  // Original dedicated single-stage request; prologue103 only.
    case 401U:
    case 402U:
    case 403U:
    case 404U:
    case 502U:
    case 503U:
    case 504U:
    case 505U:
    case 506U:
    case 508U:
    case 509U:
    case 510U:
    case 511U:
    case 513U:
    case 514U:
    case 515U:
    case 517U:
    case 518U:
    case 601U:
    case 701U:
    case 702U:
    case 709U:
    case 754U:  // ExtremeRacing finish (7EB130)
    case 755U:  // ExtremeRacing coin (7EB1E0)
    case 756U:  // BFRunner goal team (7C3710), not survival S2C756.
    case 757U:  // BFRunner current fixed checkpoint (7C37C0).
    case 758U:  // BFRunner host checkpoint vector (7C3870).
    case 759U:  // BFRunner fixed run_item/booster region (7C3630).
    case 760U:
    case 32840U:
    case 950U:
    case 32922U:
    case 32824U:  // peer object event (7CA300)
    case 32832U:  // peer character summon (7CD430)
    case 32902U:  // ExtremeRacing cannon projectile (7C7D30)
    case 32934U:  // ExtremeRacing checkpoint (7C9A70)
    case 33275U:  // peer object state (7D6410)
    case 65032U:  // smart ping (client compatibility extension)
    case 65033U:  // private training entry (client compatibility extension)
    case 65034U:  // training dummy reset
    case 65035U:  // training temporary equipment
    case 65082U:  // hide_seek_v1 authenticated capability/round request
    case 65090U:  // per-room equipment restrictions, independent of hide-seek
    case 65087U:  // hide_seek_v1 development create entry
    case 65088U:  // native create dialog with its original configuration
    case 65070U:  // negotiated room network preference
    case 65073U:  // authenticated UDP candidate registration
    case 65075U:  // sequenced movement; server validation and TCP fallback
    case 65077U:  // bounded, authenticated per-peer ICE signalling
    case 572U:    // plaza minigame visual sync (LobbyMinigameContext)
      return true;
    default:
      return protocol::IsSpecialModeClaimOpcode(opcode) ||
             protocol::IsPveEventOpcode(opcode) ||
             runtime::PirateMatch::Accepts(opcode) ||
             runtime::PirateMapAgents::Accepts(opcode) ||
             runtime::PirateMapAgents::AcceptsLink(opcode) ||
             runtime::MatchObjectives::Accepts(opcode) ||
             protocol::IsNativeStormOpcode(opcode) ||
             protocol::IsNativeWeaponObjectOpcode(opcode) ||
             protocol::IsNativeActionOpcode(opcode) ||
             protocol::IsNativeHitOpcode(opcode) ||
             protocol::IsNativeLifecycleOpcode(opcode) ||
             protocol::IsNativeRescueOpcode(opcode) ||
             protocol::IsNativeRespawnOpcode(opcode);
  }
}

bool WorldOpcodeRequiresMenuSnapshot(std::uint16_t opcode) noexcept {
  // 所有会创建新场景快照或把玩家从房间带回广场/菜单的入口，都必须在数据库线程重新读取
  // 一致性资料。不能复用登录时快照，否则购买、换装或成长后的展示会永久陈旧。
  return opcode == 2028U || opcode == 2029U || opcode == 53U ||
         opcode == 105U || opcode == 106U || opcode == 402U || opcode == 403U ||
         opcode == 404U || opcode == 114U || opcode == 141U || opcode == 107U ||
         opcode == 65033U || opcode == 65087U || opcode == 65088U ||
         (opcode >= 109U && opcode <= 112U);
}

bool WorldOpcodeRequiresAccessSnapshot(std::uint16_t opcode) noexcept {
  return opcode == 53U || opcode == 502U || opcode == 305U || opcode == 516U ||
         opcode == 2031U || (opcode >= 109U && opcode <= 112U);
}

bool IsHandledBeforeWorldDispatch(std::uint16_t opcode) noexcept {
  if (IsGiftRequest(opcode)) {
    return true;
  }
  switch (opcode) {
    case 25U:   // 票据登录前探测
    case 119U:  // 战士资料
    case 115U:  // 原活动区关闭终态，不进入普通快速入房
    case 116U:  // 原活动区读取/取消
    case 120U:  // 战士称号
    case 129U:  // 神秘商店
    case 139U:  // 科学工坊静态状态
    case 147U:  // 聊天屏蔽
    case 148U:  // 聊天屏蔽
    case 520U:  // 喇叭
    case protocol::kLobbyMinigameRequestOpcode:  // 广场小游戏
    case 714U:                                   // 即时复活扣券
    case 952U:                                   // 场景名报告
    case 956U:                                   // 排行/资料查询
    case 1500U:                                  // 科学工坊动态配置
    case 2004U:
    case 2006U:
    case 2008U:  // 等级图标
    case 2009U:
    case 2016U:  // 键位
    case 2019U:
    case 2033U:
    case 65010U:
    case 65020U:  // 启动器认证
    case 65030U:  // 聊天屏蔽刷新
    case 65036U:
    case 65040U:  // 禁用软件命中上报
      return true;
    default:
      // 132..137 为工坊动作，917..920 为泡泡抽取，936..939 为资料查询。
      return (opcode >= 132U && opcode <= 137U) ||
             (opcode >= 917U && opcode <= 920U) ||
             (opcode >= 936U && opcode <= 939U);
  }
}

GatewayOpcodeRoute ClassifyGatewayOpcode(std::uint16_t opcode) noexcept {
  if (!IsWorldOpcode(opcode) || WorldOpcodeRequiresMenuSnapshot(opcode) ||
      WorldOpcodeRequiresAccessSnapshot(opcode) ||
      IsHandledBeforeWorldDispatch(opcode)) {
    return GatewayOpcodeRoute::kDatabaseTask;
  }
  return GatewayOpcodeRoute::kWorldDirect;
}

}  // namespace bf::infrastructure::network
