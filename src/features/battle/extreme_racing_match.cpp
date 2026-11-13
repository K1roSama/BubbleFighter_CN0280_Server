#include "features/battle/extreme_racing_match.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>

#include "features/battle/match_result.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
using Timer = protocol::ExtremeRacingTimer;
using Kind = protocol::ExtremeRacingUpdateKind;
using Field = protocol::ExtremeRacingField;
using Reject = ExtremeRacingReject;
using Phase = ExtremeRacingPhase;
constexpr auto kCheckpointDelay = std::chrono::milliseconds{2000};
constexpr auto kMaxInt = std::numeric_limits<std::int32_t>::max();

bool Bounded(std::chrono::milliseconds duration, std::int64_t minimum,
             std::int64_t maximum) {
  return duration.count() >= minimum && duration.count() <= maximum;
}

bool Racing(Phase phase) {
  return phase == Phase::kRunning || phase == Phase::kRetiring;
}
}  // namespace

ExtremeRacingMatch::ExtremeRacingMatch(
    std::string_view map, std::vector<ExtremeRacingCoinPlacement> coins,
    ExtremeRacingRules rules)
    : map_(FindExtremeRacingMap(map)), rules_(std::move(rules)) {
  if (!map_ || !Bounded(rules_.countdown, 1000, 60000) ||
      !Bounded(rules_.stage_duration, 2000, 3600000) ||
      !Bounded(rules_.retire_duration, 1, 60000) ||
      !Bounded(rules_.result_duration, 1, 60000) ||
      !Bounded(rules_.position_max_age, 1, 10000) ||
      !Bounded(rules_.cannon_authorization, 1, 60000) ||
      !std::isfinite(rules_.cannon_tolerance) ||
      rules_.cannon_tolerance < 0.0F || rules_.cannon_tolerance > 100.0F ||
      !std::isfinite(rules_.region_tolerance) ||
      rules_.region_tolerance < 0.0F || rules_.region_tolerance > 100.0F ||
      !std::isfinite(rules_.coin_radius) || rules_.coin_radius <= 0.0F ||
      rules_.coin_radius > 100.0F || coins.size() > 1024U ||
      !std::ranges::is_sorted(rules_.finish_points, std::greater{}) ||
      rules_.finish_points.back() <= 0) {
    throw std::invalid_argument{"invalid server extreme racing rules or map"};
  }
  std::int64_t maximum_score = 5LL * rules_.finish_points.front();
  for (const auto& coin : coins) {
    if (coin.object == 0U ||
        coin.object > static_cast<std::uint32_t>(kMaxInt) || coin.stage < 1U ||
        coin.stage > 5U || coin.points <= 0 ||
        !std::ranges::all_of(
            coin.position, [](float value) { return std::isfinite(value); }) ||
        !coins_.emplace(coin.object, coin).second)
      throw std::invalid_argument{
          "invalid server extreme racing coin placement"};
    maximum_score += coin.points;
  }
  if (maximum_score > kMaxInt)
    throw std::invalid_argument{
        "extreme racing score would overflow native signed sum"};
}

ExtremeRacingMatch::ExtremeRacingMatch(const ExtremeRacingScene& scene,
                                       ExtremeRacingRules rules)
    : ExtremeRacingMatch(scene.Map(), scene.Coins(), std::move(rules)) {
  scene_ = scene;
}

void ExtremeRacingMatch::AppendTimer(ExtremeRacingUpdate& update, Timer kind,
                                     std::chrono::milliseconds duration,
                                     bool active) const {
  update.messages.push_back(
      {.opcode = 761U,
       .body = protocol::EncodeExtremeRacingTimer(
           kind, static_cast<std::uint32_t>(duration.count()), active)});
}

void ExtremeRacingMatch::Records(ExtremeRacingUpdate& update, Kind kind) const {
  std::vector<protocol::ExtremeRacingRecordUpdate> rows;
  for (const auto& [id, player] : players_) {
    static_cast<void>(id);
    const auto& score = player.score;
    rows.push_back(
        {.seat = score.seat,
         .fields = {
             {Field::kTotalPrimary, score.finish_points},
             {Field::kRoundPrimary, score.round_finish_points},
             {Field::kTotalSecondary, score.coin_points},
             {Field::kRoundSecondary, score.round_coin_points},
             {Field::kInactive, score.departed ? 1 : 0},
             {Field::kIndicator, std::min<std::int32_t>(score.round_place, 4)},
             {Field::kTieBreak, score.tie_break}}});
  }
  update.messages.push_back(
      {.opcode = 764U,
       .body = protocol::EncodeExtremeRacingRecords(kind, rows)});
}

void ExtremeRacingMatch::Countdown(Time now, ExtremeRacingUpdate& update) {
  phase_ = Phase::kCountdown;
  deadline_ = now + rules_.countdown;
  ++stage_;
  if (stage_ > 1U)
    combat_->ResetRound();  // Preserve match performance; revoke all
                            // previous-stage projectiles.
  finishers_ = 0U;
  for (auto& [id, player] : players_) {
    static_cast<void>(id);
    player.score.round_finish_points = 0;
    player.score.round_coin_points = 0;
    player.score.round_place = 0U;
    player.score.checkpoint.reset();
    player.position.reset();
    player.round_settled = player.score.departed;
    const auto actor = combat_->actors_.find(id);
    if (actor != combat_->actors_.end())
      actor->second.racing_stage = CombatRelay::ActorState::RacingStage{
          .stage = stage_,
          .spawn_point = player.spawn_point,
          .spawn_position =
              FindExtremeRacingMarker(
                  map_->key, "#spawn_T" + std::to_string(stage_ * 100U +
                                                         player.spawn_point))
                  ->position,
          .needs_reset = stage_ > 1U};
  }
  update.stage_changed = stage_;
  if (scene_)
    update.messages.push_back(
        {.opcode = 762U,
         .body = protocol::EncodeExtremeRacingStage(0, stage_)});
  AppendTimer(update, Timer::kCountdown, rules_.countdown, true);
  Records(update, Kind::kLocal);
  Records(update, Kind::kStandings);
}

ExtremeRacingUpdate ExtremeRacingMatch::Start(CombatRelay& combat, Time now) {
  if (phase_ != Phase::kWaiting) return {};
  if (combat.mode_id_ != 24U || combat.actors_.size() < 2U ||
      combat.actors_.size() > 16U)
    throw std::invalid_argument{
        "extreme racing core requires 2-16 human participants in mode 24"};
  std::map<domain::PlayerId, Player> frozen;
  std::set<std::uint16_t> seats;
  for (const auto& [id, actor] : combat.actors_) {
    const auto& member = actor.participant;
    if (actor.controller || actor.racing_stage ||
        !member.ordinary_respawn_allowed || member.seat_index >= 36U ||
        !seats.insert(member.seat_index).second ||
        coins_.contains(member.actor_object_id) ||
        (scene_ && scene_->OwnsObject(member.actor_object_id)))
      throw std::invalid_argument{
          "invalid extreme racing roster or object namespace collision"};
    frozen.emplace(id, Player{.object = member.actor_object_id,
                              .score = {.seat = member.seat_index}});
  }
  players_ = std::move(frozen);
  // 7BFF70 enumerates native seats, counts each team independently and wraps
  // mod 6.
  std::vector<domain::PlayerId> native_order;
  for (const auto& [id, player] : players_) {
    static_cast<void>(player);
    native_order.push_back(id);
  }
  std::ranges::sort(native_order, {}, [&](domain::PlayerId id) {
    return players_.at(id).score.seat;
  });
  std::array<std::uint32_t, 2U> team_counts{};
  for (const auto id : native_order) {
    const auto team = combat.actors_.at(id).participant.team_code;
    players_.at(id).spawn_point =
        team * 10U + (team_counts[team - 1U]++ % 6U) + 1U;
    for (std::uint32_t stage = 1U; stage <= 5U; ++stage)
      if (!FindExtremeRacingMarker(
              map_->key,
              "#spawn_T" +
                  std::to_string(stage * 100U + players_.at(id).spawn_point)))
        throw std::invalid_argument{
            "extreme racing native stage spawn marker missing"};
  }
  combat_ = &combat;
  last_time_ = now;
  ExtremeRacingUpdate update;
  if (scene_) {
    update.messages.push_back(
        {.opcode = 763U,
         .body = protocol::EncodeExtremeRacingInitialization(
             scene_->Initialization())});
    update.messages.push_back(
        {.opcode = 768U,
         .body = protocol::EncodeExtremeRacingHazards(scene_->Hazards())});
  }
  Countdown(now, update);
  return update;
}

void ExtremeRacingMatch::Observe(domain::PlayerId sender,
                                 const protocol::Packet& packet,
                                 const CombatRelay& combat, Time now) {
  if (&combat != combat_)
    throw std::invalid_argument{"extreme racing combat binding differs"};
  const auto player = players_.find(sender);
  if (!Racing(phase_) || player == players_.end() ||
      player->second.score.departed || player->second.round_settled ||
      now < *last_time_ || now < running_since_ || now >= *deadline_ ||
      !protocol::IsNativeActionOpcode(packet.opcode))
    return;
  const auto request = protocol::DecodeNativeAction(packet.opcode, packet.body);
  const auto actor = combat.actors_.find(sender);
  if (!request.position || request.actor_object_id != player->second.object ||
      actor == combat.actors_.end() ||
      actor->second.participant.actor_object_id != player->second.object ||
      actor->second.life_state != CombatLifeState::kAlive ||
      !actor->second.racing_stage ||
      actor->second.racing_stage->stage != stage_ ||
      actor->second.racing_stage->needs_reset ||
      actor->second.position != request.position ||
      (player->second.position && now < player->second.position->received))
    return;
  player->second.position =
      Position{*request.position, now, actor->second.life_generation};
}

std::optional<Reject> ExtremeRacingMatch::ValidPosition(
    const Player& player, domain::PlayerId sender, const CombatRelay& combat,
    Time now) const {
  const auto actor = combat.actors_.find(sender);
  if (actor == combat.actors_.end() ||
      actor->second.participant.actor_object_id != player.object ||
      actor->second.life_state != CombatLifeState::kAlive)
    return Reject::kInvalidLife;
  if (!actor->second.racing_stage ||
      actor->second.racing_stage->stage != stage_ ||
      actor->second.racing_stage->needs_reset)
    return Reject::kInvalidLife;
  if (!player.position ||
      player.position->life != actor->second.life_generation ||
      player.position->received < running_since_ ||
      now < player.position->received ||
      now - player.position->received > rules_.position_max_age ||
      actor->second.position != std::optional{player.position->value})
    return Reject::kMissingFreshPosition;
  return std::nullopt;
}

bool ExtremeRacingMatch::AllFinished() const {
  return std::ranges::all_of(players_, [](const auto& entry) {
    return entry.second.score.departed || entry.second.round_settled;
  });
}

void ExtremeRacingMatch::Complete(ExtremeRacingCompletion kind,
                                  ExtremeRacingUpdate& update) {
  phase_ = Phase::kFinished;
  next_fire_.clear();
  pending_shots_.clear();
  deadline_.reset();
  update.completion = kind;
  SynchronizeCombat();
}

void ExtremeRacingMatch::SynchronizeCombat() {
  if (!combat_) return;
  for (const auto& [id, player] : players_) {
    const auto actor = combat_->actors_.find(id);
    if (actor == combat_->actors_.end() || !actor->second.racing_stage)
      continue;
    auto& state = *actor->second.racing_stage;
    if (Racing(phase_) && !player.score.departed && !player.round_settled) {
      state.begins = running_since_;
      state.expires = deadline_;
    } else {
      state.begins.reset();
      state.expires.reset();
      actor->second.respawn_point.reset();
    }
  }
}

void ExtremeRacingMatch::ArmCannons(Time now) {
  next_fire_.clear();
  pending_shots_.clear();
  if (!scene_) return;
  for (const auto& cannon : scene_->Cannons())
    if (cannon.stage == stage_ && cannon.interval.count() > 0 &&
        !cannon.weapons.empty())
      next_fire_[cannon.object] = now + cannon.interval;
}

void ExtremeRacingMatch::FireCannons(Time now, ExtremeRacingUpdate& update) {
  std::erase_if(pending_shots_,
                [now](const PendingShot& shot) { return now >= shot.expires; });
  for (auto& [object, due] : next_fire_) {
    if (now < due) continue;
    const auto cannon = std::ranges::find(
        scene_->Cannons(), object, &ExtremeRacingCannonPlacement::object);
    for (const auto& weapon : cannon->weapons) {
      for (std::uint32_t shot = 0U; shot < weapon.count; ++shot) {
        update.host_messages.push_back(
            {.opcode = 770U,
             .body = protocol::EncodeExtremeRacingCannonFire(
                 {.group = cannon->group,
                  .cannon_object = object,
                  .weapon = weapon.weapon,
                  .speed_multiplier = weapon.speed_multiplier})});
        if (pending_shots_.size() < 512U)
          pending_shots_.push_back({object, weapon.weapon,
                                    combat_->host_.value_or(domain::PlayerId{}),
                                    now, now + rules_.cannon_authorization});
      }
    }
    // A late scheduler fires once and re-anchors instead of replaying missed
    // volleys.
    due = now - due >= cannon->interval ? now + cannon->interval
                                        : due + cannon->interval;
  }
}

CombatRelayResult ExtremeRacingMatch::CannonProjectile(
    domain::PlayerId sender, const protocol::Packet& packet,
    CombatRelay& combat, Time now) {
  const auto shot = protocol::DecodeExtremeRacingCannonProjectile(packet.body);
  if (combat_ && &combat != combat_)
    throw std::invalid_argument{"extreme racing combat binding differs"};
  const auto actor = combat.actors_.find(sender);
  if (actor == combat.actors_.end() || actor->second.controller)
    return {.reject_reason = CombatRejectReason::kUnknownRelatedActor};
  if (shot.actor_object != actor->second.participant.actor_object_id)
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kInvalidValue,
        "cannon projectile impersonates another actor"};
  if (!scene_ || !Racing(phase_) || combat.host_ != sender ||
      (last_time_ && now < *last_time_) || now < running_since_ || !deadline_ ||
      now >= *deadline_)
    return {.reject_reason = CombatRejectReason::kInactiveMatchPhase};
  const auto cannon = std::ranges::find(scene_->Cannons(), shot.cannon_object,
                                        &ExtremeRacingCannonPlacement::object);
  if (cannon == scene_->Cannons().end() || cannon->stage != stage_)
    return {.reject_reason = CombatRejectReason::kUnknownTarget};
  double distance = 0.0;
  double alignment = 0.0;
  double direction_norm = 0.0;
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    const double delta =
        static_cast<double>(shot.position[axis]) - cannon->position[axis];
    distance += delta * delta;
    alignment +=
        static_cast<double>(shot.direction[axis]) * cannon->direction[axis];
    direction_norm +=
        static_cast<double>(shot.direction[axis]) * shot.direction[axis];
  }
  if (distance > static_cast<double>(rules_.cannon_tolerance) *
                     rules_.cannon_tolerance ||
      alignment < 0.98 || std::abs(direction_norm - 1.0) > 0.02)
    return {.reject_reason = CombatRejectReason::kUnrecoveredAction};
  last_time_ = now;
  std::erase_if(pending_shots_, [now](const PendingShot& pending) {
    return now >= pending.expires;
  });
  const auto pending =
      std::ranges::find_if(pending_shots_, [&](const PendingShot& entry) {
        return entry.cannon == shot.cannon_object &&
               entry.weapon == shot.weapon && entry.controller == sender &&
               now >= entry.issued;
      });
  if (pending == pending_shots_.end())
    return {.reject_reason = CombatRejectReason::kDuplicateAction};
  combat.PruneProjectiles(now);
  if (combat.ContainsDamagerCookie(shot.sequence) ||
      combat.projectiles_.size() >= 8192U)
    return {.reject_reason = CombatRejectReason::kDuplicateProjectile};
  pending_shots_.erase(pending);
  auto& projectile =
      combat.projectiles_.try_emplace(shot.sequence).first->second;
  projectile.owner_object_id = shot.actor_object;
  projectile.created_at = now;
  projectile.weapon_bar = 0U;
  return combat.AcceptedResult(sender, packet);
}

CombatRelayResult ExtremeRacingMatch::StoneBlast(domain::PlayerId sender,
                                                 const protocol::Packet& packet,
                                                 CombatRelay& combat,
                                                 Time now) {
  if (packet.opcode != 32772U)
    throw std::invalid_argument{"not a native racing stone blast"};
  const auto request =
      protocol::DecodeNativeWeaponObject(packet.opcode, packet.body);
  if (combat_ && &combat != combat_)
    throw std::invalid_argument{"extreme racing combat binding differs"};
  const auto actor = combat.actors_.find(sender);
  const auto player = players_.find(sender);
  if (actor == combat.actors_.end() || actor->second.controller ||
      player == players_.end())
    return {.reject_reason = CombatRejectReason::kUnknownRelatedActor};
  if (request.actor_object_id != actor->second.participant.actor_object_id)
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kInvalidValue,
        "racing stone blast impersonates another actor"};
  if (!scene_ || scene_->Map() != "rel_racing02_extreme" ||
      scene_->Disposers().size() != 18U || combat.mode_id_ != 24U ||
      stage_ != 3U || !Racing(phase_) || player->second.score.departed ||
      player->second.round_settled || (last_time_ && now < *last_time_) ||
      now < running_since_ || !deadline_ || now >= *deadline_)
    return {.reject_reason = CombatRejectReason::kInactiveMatchPhase};
  if ((request.template_id != 60001U && request.template_id != 60002U) ||
      request.attack_variant != actor->second.participant.team_code ||
      ValidPosition(player->second, sender, combat, now))
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  // 630330 -> 7C8180/7EE3F0: local-player position, zero direction, fixed
  // (0,1,0). No stone cookie is sent. This proves a native local collision
  // report, not server physics.
  protocol::PacketReader vectors{std::span{packet.body}.subspan(7U, 36U)};
  double distance = 0.0;
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    const double delta =
        vectors.ReadF32() - player->second.position->value[axis];
    distance += delta * delta;
  }
  if (distance >
      static_cast<double>(rules_.region_tolerance) * rules_.region_tolerance)
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  for (unsigned index = 0U; index < 6U; ++index)
    if (vectors.ReadF32() != (index == 4U ? 1.0F : 0.0F))
      return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  combat.PruneProjectiles(now);
  if (combat.ContainsDamagerCookie(request.cookie))
    return {.reject_reason = CombatRejectReason::kDuplicateProjectile};
  if (combat.weapon_objects_.size() + combat.storms_.size() >= 8192U)
    return {.reject_reason = CombatRejectReason::kProjectileLimit};
  if (!combat.ConsumeActionQuota(actor->second, now))
    return {.reject_reason = CombatRejectReason::kRateLimited};
  last_time_ = now;
  combat.weapon_objects_.emplace(
      request.cookie,
      CombatRelay::ProjectileState{.owner_object_id = request.actor_object_id,
                                   .created_at = now,
                                   .self_only = true,
                                   .self_life = actor->second.life_generation});
  // Only the reporter receives this environmental damager. Peers simulate their
  // own stones; do not broadcast a generic blast that could damage an unrelated
  // client's local player.
  return {.accepted = true,
          .deliveries = {
              {.recipient = sender,
               .message = {.opcode = packet.opcode, .body = packet.body}}}};
}

void ExtremeRacingMatch::CloseRound(Time now, ExtremeRacingUpdate& update) {
  next_fire_.clear();
  pending_shots_.clear();
  // Local policy: an unfinished round receives no finishing points and the
  // largest possible round-time penalty. Earned coins remain part of the score.
  const auto penalty = static_cast<std::int32_t>(
      (rules_.stage_duration + rules_.retire_duration).count());
  for (auto& [id, player] : players_) {
    static_cast<void>(id);
    if (!player.round_settled) {
      player.score.tie_break += penalty;
      player.round_settled = true;
    }
  }
  AppendTimer(update, Timer::kStage, rules_.stage_duration, false);
  AppendTimer(update, Timer::kRetire, rules_.retire_duration, false);
  Records(update, Kind::kLocal);
  Records(update, Kind::kRoundResult);
  update.score_changed = true;
  if (stage_ == 5U) {
    Complete(ExtremeRacingCompletion::kFiveStages, update);
  } else {
    phase_ = Phase::kRoundResult;
    deadline_ = now + rules_.result_duration;
  }
  SynchronizeCombat();
}

ExtremeRacingUpdate ExtremeRacingMatch::Tick(Time now) {
  ExtremeRacingUpdate update;
  if (last_time_ && now < *last_time_) {
    update.rejection = Reject::kClockRegression;
    return update;
  }
  if (phase_ == Phase::kWaiting || phase_ == Phase::kFinished) return update;
  last_time_ = now;
  if (Racing(phase_) && now < *deadline_) FireCannons(now, update);
  if (now < *deadline_) return update;
  if (phase_ == Phase::kCountdown) {
    phase_ = Phase::kRunning;
    ArmCannons(now);
    // Timers begin when the corresponding native activation is emitted. A late
    // scheduler cannot retroactively consume the next playable round.
    running_since_ = now;
    deadline_ = now + rules_.stage_duration;
    AppendTimer(update, Timer::kCountdown, rules_.countdown, false);
    AppendTimer(update, Timer::kStage, rules_.stage_duration, true);
  } else if (phase_ == Phase::kRoundResult) {
    Countdown(now, update);
  } else {
    CloseRound(now, update);
  }
  SynchronizeCombat();
  return update;
}

ExtremeRacingUpdate ExtremeRacingMatch::Request(domain::PlayerId sender,
                                                const protocol::Packet& packet,
                                                const CombatRelay& combat,
                                                Time now) {
  if (combat_ && &combat != combat_)
    throw std::invalid_argument{"extreme racing combat binding differs"};
  std::optional<protocol::ExtremeRacingCheckpoint> checkpoint;
  std::optional<std::uint32_t> finish;
  std::optional<std::uint32_t> object;
  if (packet.opcode == 754U)
    finish = protocol::DecodeExtremeRacingFinish(packet.body);
  else if (packet.opcode == 755U)
    object = protocol::DecodeExtremeRacingCoin(packet.body);
  else if (packet.opcode == 32934U)
    checkpoint = protocol::DecodeExtremeRacingCheckpoint(packet.body);
  else
    throw std::invalid_argument{"not an extreme racing request"};
  const auto found = players_.find(sender);
  if (checkpoint && found != players_.end() &&
      checkpoint->actor_object != found->second.object)
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kInvalidValue,
        "extreme racing checkpoint impersonates another actor"};
  auto update = Tick(now);
  const auto reject = [&](Reject reason) {
    update.rejection = reason;
    return update;
  };
  if (update.rejection) return update;
  if (found == players_.end() || found->second.score.departed)
    return reject(Reject::kUnknownParticipant);
  if (!Racing(phase_)) return reject(Reject::kInactivePhase);
  auto& player = found->second;
  if ((finish && *finish != stage_) ||
      (checkpoint && checkpoint->stage != stage_))
    return reject(Reject::kWrongStage);
  if (player.round_settled) return reject(Reject::kDuplicate);
  const auto& stage = map_->stages[stage_ - 1U];
  if (checkpoint) {
    if (checkpoint->checkpoint >= stage.checkpoints.size())
      return reject(Reject::kUnknownCheckpoint);
    if (player.score.checkpoint &&
        checkpoint->checkpoint <= *player.score.checkpoint)
      return reject(Reject::kDuplicate);
    if (now - running_since_ < kCheckpointDelay)
      return reject(Reject::kCheckpointTooEarly);
  }
  if (object) {
    const auto coin = coins_.find(*object);
    if (coin == coins_.end()) return reject(Reject::kUnknownObject);
    if (coin->second.stage != stage_) return reject(Reject::kWrongStage);
    if (player.coins.contains(*object)) return reject(Reject::kDuplicate);
  }
  if (const auto invalid = ValidPosition(player, sender, combat, now))
    return reject(*invalid);
  const auto& position = player.position->value;
  if (checkpoint) {
    if (!stage.checkpoints[checkpoint->checkpoint].Contains(
            position, rules_.region_tolerance))
      return reject(Reject::kOutsideRegion);
    player.score.checkpoint = checkpoint->checkpoint;
    update.accepted = true;
    return update;  // C2S32934 is an authoritative save, not an invented reply.
  }
  if (object) {
    const auto& coin = coins_.at(*object);
    double distance_squared = 0.0;
    for (std::size_t axis = 0U; axis < position.size(); ++axis) {
      const auto distance =
          static_cast<double>(position[axis]) - coin.position[axis];
      distance_squared += distance * distance;
    }
    if (distance_squared >
        static_cast<double>(rules_.coin_radius) * rules_.coin_radius)
      return reject(Reject::kOutsideRegion);
    player.coins.insert(*object);
    player.score.coin_points += coin.points;
    player.score.round_coin_points += coin.points;
    update.accepted = true;
    update.score_changed = true;
    Records(update, Kind::kLocal);
    Records(update, Kind::kStandings);
    return update;
  }
  if (rules_.require_last_checkpoint &&
      player.score.checkpoint != std::optional{static_cast<std::uint32_t>(
                                     stage.checkpoints.size() - 1U)})
    return reject(Reject::kMissingCheckpoint);
  if (!stage.goal.Contains(position, rules_.region_tolerance))
    return reject(Reject::kOutsideRegion);
  const auto points = rules_.finish_points[finishers_++];
  player.score.finish_points += points;
  player.score.round_finish_points = points;
  player.score.round_place = finishers_;
  player.score.tie_break += static_cast<std::int32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                            running_since_)
          .count());
  player.round_settled = true;
  update.accepted = true;
  update.score_changed = true;
  if (AllFinished()) {
    CloseRound(now, update);
  } else {
    if (phase_ == Phase::kRunning) {
      phase_ = Phase::kRetiring;
      deadline_ = now + rules_.retire_duration;
      // Keep the stage timer alive. Native 521EE0 still requires its nonzero
      // duration while accepting checkpoints during the retirement grace
      // period.
      AppendTimer(update, Timer::kRetire, rules_.retire_duration, true);
    }
    Records(update, Kind::kLocal);
    Records(update, Kind::kStandings);
  }
  SynchronizeCombat();
  return update;
}

ExtremeRacingUpdate ExtremeRacingMatch::Leave(domain::PlayerId id, Time now) {
  if (last_time_ && now < *last_time_)
    return {.rejection = Reject::kClockRegression};
  const auto found = players_.find(id);
  if (found == players_.end() || found->second.score.departed ||
      phase_ == Phase::kFinished)
    return {};
  auto& player = found->second;
  player.score.departed = true;
  player.position.reset();
  ExtremeRacingUpdate update;
  if (std::ranges::all_of(players_, [](const auto& entry) {
        return entry.second.score.departed;
      })) {
    last_time_ = now;
    AppendTimer(update, Timer::kCountdown, rules_.countdown, false);
    AppendTimer(update, Timer::kStage, rules_.stage_duration, false);
    AppendTimer(update, Timer::kRetire, rules_.retire_duration, false);
    Records(update, Kind::kStandings);
    Complete(ExtremeRacingCompletion::kNoParticipants, update);
  } else {
    update = Tick(now);
    if (Racing(phase_) && AllFinished())
      CloseRound(now, update);
    else if (!update.completion)
      Records(update, Kind::kStandings);
  }
  update.score_changed = true;
  SynchronizeCombat();
  return update;
}

ExtremeRacingProgress ExtremeRacingMatch::Progress(
    domain::PlayerId player) const {
  return players_.at(player).score;
}

std::uint32_t ExtremeRacingMatch::SpawnPoint(domain::PlayerId player) const {
  return players_.at(player).spawn_point;
}

std::vector<domain::PlayerId> ExtremeRacingMatch::Standings() const {
  std::vector<domain::PlayerId> result;
  for (const auto& [id, player] : players_) {
    static_cast<void>(player);
    result.push_back(id);
  }
  std::ranges::sort(result, [&](auto left, auto right) {
    const auto& a = players_.at(left).score;
    const auto& b = players_.at(right).score;
    return domain::BetterExtremeRacingOrder(
        {a.departed, static_cast<std::int64_t>(a.finish_points) + a.coin_points,
         a.tie_break, a.seat},
        {b.departed, static_cast<std::int64_t>(b.finish_points) + b.coin_points,
         b.tie_break, b.seat});
  });
  return result;
}
}  // namespace bf::runtime
