#include "protocol/codec/packet_io.h"

#include <bit>
#include <limits>
#include <utility>

namespace bf::protocol {
namespace {

static_assert(sizeof(float) == sizeof(std::uint32_t) &&
                  std::numeric_limits<float>::is_iec559,
              "Nx float fields require a 32-bit IEEE-754 representation");
static_assert(sizeof(double) == sizeof(std::uint64_t) &&
                  std::numeric_limits<double>::is_iec559,
              "Nx double fields require a 64-bit IEEE-754 representation");

constexpr char16_t kHighSurrogateFirst = 0xD800;
constexpr char16_t kHighSurrogateLast = 0xDBFF;
constexpr char16_t kLowSurrogateFirst = 0xDC00;
constexpr char16_t kLowSurrogateLast = 0xDFFF;

bool IsHighSurrogate(char16_t value) noexcept {
  return value >= kHighSurrogateFirst && value <= kHighSurrogateLast;
}

bool IsLowSurrogate(char16_t value) noexcept {
  return value >= kLowSurrogateFirst && value <= kLowSurrogateLast;
}

void ValidateUtf16(std::u16string_view value) {
  for (std::size_t index = 0U; index < value.size(); ++index) {
    if (IsHighSurrogate(value[index])) {
      if (index + 1U >= value.size() || !IsLowSurrogate(value[index + 1U])) {
        throw ProtocolError{ProtocolErrorCode::kInvalidUtf16,
                            "UTF-16 high surrogate is not paired"};
      }
      ++index;
    } else if (IsLowSurrogate(value[index])) {
      throw ProtocolError{ProtocolErrorCode::kInvalidUtf16,
                          "UTF-16 low surrogate has no leading pair"};
    }
  }
}

template <typename Integer>
Integer ReadLittleEndian(PacketReader& reader) {
  const auto bytes = reader.ReadBytes(sizeof(Integer));
  Integer result = 0;
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    result |= static_cast<Integer>(bytes[index]) << (index * 8U);
  }
  return result;
}

template <typename Integer>
void WriteLittleEndian(std::vector<std::uint8_t>& output, Integer value) {
  for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
    output.push_back(
        static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU));
  }
}

}  // namespace

ProtocolError::ProtocolError(ProtocolErrorCode code, std::string_view message)
    : std::runtime_error(std::string{message}), code_(code) {}

ProtocolErrorCode ProtocolError::Code() const noexcept { return code_; }

std::string_view ProtocolErrorCodeName(ProtocolErrorCode code) noexcept {
  switch (code) {
    case ProtocolErrorCode::kTruncatedPayload:
      return "truncated_payload";
    case ProtocolErrorCode::kTrailingPayload:
      return "trailing_payload";
    case ProtocolErrorCode::kLengthLimit:
      return "length_limit";
    case ProtocolErrorCode::kInvalidUtf16:
      return "invalid_utf16";
    case ProtocolErrorCode::kInvalidUtf8:
      return "invalid_utf8";
    case ProtocolErrorCode::kInvalidValue:
      return "invalid_value";
    case ProtocolErrorCode::kChecksumMismatch:
      return "checksum_mismatch";
    case ProtocolErrorCode::kInvalidFrame:
      return "invalid_frame";
    case ProtocolErrorCode::kReceiveBufferLimit:
      return "receive_buffer_limit";
  }
  return "unknown_protocol_error";
}

PacketReader::PacketReader(std::span<const std::uint8_t> payload) noexcept
    : payload_(payload) {}

std::uint8_t PacketReader::ReadU8() {
  return ReadLittleEndian<std::uint8_t>(*this);
}

std::int8_t PacketReader::ReadI8() {
  return std::bit_cast<std::int8_t>(ReadU8());
}

std::uint16_t PacketReader::ReadU16() {
  return ReadLittleEndian<std::uint16_t>(*this);
}

std::int16_t PacketReader::ReadI16() {
  return std::bit_cast<std::int16_t>(ReadU16());
}

std::uint32_t PacketReader::ReadU32() {
  return ReadLittleEndian<std::uint32_t>(*this);
}

std::int32_t PacketReader::ReadI32() {
  return std::bit_cast<std::int32_t>(ReadU32());
}

std::uint64_t PacketReader::ReadU64() {
  return ReadLittleEndian<std::uint64_t>(*this);
}

float PacketReader::ReadF32() { return std::bit_cast<float>(ReadU32()); }

double PacketReader::ReadF64() { return std::bit_cast<double>(ReadU64()); }

std::uint32_t PacketReader::ReadCount(std::size_t maximum_elements) {
  const auto count = ReadU32();
  if (count > maximum_elements) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "vector element count exceeds limit"};
  }
  return count;
}

std::u16string PacketReader::ReadUtf16(std::size_t maximum_code_units) {
  const auto code_units = ReadU32();
  if (code_units > maximum_code_units ||
      code_units > std::numeric_limits<std::size_t>::max() / 2U) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "UTF-16 field exceeds limit"};
  }
  const auto raw = ReadBytes(static_cast<std::size_t>(code_units) * 2U);
  std::u16string result;
  result.reserve(code_units);
  for (std::size_t offset = 0U; offset < raw.size(); offset += 2U) {
    result.push_back(static_cast<char16_t>(
        static_cast<std::uint16_t>(raw[offset]) |
        (static_cast<std::uint16_t>(raw[offset + 1U]) << 8U)));
  }
  ValidateUtf16(result);
  return result;
}

std::span<const std::uint8_t> PacketReader::ReadBytes(std::size_t count) {
  if (count > payload_.size() - position_) {
    throw ProtocolError{ProtocolErrorCode::kTruncatedPayload,
                        "payload is truncated"};
  }
  const auto result = payload_.subspan(position_, count);
  position_ += count;
  return result;
}

void PacketReader::RequireEnd() const {
  if (position_ != payload_.size()) {
    throw ProtocolError{ProtocolErrorCode::kTrailingPayload,
                        "payload has trailing bytes"};
  }
}

std::size_t PacketReader::RemainingBytes() const noexcept {
  return payload_.size() - position_;
}

void PacketWriter::WriteU8(std::uint8_t value) {
  WriteLittleEndian(bytes_, value);
}

void PacketWriter::WriteI8(std::int8_t value) {
  WriteU8(std::bit_cast<std::uint8_t>(value));
}

void PacketWriter::WriteU16(std::uint16_t value) {
  WriteLittleEndian(bytes_, value);
}

void PacketWriter::WriteI16(std::int16_t value) {
  WriteU16(std::bit_cast<std::uint16_t>(value));
}

void PacketWriter::WriteU32(std::uint32_t value) {
  WriteLittleEndian(bytes_, value);
}

void PacketWriter::WriteI32(std::int32_t value) {
  WriteU32(std::bit_cast<std::uint32_t>(value));
}

void PacketWriter::WriteU64(std::uint64_t value) {
  WriteLittleEndian(bytes_, value);
}

void PacketWriter::WriteF32(float value) {
  WriteU32(std::bit_cast<std::uint32_t>(value));
}

void PacketWriter::WriteF64(double value) {
  WriteU64(std::bit_cast<std::uint64_t>(value));
}

void PacketWriter::WriteUtf16(std::u16string_view value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "UTF-16 field exceeds uint32 length"};
  }
  ValidateUtf16(value);
  WriteU32(static_cast<std::uint32_t>(value.size()));
  for (const char16_t code_unit : value) {
    WriteU16(static_cast<std::uint16_t>(code_unit));
  }
}

void PacketWriter::WriteBytes(std::span<const std::uint8_t> value) {
  bytes_.insert(bytes_.end(), value.begin(), value.end());
}

std::span<const std::uint8_t> PacketWriter::Bytes() const noexcept {
  return bytes_;
}

std::vector<std::uint8_t> PacketWriter::Release() noexcept {
  return std::exchange(bytes_, {});
}

}  // namespace bf::protocol
