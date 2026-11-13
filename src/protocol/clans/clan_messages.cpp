#include "protocol/clans/clan_messages.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

#include "protocol/codec/text_codec.h"

namespace bf::protocol {
namespace {

constexpr std::string_view kCryptTableHex =
    "8a0c85f3cc825fc19256576aaa03f0ba70e7877abb38849e4c189978504dc55a0f955bf25a9934ea491ac48964b5d3"
    "d73972ea1818ed60065351fdac567a20d6657263d2db12359cc01052a9f73d9057fba8894c550a684342928f8bcb09"
    "413e2dde87764c930cf8371112b1d3eccc6eba213fa2722fd9c07c4f9263e28522cf4ccc178f30380830362bfb5bea"
    "8d50663ca4cc7a7570967b29af3259cf71dadcebdf44290b119d8a06206bb3b45b06190d345605e2d8a105378e7ae4"
    "4b45a9caf870249f671f61e2b4e4a09d2a067ef97b02654bc9e2a401d37bdcd192e77016240bb3a3d2da8faa9524bd"
    "cdca32ec0b1a7364a53681edabf53aae40e53ab44b";

consteval std::uint8_t HexNibble(char value) {
  return value >= '0' && value <= '9'
             ? static_cast<std::uint8_t>(value - '0')
             : static_cast<std::uint8_t>(value - 'a' + 10);
}

consteval std::array<std::uint8_t, 256U> MakeCryptTable() {
  static_assert(kCryptTableHex.size() == 512U);
  std::array<std::uint8_t, 256U> result{};
  for (std::size_t index = 0U; index < result.size(); ++index) {
    result[index] = static_cast<std::uint8_t>(
        (HexNibble(kCryptTableHex[index * 2U]) << 4U) |
        HexNibble(kCryptTableHex[index * 2U + 1U]));
  }
  return result;
}

constexpr auto kCryptTable = MakeCryptTable();
constexpr auto kFarFuture = std::chrono::system_clock::time_point{
    std::chrono::seconds{4'102'444'800LL}};

std::chrono::system_clock::time_point DisbandDate(
    const application::ClanSummary& clan) {
  // Native7212E0 compares this OLE date with2100-01-01. A historical
  // request timestamp must not label an active/cancelled clan as dissolving.
  return clan.pending_disband
             ? clan.disband_requested_at.value_or(clan.created_at)
             : kFarFuture;
}

std::uint32_t LegacyId(std::uint64_t value) {
  if (value == 0U || value > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "clan identifier exceeds uint32"};
  }
  return static_cast<std::uint32_t>(value);
}

std::uint32_t OptionalLegacyId(const std::optional<domain::PlayerId>& value) {
  return value.has_value() ? LegacyId(value->Value()) : 0U;
}

void WriteSummaryProfile(ClanPacketWriter& writer,
                         const application::ClanSummary& clan,
                         std::span<const application::ClanMemberView> roster) {
  const auto deputy =
      std::ranges::find(roster, application::kClanDeputyRole,
                        &application::ClanMemberView::role_code);
  writer.WriteText(clan.name);
  writer.WriteU32(OptionalLegacyId(clan.leader_player_id));
  writer.WriteText(clan.leader_nickname);
  writer.WriteU32(deputy == roster.end() ? 0U
                                         : LegacyId(deputy->player_id.Value()));
  writer.WriteText(deputy == roster.end() ? std::string_view{}
                                          : deputy->nickname);
  writer.WriteU32(clan.level);
  writer.WriteU32(clan.level_points);
  writer.WriteDate(clan.created_at);
  writer.WriteDate(clan.created_at);
  writer.WriteU32(clan.recruitment_mode);
  writer.WriteU32(clan.member_count);
  writer.WriteU32(0U);
  writer.WriteText(clan.introduction);
  writer.WriteText(clan.notice);
  writer.WriteU32(clan.mark_code);
  writer.WriteU32(clan.capacity);
}

}  // namespace

void CryptClanPayload(std::uint16_t opcode,
                      std::span<std::uint8_t> body) noexcept {
  // 客户端表实际有 256 字节，但 0.2.80.1 索引明确对 255 取模，末字节不会使用。
  const auto key = kCryptTable[(body.size() + opcode) % 255U];
  for (std::size_t index = 0U; index < body.size(); ++index) {
    const auto divisor = static_cast<std::uint16_t>(index + 1U);
    const auto mask = static_cast<std::uint8_t>(key + (key % divisor));
    body[index] ^= mask;
  }
}

std::vector<std::uint8_t> EncodeClanFrame(const ClanPacket& packet) {
  if (packet.body.size() > kMaximumClanBodyBytes) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "clan frame body exceeds uint16 limit"};
  }
  PacketWriter writer;
  writer.WriteU16(static_cast<std::uint16_t>(packet.body.size()));
  writer.WriteU16(packet.opcode);
  auto encrypted = packet.body;
  CryptClanPayload(packet.opcode, encrypted);
  writer.WriteBytes(encrypted);
  return writer.Release();
}

ClanPacket DecodeClanFrame(std::span<const std::uint8_t> frame) {
  PacketReader reader{frame};
  const auto size = reader.ReadU16();
  const auto opcode = reader.ReadU16();
  if (size > kMaximumClanBodyBytes || reader.RemainingBytes() != size) {
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "clan frame length mismatch"};
  }
  const auto bytes = reader.ReadBytes(size);
  ClanPacket result{.opcode = opcode, .body = {bytes.begin(), bytes.end()}};
  CryptClanPayload(opcode, result.body);
  return result;
}

ClanPacketReader::ClanPacketReader(std::span<const std::uint8_t> body) noexcept
    : reader_(body) {}

std::uint8_t ClanPacketReader::ReadU8() { return reader_.ReadU8(); }

std::uint16_t ClanPacketReader::ReadU16() { return reader_.ReadU16(); }

std::uint32_t ClanPacketReader::ReadU32() { return reader_.ReadU32(); }

std::int32_t ClanPacketReader::ReadI32() { return reader_.ReadI32(); }

std::chrono::sys_days ClanPacketReader::ReadMonth() {
  const double value = reader_.ReadF64();
  if (!std::isfinite(value) || value < 36526.0 || value > 73416.0 ||
      std::floor(value) != value) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "clan month is not a valid OLE day"};
  }
  const std::chrono::sys_days day{
      std::chrono::days{static_cast<int>(value) - 25569}};
  const std::chrono::year_month_day calendar{day};
  if (calendar.day() != std::chrono::day{1} ||
      calendar.year() > std::chrono::year{2100}) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "clan query requires a month start"};
  }
  return day;
}

std::span<const std::uint8_t> ClanPacketReader::ReadBytes(std::size_t count) {
  return reader_.ReadBytes(count);
}

std::string ClanPacketReader::ReadText(std::size_t maximum_code_units) {
  std::u16string text;
  text.reserve(std::min<std::size_t>(maximum_code_units, 64U));
  for (std::size_t index = 0U; index <= maximum_code_units; ++index) {
    const auto code_unit = static_cast<char16_t>(reader_.ReadU16());
    if (code_unit == u'\0') {
      return Utf16ToUtf8(text);
    }
    if (index == maximum_code_units) {
      throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                          "clan UTF-16 text exceeds limit"};
    }
    text.push_back(code_unit);
  }
  throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                      "clan UTF-16 text is unterminated"};
}

void ClanPacketReader::RequireEnd() const { reader_.RequireEnd(); }

void ClanPacketWriter::WriteU8(std::uint8_t value) { writer_.WriteU8(value); }

void ClanPacketWriter::WriteU16(std::uint16_t value) {
  writer_.WriteU16(value);
}

void ClanPacketWriter::WriteU32(std::uint32_t value) {
  writer_.WriteU32(value);
}

void ClanPacketWriter::WriteI32(std::int32_t value) { writer_.WriteI32(value); }

void ClanPacketWriter::WriteU64(std::uint64_t value) {
  writer_.WriteU64(value);
}

void ClanPacketWriter::WriteBytes(std::span<const std::uint8_t> value) {
  writer_.WriteBytes(value);
}

void ClanPacketWriter::WriteText(std::string_view utf8) {
  const auto text = Utf8ToUtf16(utf8);
  for (const auto code_unit : text) {
    writer_.WriteU16(static_cast<std::uint16_t>(code_unit));
  }
  writer_.WriteU16(0U);
}

void ClanPacketWriter::WriteDate(std::chrono::system_clock::time_point value) {
  const auto seconds =
      std::chrono::duration<double>{value.time_since_epoch()}.count();
  writer_.WriteF64(seconds / 86'400.0 + 25'569.0);
}

std::vector<std::uint8_t> ClanPacketWriter::Release() noexcept {
  return writer_.Release();
}

std::vector<std::uint8_t> EncodeGatewayClanTicket(
    domain::PlayerId player_id, std::string_view nickname,
    std::span<const std::uint8_t> secret) {
  if (!player_id.IsValid() || secret.empty() || secret.size() > 128U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "gateway clan ticket value is invalid"};
  }
  PacketWriter writer;
  writer.WriteU32(LegacyId(player_id.Value()));
  writer.WriteUtf16(Utf8ToUtf16(nickname));
  writer.WriteU32(1U);
  writer.WriteU32(1U);
  writer.WriteU32(static_cast<std::uint32_t>(secret.size()));
  writer.WriteBytes(secret);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeClanHome(
    const application::ClanHomeState& state) {
  ClanPacketWriter writer;
  const bool member = state.clan.has_value();
  writer.WriteU8(member ? 1U : 0U);
  writer.WriteU32(member ? LegacyId(state.clan->clan_id)
                         : (state.pending_clan_id.has_value()
                                ? LegacyId(*state.pending_clan_id)
                                : 0U));
  writer.WriteU32(member ? state.role_code.value_or(0U)
                         : (state.pending_clan_id.has_value()
                                ? application::kClanApplicantRole
                                : 0U));
  writer.WriteText(member ? state.clan->name : state.pending_clan_name);
  writer.WriteDate(member ? DisbandDate(*state.clan) : kFarFuture);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeClanProfile(
    const std::optional<application::ClanSummary>& clan,
    std::span<const application::ClanMemberView> roster) {
  ClanPacketWriter writer;
  if (clan.has_value()) {
    WriteSummaryProfile(writer, *clan, roster);
  } else {
    writer.WriteText("");
    writer.WriteU32(0U);
    writer.WriteText("");
    writer.WriteU32(0U);
    writer.WriteText("");
    writer.WriteU32(0U);
    writer.WriteU32(0U);
    writer.WriteU64(0U);
    writer.WriteU64(0U);
    writer.WriteU32(0U);
    writer.WriteU32(0U);
    writer.WriteU32(0U);
    writer.WriteText("");
    writer.WriteText("");
    writer.WriteU32(0U);
    writer.WriteU32(0U);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeClanRoster(
    std::span<const ClanRosterWireEntry> entries, bool extended) {
  ClanPacketWriter writer;
  if (extended) {
    writer.WriteU32(1U);
  }
  writer.WriteU32(static_cast<std::uint32_t>(entries.size()));
  for (const auto& entry : entries) {
    writer.WriteU32(LegacyId(entry.member.player_id.Value()));
    writer.WriteText(entry.member.nickname);
    if (!extended) {
      writer.WriteU32(entry.scene);
      writer.WriteU32(entry.member.level);
      writer.WriteU32(entry.member.role_code);
      continue;
    }
    writer.WriteU32(entry.member.level);
    writer.WriteU32(entry.member.role_code);
    writer.WriteU32(entry.scene);
    writer.WriteU32(0U);
    writer.WriteDate(entry.member.joined_at);
    writer.WriteDate(entry.member.joined_at);
    writer.WriteDate(entry.member.joined_at);
    writer.WriteU32(0U);
    writer.WriteU32(0U);
    writer.WriteU32(0U);
    writer.WriteU32(0U);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeClanSearch(
    std::span<const application::ClanSummary> entries,
    std::uint32_t total_count, std::uint32_t page, std::string_view query) {
  ClanPacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(entries.size()));
  writer.WriteU32(total_count);
  writer.WriteU32(page);
  writer.WriteText(query);
  for (const auto& clan : entries) {
    writer.WriteU32(LegacyId(clan.clan_id));
    writer.WriteText(clan.name);
    writer.WriteU32(OptionalLegacyId(clan.leader_player_id));
    writer.WriteText(clan.leader_nickname);
    writer.WriteU32(clan.level);
    writer.WriteU32(clan.level_points);
    writer.WriteDate(clan.created_at);
    writer.WriteDate(clan.created_at);
    writer.WriteU32(!clan.pending_disband && clan.member_count < clan.capacity
                        ? clan.recruitment_mode
                        : 0U);
    writer.WriteU32(clan.member_count);
    writer.WriteU32(0U);
    writer.WriteText(clan.introduction);
    writer.WriteDate(DisbandDate(clan));
    writer.WriteDate(clan.created_at);
    writer.WriteU32(clan.mark_code);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeClanApplicants(
    std::span<const application::ClanApplicantView> entries) {
  ClanPacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(entries.size()));
  for (const auto& entry : entries) {
    writer.WriteU32(LegacyId(entry.player_id.Value()));
    writer.WriteText(entry.nickname);
    writer.WriteU32(entry.level);
    writer.WriteU32(application::kClanApplicantRole);
    writer.WriteDate(entry.created_at);
    writer.WriteU32(0U);
    writer.WriteU32(0U);
    writer.WriteU32(0U);
    writer.WriteU32(0U);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeClanPosts(
    std::span<const application::ClanPostView> entries) {
  ClanPacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(entries.size()));
  writer.WriteU32(entries.empty() ? 0U : LegacyId(entries.front().post_id));
  writer.WriteU32(entries.empty() ? 0U : LegacyId(entries.back().post_id));
  for (const auto& entry : entries) {
    writer.WriteU32(LegacyId(entry.author_player_id.Value()));
    writer.WriteText(entry.author_nickname);
    writer.WriteU32(entry.author_level);
    writer.WriteU32(LegacyId(entry.post_id));
    writer.WriteU16(1U);
    writer.WriteDate(entry.created_at);
    writer.WriteText(entry.body);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeClanRecords(
    std::span<const application::ClanRecordEntry> entries, bool opponents) {
  ClanPacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(entries.size()));
  for (const auto& entry : entries) {
    writer.WriteU32(entry.id);
    if (opponents) writer.WriteU32(entry.display_code);
    writer.WriteText(entry.name);
    if (!opponents) writer.WriteU32(entry.display_code);
    for (const auto& counts : {entry.monthly, entry.lifetime}) {
      writer.WriteU32(counts.wins);
      writer.WriteU32(counts.draws);
      writer.WriteU32(counts.losses);
    }
  }
  auto body = writer.Release();
  if (body.size() > kMaximumClanBodyBytes) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "clan records exceed native frame"};
  }
  return body;
}

std::vector<std::uint8_t> EncodeClanRecentMatches(
    std::span<const application::ClanRecentMatch> entries) {
  ClanPacketWriter writer;
  writer.WriteU32(0U);  // 729F90 consumes this field without using it.
  writer.WriteU32(
      0U);  // Starting row; this response contains one complete batch.
  writer.WriteU32(static_cast<std::uint32_t>(entries.size()));
  for (const auto& entry : entries) {
    writer.WriteU32(entry.opponent_id);
    writer.WriteU32(0U);  // Second per-record DWORD is skipped by 729F90.
    writer.WriteU32(entry.opponent_mark);
    writer.WriteText(entry.opponent_name);
    writer.WriteU32(entry.result.wins);
    writer.WriteU32(entry.result.draws);
    writer.WriteU32(entry.result.losses);
    for (const auto& name : entry.participant_names) writer.WriteText(name);
    writer.WriteU32(
        0U);  // Stored at model+332, unused by this build's list/detail views.
    writer.WriteDate(entry.ended_at + std::chrono::hours{8});
  }
  auto body = writer.Release();
  if (body.size() > kMaximumClanBodyBytes)
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "recent clan matches exceed native frame"};
  return body;
}

std::vector<std::uint8_t> EncodeClanActivity(
    const application::ClanActivityView& value) {
  ClanPacketWriter writer;
  writer.WriteU32(1U);
  writer.WriteDate(value.period_start + std::chrono::hours{8});
  writer.WriteDate(value.period_end + std::chrono::hours{8});
  writer.WriteU32(value.weekly_points);
  writer.WriteU32(
      0U);  // model+740 is stored but not displayed by 74F7D0/74FC00.
  writer.WriteU32(value.personal_points);
  writer.WriteU32(
      0U);  // model+748 is also unused by this build's activity form.
  return writer.Release();
}

std::vector<std::uint8_t> EncodeClanRankings(
    std::span<const application::ClanRankingEntry> entries, bool single) {
  if ((single && entries.size() != 1U) || (!single && entries.size() > 100U))
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "native clan ranking entry count invalid"};
  ClanPacketWriter writer;
  writer.WriteU32(single ? 0U : static_cast<std::uint32_t>(entries.size()));
  for (const auto& entry : entries) {
    writer.WriteU32(entry.clan_id);
    writer.WriteText(entry.name);
    writer.WriteU32(entry.rank);
    writer.WriteU32(entry.level);
    writer.WriteU32(entry.results.wins);
    writer.WriteU32(entry.results.draws);
    writer.WriteU32(entry.results.losses);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeClanPointsNotice(std::uint32_t clan_id,
                                                 std::uint32_t points,
                                                 std::uint32_t reason,
                                                 bool event_icon) {
  if (clan_id == 0U ||
      points > static_cast<std::uint32_t>(
                   std::numeric_limits<std::int32_t>::max()) ||
      (reason != 1U && reason != 2U && reason != 3U && reason != 11U &&
       reason != 12U && reason != 13U))
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid clan points notice"};
  PacketWriter writer;
  writer.WriteU32(clan_id);  // B62680 does not use the first DWORD; it is not
                             // an opcode or list count.
  writer.WriteU32(points);
  writer.WriteU32(reason);
  writer.WriteU32(
      event_icon
          ? 1U
          : 0U);  // 74F7D0 controls pnlClanEventIcon, not level-up state.
  return writer.Release();
}

}  // namespace bf::protocol
