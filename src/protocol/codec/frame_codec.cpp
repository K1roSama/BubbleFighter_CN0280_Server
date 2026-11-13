#include "protocol/codec/frame_codec.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {

constexpr std::array<std::uint8_t, 49U> kCipherTableA{
    0x5F, 0x3E, 0x32, 0x72, 0xC1, 0x18, 0x46, 0x87, 0x5D, 0x2E,
    0x29, 0x4E, 0x68, 0x79, 0x86, 0x6C, 0xB5, 0x8D, 0x56, 0xF1,
    0x38, 0xC1, 0x48, 0x42, 0x5F, 0x0C, 0xCD, 0x80, 0xE8, 0xE2,
    0xFE, 0xF0, 0x42, 0x5F, 0xF7, 0x59, 0x7E, 0x5F, 0xA5, 0x8D,
    0x3D, 0x76, 0xE7, 0x11, 0x41, 0x62, 0x67, 0x59, 0x7F,
};
constexpr std::array<std::uint8_t, 24U> kCipherTableB{
    0x23, 0x00, 0x24, 0x00, 0x25, 0x00, 0x73, 0x00, 0x65, 0x00, 0x72, 0x00,
    0x74, 0x00, 0x33, 0x00, 0x34, 0x00, 0x25, 0x00, 0x77, 0x00, 0x65, 0x00,
};
constexpr std::size_t kFrameHeaderBytes = 5U;
constexpr std::uint16_t kEnvironmentReportOpcode = 21U;
constexpr std::size_t kEnvironmentLengthPrefixBytes = 4U;

std::uint16_t ReadU16At(std::span<const std::uint8_t> bytes,
                        std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(bytes[offset]) |
         (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t ReadU32At(std::span<const std::uint8_t> bytes,
                        std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

}  // namespace

LegacyCipher::LegacyCipher(std::uint8_t table_a_index,
                           std::uint8_t table_b_index)
    : table_a_index_(table_a_index), table_b_index_(table_b_index) {
  if (table_a_index >= kCipherTableA.size() ||
      table_b_index >= kCipherTableB.size()) {
    throw std::invalid_argument("legacy cipher index is out of range");
  }
}

LegacyCipher LegacyCipher::ForEndpoint(CipherEndpoint endpoint) {
  if (endpoint == CipherEndpoint::kClientToServer) {
    return LegacyCipher{0U, 1U};
  }
  return LegacyCipher{5U, 3U};
}

void LegacyCipher::Encrypt(std::span<std::uint8_t> bytes) noexcept {
  Transform(bytes, false);
}

void LegacyCipher::Decrypt(std::span<std::uint8_t> bytes) noexcept {
  Transform(bytes, true);
}

void LegacyCipher::Transform(std::span<std::uint8_t> bytes,
                             bool is_decrypting) noexcept {
  for (auto& byte : bytes) {
    const auto offset = static_cast<std::uint8_t>(
        kCipherTableA[table_a_index_] + kCipherTableB[table_b_index_]);
    byte = is_decrypting ? static_cast<std::uint8_t>(byte - offset)
                         : static_cast<std::uint8_t>(byte + offset);
    table_a_index_ =
        static_cast<std::uint8_t>((table_a_index_ + 1U) % kCipherTableA.size());
    table_b_index_ =
        static_cast<std::uint8_t>((table_b_index_ + 1U) % kCipherTableB.size());
  }
}

std::uint8_t PayloadChecksum(std::span<const std::uint8_t> body) noexcept {
  std::uint8_t result = 0U;
  for (const auto byte : body) {
    result ^= byte;
  }
  return result;
}

std::vector<std::uint8_t> EncodeFrame(std::uint16_t opcode,
                                      std::span<const std::uint8_t> body) {
  if (body.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "outgoing frame exceeds uint16 payload length"};
  }
  std::vector<std::uint8_t> result;
  result.reserve(kFrameHeaderBytes + body.size());
  const auto size = static_cast<std::uint16_t>(body.size());
  result.push_back(static_cast<std::uint8_t>(size & 0xFFU));
  result.push_back(static_cast<std::uint8_t>(size >> 8U));
  result.push_back(static_cast<std::uint8_t>(opcode & 0xFFU));
  result.push_back(static_cast<std::uint8_t>(opcode >> 8U));
  result.push_back(PayloadChecksum(body));
  result.insert(result.end(), body.begin(), body.end());
  return result;
}

std::array<std::uint8_t, 258U> MakeServerHello() noexcept {
  std::array<std::uint8_t, 258U> hello{};
  hello[0] = 2U;
  return hello;
}

FrameDecoder::FrameDecoder(std::size_t maximum_frame_bytes,
                           std::size_t maximum_environment_code_units,
                           FrameNamespace message_namespace)
    : cipher_(LegacyCipher::ForEndpoint(CipherEndpoint::kClientToServer)),
      maximum_frame_bytes_(maximum_frame_bytes),
      maximum_environment_code_units_(maximum_environment_code_units),
      message_namespace_(message_namespace) {
  if (maximum_frame_bytes < kFrameHeaderBytes ||
      maximum_environment_code_units == 0U) {
    throw std::invalid_argument("invalid frame decoder limits");
  }
}

std::vector<Packet> FrameDecoder::Feed(
    std::span<const std::uint8_t> encrypted_bytes) {
  if (encrypted_bytes.size() > maximum_frame_bytes_) {
    throw ProtocolError{ProtocolErrorCode::kReceiveBufferLimit,
                        "single network read exceeds configured decoder limit"};
  }
  std::vector<std::uint8_t> decrypted(encrypted_bytes.begin(),
                                      encrypted_bytes.end());
  cipher_.Decrypt(decrypted);
  buffer_.insert(buffer_.end(), decrypted.begin(), decrypted.end());

  std::vector<Packet> packets;
  std::size_t consumed = 0U;
  while (buffer_.size() - consumed >= kFrameHeaderBytes) {
    const std::span<const std::uint8_t> available{buffer_.data() + consumed,
                                                  buffer_.size() - consumed};
    const auto opcode = ReadU16At(available, 2U);
    if (message_namespace_ == FrameNamespace::kGateway &&
        opcode == kEnvironmentReportOpcode &&
        available.size() < kFrameHeaderBytes + kEnvironmentLengthPrefixBytes) {
      break;
    }
    const auto body_bytes = FrameBodyBytes(available);
    const auto frame_bytes = kFrameHeaderBytes + body_bytes;
    if (frame_bytes > maximum_frame_bytes_) {
      throw ProtocolError{ProtocolErrorCode::kReceiveBufferLimit,
                          "incoming frame exceeds configured limit"};
    }
    if (available.size() < frame_bytes) {
      break;
    }
    const std::span<const std::uint8_t> body =
        available.subspan(kFrameHeaderBytes, body_bytes);
    if (PayloadChecksum(body) != available[4U]) {
      throw ProtocolError{ProtocolErrorCode::kChecksumMismatch,
                          "payload checksum does not match frame header"};
    }
    packets.push_back(
        Packet{.opcode = opcode,
               .body = std::vector<std::uint8_t>{body.begin(), body.end()}});
    consumed += frame_bytes;
  }

  if (consumed != 0U) {
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
  }
  if (buffer_.size() > maximum_frame_bytes_) {
    throw ProtocolError{ProtocolErrorCode::kReceiveBufferLimit,
                        "incomplete receive buffer exceeds configured limit"};
  }
  return packets;
}

std::size_t FrameDecoder::BufferedBytes() const noexcept {
  return buffer_.size();
}

std::size_t FrameDecoder::FrameBodyBytes(
    std::span<const std::uint8_t> available) const {
  const auto declared_size = ReadU16At(available, 0U);
  const auto opcode = ReadU16At(available, 2U);
  if (message_namespace_ != FrameNamespace::kGateway ||
      opcode != kEnvironmentReportOpcode) {
    return declared_size;
  }
  const auto code_units = ReadU32At(available, kFrameHeaderBytes);
  if (code_units > maximum_environment_code_units_ ||
      code_units > (std::numeric_limits<std::size_t>::max() -
                    kEnvironmentLengthPrefixBytes) /
                       2U) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "C2S21 environment report exceeds code-unit limit"};
  }
  const auto actual_size =
      kEnvironmentLengthPrefixBytes + static_cast<std::size_t>(code_units) * 2U;
  if (declared_size != static_cast<std::uint16_t>(actual_size & 0xFFFFU)) {
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "C2S21 wrapped length does not match UTF-16 field"};
  }
  return actual_size;
}

}  // namespace bf::protocol
