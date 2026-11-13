#include "features/battle/match_objectives.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
void MatchObjectives::AdvanceSurvivalTide(Time now,
                                          MatchObjectiveUpdate& result) {
  if (!survival_tide_due_ || now < *survival_tide_due_) return;
  const auto emit = [&result](bool start, std::uint32_t phase,
                              std::uint32_t duration) {
    protocol::PacketWriter writer;
    // 5E6D50 -> 5E3C00 -> 525D40: three full DWORDs, not a byte boolean.
    writer.WriteU32(start ? 1U : 0U);
    writer.WriteU32(phase);
    writer.WriteU32(duration);
    result.messages.push_back({.opcode = 756U, .body = writer.Release()});
  };
  // End the current timer before switching phases: 525D40(false) invokes
  // 5265E0 to capture the withdrawal/rise endpoints and plays the wave sound.
  if (survival_tide_phase_ != 0U) emit(false, survival_tide_phase_, 0U);
  survival_tide_phase_ = survival_tide_phase_ % 4U + 1U;
  // Server cadence policy: requested 60s wait; one 9s native siren interval,
  // 2s withdrawal and 7s rise. The packet layout/phase meanings are recovered
  // from the client; the original server's phase durations are not available.
  constexpr std::array<std::uint32_t, 4U> duration{60'000U, 9'000U, 2'000U,
                                                   7'000U};
  const auto milliseconds = duration[survival_tide_phase_ - 1U];
  emit(true, survival_tide_phase_, milliseconds);
  // A delayed world tick must not burst several whole waves into the client.
  survival_tide_due_ = now + std::chrono::milliseconds{milliseconds};
}
}  // namespace bf::runtime
