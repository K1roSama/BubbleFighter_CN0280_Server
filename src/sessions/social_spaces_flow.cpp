#include "sessions/social_spaces_flow.h"

#include <algorithm>

#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"

namespace bf::runtime {
namespace {
using application::SocialSpaceAction;

std::vector<std::uint8_t> Encode(const application::SocialSpacesSnapshot& s,
                                 const SessionRegistry& sessions) {
  protocol::PacketWriter w;
  w.WriteU32(1U);
  w.WriteU32(static_cast<std::uint32_t>(s.player.Value()));
  w.WriteU32(static_cast<std::uint32_t>(s.categories.size()));
  for (const auto& c : s.categories) {
    w.WriteU32(c.id);
    w.WriteUtf16(protocol::Utf8ToUtf16(c.name));
    w.WriteU32(static_cast<std::uint32_t>(c.members.size()));
    for (auto p : c.members) w.WriteU32(static_cast<std::uint32_t>(p.Value()));
  }
  w.WriteU32(static_cast<std::uint32_t>(s.chats.size()));
  for (const auto& chat : s.chats) {
    w.WriteU32(chat.id);
    w.WriteU32(static_cast<std::uint32_t>(chat.members.size()));
    for (const auto& m : chat.members) {
      w.WriteU32(static_cast<std::uint32_t>(m.player.Value()));
      w.WriteUtf16(protocol::Utf8ToUtf16(m.nickname));
      w.WriteU32(sessions.Lookup(m.player).online ? 1U : 0U);
    }
  }
  w.WriteU32(static_cast<std::uint32_t>(s.invitations.size()));
  for (const auto& i : s.invitations) {
    w.WriteU32(i.chat);
    w.WriteU32(static_cast<std::uint32_t>(i.inviter.player.Value()));
    w.WriteUtf16(protocol::Utf8ToUtf16(i.inviter.nickname));
  }
  return w.Release();
}

std::vector<std::uint8_t> Encode(const application::GroupChatMessage& m) {
  protocol::PacketWriter w;
  w.WriteU32(1U);
  w.WriteU64(m.sequence);
  w.WriteU32(m.chat);
  w.WriteU32(static_cast<std::uint32_t>(m.sender.player.Value()));
  w.WriteUtf16(protocol::Utf8ToUtf16(m.sender.nickname));
  w.WriteUtf16(protocol::Utf8ToUtf16(m.text));
  return w.Release();
}
}  // namespace

std::optional<application::ServiceError> SocialSpacesFlow::Handle(
    domain::PlayerId actor, SessionHandle session,
    std::span<const std::uint8_t> body,
    std::chrono::steady_clock::time_point now) {
  protocol::PacketReader reader{body};
  if (reader.ReadU32() != 1U)
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "social spaces version"};
  const auto action = reader.ReadU32();
  if (action > 16U || (action > 5U && action < 10U))
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "social spaces action"};
  application::SocialSpaceCommand c{
      .actor = actor,
      .action = static_cast<SocialSpaceAction>(action),
      .space = reader.ReadU32(),
      .target = reader.ReadU32(),
      .text = reader.ReadUtf16(256U)};
  reader.RequireEnd();
  const bool target_action =
      action == 4U || action == 5U || action == 10U || action == 11U;
  const bool text_action = action == 1U || action == 2U || action == 15U;
  if ((target_action != (c.target != 0U)) ||
      (!text_action && !c.text.empty()) ||
      ((action == 0U || action == 1U) != (c.space == 0U)))
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidInput,
        .safe_message = "通讯请求参数无效"};
  std::scoped_lock lock{mutex_};
  if (!sessions_.IsCurrentPlayerSession(actor, session))
    return application::ServiceError{
        .code = application::ErrorCode::kUnauthenticated,
        .safe_message = "通讯会话已经失效"};
  std::erase_if(quota_, [&](auto& entry) {
    return entry.second.empty() ||
           now - entry.second.back() >= std::chrono::seconds{10};
  });
  auto& times = quota_[actor];
  while (!times.empty() && now - times.front() >= std::chrono::seconds{10})
    times.pop_front();
  if (times.size() >= 20U)
    return application::ServiceError{
        .code = application::ErrorCode::kOverloaded,
        .safe_message = "通讯操作过快，请稍后再试"};
  times.push_back(now);
  auto changed = store_.Execute(c);
  if (const auto* error = std::get_if<application::ServiceError>(&changed))
    return *error;
  auto& change = std::get<application::SocialSpacesChange>(changed);
  std::map<domain::PlayerId, std::vector<OutboundMessage>> batches;
  for (const auto& s : change.snapshots)
    batches[s.player].push_back(
        {.opcode = 65037U, .body = Encode(s, sessions_)});
  for (const auto& m : change.messages)
    for (auto p : m.recipients)
      batches[p].push_back({.opcode = 65038U, .body = Encode(m)});
  for (auto& [player, messages] : batches) {
    const auto current = sessions_.CurrentPlayerSession(player);
    if (current && (player != actor || *current == session))
      static_cast<void>(
          sessions_.DispatchIfCurrent(player, *current, std::move(messages)));
  }
  return std::nullopt;
}
}  // namespace bf::runtime
