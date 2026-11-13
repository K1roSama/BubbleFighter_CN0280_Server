#include "features/scenes/native_x_stream.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
constexpr std::size_t kMaximumBytes = 128U * 1024U * 1024U;
constexpr std::uint32_t kWindow = 32768U;

[[noreturn]] void Invalid(std::string_view message) {
  throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                message};
}

void Require(bool value, std::string_view message) {
  if (!value) Invalid(message);
}

class Bits final {
 public:
  explicit Bits(std::span<const std::uint8_t> bytes) : bytes_{bytes} {}

  std::uint32_t Read(std::uint32_t width) {
    Require(width <= 16U, "X MSZIP invalid bit width");
    if (width > bytes_.size() * 8U - position_)
      throw protocol::ProtocolError{
          protocol::ProtocolErrorCode::kTruncatedPayload,
          "X MSZIP bit stream is truncated"};
    std::uint32_t result = 0U;
    for (std::uint32_t index = 0U; index < width; ++index, ++position_)
      result |= ((bytes_[position_ / 8U] >> (position_ % 8U)) & 1U) << index;
    return result;
  }

  void Align() noexcept { position_ = (position_ + 7U) & ~std::size_t{7U}; }

  std::span<const std::uint8_t> Literals(std::uint32_t count) {
    Require(position_ % 8U == 0U, "X MSZIP literal block is unaligned");
    if (count > bytes_.size() - position_ / 8U)
      throw protocol::ProtocolError{
          protocol::ProtocolErrorCode::kTruncatedPayload,
          "X MSZIP literal block is truncated"};
    const auto result = bytes_.subspan(position_ / 8U, count);
    position_ += static_cast<std::size_t>(count) * 8U;
    return result;
  }

  bool Consumed() const noexcept {
    return (position_ + 7U) / 8U == bytes_.size();
  }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t position_{0U};
};

class Huffman final {
 public:
  explicit Huffman(std::span<const std::uint32_t> lengths) {
    for (const auto length : lengths) {
      Require(length <= 15U, "X MSZIP Huffman length exceeds bound");
      if (length) ++counts_[length];
      maximum_ = std::max(maximum_, length);
    }
    std::uint32_t code = 0U;
    for (std::uint32_t length = 1U; length <= maximum_; ++length) {
      code = (code + counts_[length - 1U]) << 1U;
      Require(code + counts_[length] <= (1U << length),
              "X MSZIP Huffman tree is oversubscribed");
      codes_[length] = code;
      starts_[length] = static_cast<std::uint32_t>(symbols_.size());
      for (std::size_t symbol = 0U; symbol < lengths.size(); ++symbol)
        if (lengths[symbol] == length)
          symbols_.push_back(static_cast<std::uint32_t>(symbol));
    }
  }

  std::uint32_t Decode(Bits& bits) const {
    std::uint32_t code = 0U;
    for (std::uint32_t length = 1U; length <= maximum_; ++length) {
      code = (code << 1U) | bits.Read(1U);
      if (code >= codes_[length] && code - codes_[length] < counts_[length])
        return symbols_[starts_[length] + code - codes_[length]];
    }
    Invalid("X MSZIP invalid Huffman code");
  }

 private:
  std::array<std::uint32_t, 16> counts_{}, codes_{}, starts_{};
  std::vector<std::uint32_t> symbols_;
  std::uint32_t maximum_{0U};
};

std::pair<Huffman, Huffman> Tables(Bits& bits, std::uint32_t kind) {
  if (kind == 1U) {
    std::array<std::uint32_t, 288> literals{};
    std::fill(literals.begin(), literals.begin() + 144, 8U);
    std::fill(literals.begin() + 144, literals.begin() + 256, 9U);
    std::fill(literals.begin() + 256, literals.begin() + 280, 7U);
    std::fill(literals.begin() + 280, literals.end(), 8U);
    std::array<std::uint32_t, 32> distances{};
    distances.fill(5U);
    return {Huffman{literals}, Huffman{distances}};
  }
  Require(kind == 2U, "X MSZIP reserved DEFLATE block kind");
  const auto literal_count = bits.Read(5U) + 257U;
  const auto distance_count = bits.Read(5U) + 1U;
  const auto code_count = bits.Read(4U) + 4U;
  Require(literal_count <= 286U && distance_count <= 32U,
          "X MSZIP invalid alphabet size");
  constexpr std::array<std::uint32_t, 19> order{16U, 17U, 18U, 0U,  8U, 7U,  9U,
                                                6U,  10U, 5U,  11U, 4U, 12U, 3U,
                                                13U, 2U,  14U, 1U,  15U};
  std::array<std::uint32_t, 19> code_lengths{};
  for (std::uint32_t index = 0U; index < code_count; ++index)
    code_lengths[order[index]] = bits.Read(3U);
  const Huffman code{code_lengths};
  std::vector<std::uint32_t> lengths;
  const auto total = literal_count + distance_count;
  lengths.reserve(total);
  while (lengths.size() < total) {
    const auto symbol = code.Decode(bits);
    if (symbol < 16U) {
      lengths.push_back(symbol);
      continue;
    }
    std::uint32_t value = 0U, count = 0U;
    if (symbol == 16U) {
      Require(!lengths.empty(), "X MSZIP repeat has no preceding length");
      value = lengths.back();
      count = bits.Read(2U) + 3U;
    } else if (symbol == 17U) {
      count = bits.Read(3U) + 3U;
    } else {
      Require(symbol == 18U, "X MSZIP invalid length symbol");
      count = bits.Read(7U) + 11U;
    }
    Require(count <= total - lengths.size(),
            "X MSZIP code-length repeat exceeds alphabet");
    lengths.insert(lengths.end(), count, value);
  }
  Require(lengths[256U] != 0U, "X MSZIP end-of-block symbol is absent");
  const std::span<const std::uint32_t> all{lengths};
  return {Huffman{all.first(literal_count)},
          Huffman{all.subspan(literal_count)}};
}

std::vector<std::uint8_t> Chunk(std::span<const std::uint8_t> bytes,
                                std::span<const std::uint8_t> dictionary,
                                std::uint32_t expected) {
  Bits bits{bytes};
  std::vector<std::uint8_t> history{dictionary.begin(), dictionary.end()};
  const auto start = history.size();
  history.reserve(start + expected);
  constexpr std::array<std::uint32_t, 29> length_bases{
      3U,  4U,  5U,  6U,   7U,   8U,   9U,   10U,  11U, 13U,
      15U, 17U, 19U, 23U,  27U,  31U,  35U,  43U,  51U, 59U,
      67U, 83U, 99U, 115U, 131U, 163U, 195U, 227U, 258U};
  constexpr std::array<std::uint32_t, 29> length_bits{
      0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 2U, 2U, 2U,
      2U, 3U, 3U, 3U, 3U, 4U, 4U, 4U, 4U, 5U, 5U, 5U, 5U, 0U};
  std::array<std::uint32_t, 30> distance_bases{}, distance_bits{};
  std::uint32_t distance = 1U;
  for (std::uint32_t index = 0U; index < 30U; ++index) {
    distance_bases[index] = distance;
    distance_bits[index] = index < 4U ? 0U : index / 2U - 1U;
    distance += 1U << distance_bits[index];
  }
  for (std::uint32_t block = 0U; block < 100000U; ++block) {
    const auto final = bits.Read(1U);
    const auto kind = bits.Read(2U);
    if (kind == 0U) {
      bits.Align();
      const auto count = bits.Read(16U);
      Require(count <= expected - (history.size() - start),
              "X MSZIP stored block exceeds output");
      const auto literals = bits.Literals(count);
      history.insert(history.end(), literals.begin(), literals.end());
    } else {
      const auto [literal, distances] = Tables(bits, kind);
      for (;;) {
        const auto symbol = literal.Decode(bits);
        if (symbol == 256U) break;
        if (symbol < 256U) {
          Require(history.size() - start < expected,
                  "X MSZIP literal exceeds output");
          history.push_back(static_cast<std::uint8_t>(symbol));
          continue;
        }
        Require(symbol <= 285U, "X MSZIP reserved length symbol");
        const auto length_index = symbol - 257U;
        const auto count =
            length_bases[length_index] + bits.Read(length_bits[length_index]);
        const auto index = distances.Decode(bits);
        Require(index < 30U, "X MSZIP reserved distance symbol");
        const auto offset =
            distance_bases[index] + bits.Read(distance_bits[index]);
        Require(offset <= kWindow && offset <= history.size(),
                "X MSZIP invalid back-reference");
        Require(count <= expected - (history.size() - start),
                "X MSZIP match exceeds output");
        for (std::uint32_t item = 0U; item < count; ++item)
          history.push_back(history[history.size() - offset]);
      }
    }
    if (final) {
      Require(history.size() - start == expected && bits.Consumed(),
              "X MSZIP chunk extent differs");
      return {history.begin() + static_cast<std::ptrdiff_t>(start),
              history.end()};
    }
  }
  Invalid("X MSZIP block count exceeds bound");
}
}  // namespace

NativeXStream ReadNativeXStream(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaximumBytes)
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kLengthLimit,
                                  "X input exceeds limit"};
  Require(bytes.size() >= 16U, "X header is truncated");
  const std::string_view header{reinterpret_cast<const char*>(bytes.data()),
                                16U};
  Require(header.substr(0U, 4U) == "xof ", "unsupported X magic");
  Require(header.substr(4U, 4U) == "0302" || header.substr(4U, 4U) == "0303",
          "unsupported X version");
  Require(header.substr(12U, 4U) == "0032",
          "unsupported X floating-point width");
  const auto kind = header.substr(8U, 4U);
  Require(kind == "bin " || kind == "txt " || kind == "bzip" || kind == "tzip",
          "unsupported X encoding");
  NativeXStream result{.binary = kind == "bin " || kind == "bzip",
                       .payload = {}};
  if (kind == "bin " || kind == "txt ") {
    result.payload.assign(bytes.begin() + 16, bytes.end());
    return result;
  }
  protocol::PacketReader input{bytes.subspan(16U)};
  const auto declared = input.ReadU32();
  if (declared > kMaximumBytes)
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kLengthLimit,
                                  "X expanded stream exceeds limit"};
  Require(declared >= 16U, "X invalid expanded length");
  const auto expected = declared - 16U;
  result.payload.reserve(expected);
  while (input.RemainingBytes()) {
    const auto count = input.ReadU16();
    const auto compressed = input.ReadU16();
    Require(count > 0U && count <= kWindow && compressed > 2U &&
                compressed <= 32780U,
            "X invalid MSZIP chunk length");
    Require(count <= expected - result.payload.size(),
            "X chunk exceeds expanded length");
    const auto raw = input.ReadBytes(compressed);
    Require(raw[0] == 'C' && raw[1] == 'K', "X MSZIP signature differs");
    const std::span<const std::uint8_t> prior{result.payload};
    const auto dictionary =
        prior.last(std::min(prior.size(), static_cast<std::size_t>(kWindow)));
    auto decoded = Chunk(raw.subspan(2U), dictionary, count);
    result.payload.insert(result.payload.end(), decoded.begin(), decoded.end());
  }
  Require(result.payload.size() == expected, "X expanded length differs");
  return result;
}
}  // namespace bf::runtime
