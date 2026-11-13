#include "features/scenes/native_x_scene.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <map>
#include <system_error>
#include <utility>

#include "features/scenes/native_x_stream.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
constexpr std::size_t kMaximumNodes = 300000U;
constexpr std::size_t kMaximumDepth = 128U;
constexpr std::size_t kMaximumNumericBytes = 128U * 1024U * 1024U;
constexpr std::size_t kMaximumValues = 10000000U;

[[noreturn]] void Invalid(std::string_view message) {
  throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                message};
}

void Require(bool value, std::string_view message) {
  if (!value) Invalid(message);
}

std::string_view Text(std::span<const std::uint8_t> raw) {
  if (raw.empty()) return {};
  return {reinterpret_cast<const char*>(raw.data()), raw.size()};
}

std::uint32_t Word(std::span<const std::uint8_t> raw) {
  Require(raw.size() == 4U, "X integer width differs");
  std::uint32_t result = 0U;
  for (std::size_t index = 0U; index < 4U; ++index)
    result |= static_cast<std::uint32_t>(raw[index]) << (index * 8U);
  return result;
}

bool Digit(std::uint8_t value) noexcept { return value >= '0' && value <= '9'; }

bool HexDigit(std::uint8_t value) noexcept {
  return Digit(value) || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

bool Whitespace(std::uint8_t value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
         value == '\v' || value == '\f';
}

std::string Guid(std::span<const std::uint8_t> raw, bool binary) {
  if (!binary) {
    Require(raw.size() == 36U, "X GUID width differs");
    std::string result{Text(raw)};
    for (std::size_t index = 0U; index < result.size(); ++index) {
      if (index == 8U || index == 13U || index == 18U || index == 23U) {
        Require(result[index] == '-', "X GUID separator differs");
      } else {
        Require(HexDigit(static_cast<std::uint8_t>(result[index])),
                "X GUID has invalid digit");
        if (result[index] >= 'A' && result[index] <= 'F')
          result[index] = static_cast<char>(result[index] - 'A' + 'a');
      }
    }
    return result;
  }
  Require(raw.size() == 16U, "X binary GUID width differs");
  constexpr std::array<std::size_t, 16> order{
      3U, 2U, 1U, 0U, 5U, 4U, 7U, 6U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U};
  constexpr std::string_view digits{"0123456789abcdef"};
  std::string result;
  result.reserve(36U);
  for (std::size_t index = 0U; index < order.size(); ++index) {
    if (index == 4U || index == 6U || index == 8U || index == 10U)
      result.push_back('-');
    const auto value = raw[order[index]];
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 15U]);
  }
  return result;
}

enum class Kind {
  kEnd,
  kName,
  kString,
  kInt,
  kFloat,
  kGuid,
  kNumber,
  kTemplate,
  kKeyword,
  kLeftBrace,
  kRightBrace,
  kComma,
  kSemicolon,
  kOther
};

struct Token final {
  Kind kind{Kind::kEnd};
  std::span<const std::uint8_t> raw;
};

Kind Punctuation(std::uint32_t token) {
  if (token == 10U) return Kind::kLeftBrace;
  if (token == 11U) return Kind::kRightBrace;
  if (token == 19U) return Kind::kComma;
  if (token == 20U) return Kind::kSemicolon;
  Require(token >= 12U && token <= 18U, "X unknown binary punctuation");
  return Kind::kOther;
}

class Lexer final {
 public:
  explicit Lexer(const NativeXStream& stream)
      : bytes_{stream.payload}, binary_{stream.binary} {}

  bool Binary() const noexcept { return binary_; }

  Token Next() {
    if (binary_) return BinaryToken();
    return TextToken();
  }

 private:
  std::span<const std::uint8_t> Take(std::size_t count) {
    if (count > bytes_.size() - position_)
      throw protocol::ProtocolError{
          protocol::ProtocolErrorCode::kTruncatedPayload,
          "X token is truncated"};
    const auto result = bytes_.subspan(position_, count);
    position_ += count;
    return result;
  }

  std::uint32_t U32() { return Word(Take(4U)); }

  Token BinaryToken() {
    if (position_ == bytes_.size()) return {};
    const auto raw = Take(2U);
    const auto token = static_cast<std::uint32_t>(raw[0]) |
                       (static_cast<std::uint32_t>(raw[1]) << 8U);
    if (token >= 10U && token <= 20U)
      return {.kind = Punctuation(token), .raw = {}};
    if (token == 31U) return {.kind = Kind::kTemplate, .raw = {}};
    if (token == 1U || token == 2U) {
      const auto count = U32();
      Require(count <= (token == 1U ? 4096U : 1048576U),
              "X name or string exceeds bound");
      return {.kind = token == 1U ? Kind::kName : Kind::kString,
              .raw = Take(count)};
    }
    if (token == 3U) return {.kind = Kind::kInt, .raw = Take(4U)};
    if (token == 5U) return {.kind = Kind::kGuid, .raw = Take(16U)};
    if (token == 6U || token == 7U) {
      const auto count = U32();
      Require(count <= (bytes_.size() - position_) / 4U,
              "X numeric token count exceeds extent");
      return {.kind = token == 6U ? Kind::kInt : Kind::kFloat,
              .raw = Take(static_cast<std::size_t>(count) * 4U)};
    }
    Require(token >= 40U && token <= 52U, "X unsupported binary token");
    return {.kind = Kind::kKeyword, .raw = {}};
  }

  bool Starts(std::string_view value) const {
    if (value.size() > bytes_.size() - position_) return false;
    return Text(bytes_.subspan(position_, value.size())) == value;
  }

  void SkipSpace() {
    while (position_ < bytes_.size()) {
      if (Whitespace(bytes_[position_])) {
        ++position_;
      } else if (Starts("//") || bytes_[position_] == '#') {
        while (position_ < bytes_.size() && bytes_[position_] != '\n')
          ++position_;
      } else if (Starts("/*")) {
        position_ += 2U;
        while (position_ < bytes_.size() && !Starts("*/")) ++position_;
        Require(position_ < bytes_.size(), "X comment is unterminated");
        position_ += 2U;
      } else {
        return;
      }
    }
  }

  Token Number() {
    const auto start = position_;
    if (bytes_[position_] == '+' || bytes_[position_] == '-') ++position_;
    if (Starts("0x") || Starts("0X")) {
      position_ += 2U;
      const auto digits = position_;
      while (position_ < bytes_.size() && HexDigit(bytes_[position_]))
        ++position_;
      Require(position_ != digits, "X hex number has no digits");
    } else {
      std::size_t digits = 0U;
      while (position_ < bytes_.size() && Digit(bytes_[position_])) {
        ++position_;
        ++digits;
      }
      if (position_ < bytes_.size() && bytes_[position_] == '.') {
        ++position_;
        while (position_ < bytes_.size() && Digit(bytes_[position_])) {
          ++position_;
          ++digits;
        }
      }
      Require(digits > 0U, "X number has no digits");
      if (position_ < bytes_.size() &&
          (bytes_[position_] == 'e' || bytes_[position_] == 'E')) {
        ++position_;
        if (position_ < bytes_.size() &&
            (bytes_[position_] == '+' || bytes_[position_] == '-'))
          ++position_;
        const auto exponent = position_;
        while (position_ < bytes_.size() && Digit(bytes_[position_]))
          ++position_;
        Require(position_ > exponent, "X exponent has no digits");
      }
    }
    Require(position_ - start <= 128U, "X number exceeds bound");
    return {.kind = Kind::kNumber,
            .raw = bytes_.subspan(start, position_ - start)};
  }

  Token TextToken() {
    SkipSpace();
    if (position_ == bytes_.size()) return {};
    const auto start = position_;
    const auto value = bytes_[position_];
    if (Digit(value) || value == '+' || value == '-' ||
        (value == '.' && position_ + 1U < bytes_.size() &&
         Digit(bytes_[position_ + 1U])))
      return Number();
    ++position_;
    switch (value) {
      case '{':
        return {.kind = Kind::kLeftBrace, .raw = {}};
      case '}':
        return {.kind = Kind::kRightBrace, .raw = {}};
      case ',':
        return {.kind = Kind::kComma, .raw = {}};
      case ';':
        return {.kind = Kind::kSemicolon, .raw = {}};
      case '[':
      case ']':
      case '(':
      case ')':
      case '.':
        return {.kind = Kind::kOther, .raw = {}};
      case '<': {
        const auto begin = position_;
        while (position_ < bytes_.size() && bytes_[position_] != '>')
          ++position_;
        Require(position_ < bytes_.size(), "X GUID is unterminated");
        const auto result = bytes_.subspan(begin, position_ - begin);
        ++position_;
        return {.kind = Kind::kGuid, .raw = result};
      }
      case '"': {
        bool escaped = false;
        while (position_ < bytes_.size()) {
          const auto current = bytes_[position_++];
          if (current == '"' && !escaped) {
            Require(position_ - start - 2U <= 1048576U,
                    "X string exceeds bound");
            return {.kind = Kind::kString,
                    .raw = bytes_.subspan(start + 1U, position_ - start - 2U)};
          }
          escaped = current == '\\' && !escaped;
        }
        Invalid("X string is unterminated");
      }
      default:
        break;
    }
    constexpr std::string_view delimiters{" \t\r\n{};,[]()<>\""};
    while (position_ < bytes_.size() &&
           delimiters.find(static_cast<char>(bytes_[position_])) ==
               std::string_view::npos)
      ++position_;
    const auto raw = bytes_.subspan(start, position_ - start);
    Require(!raw.empty() && raw.size() <= 4096U, "X name exceeds bound");
    return {.kind = Text(raw) == "template" ? Kind::kTemplate : Kind::kName,
            .raw = raw};
  }

  std::span<const std::uint8_t> bytes_;
  bool binary_;
  std::size_t position_{0U};
};

class Numeric final {
 public:
  Numeric(std::span<const Token> fields, std::size_t& budget)
      : fields_{fields}, budget_{budget} {
    for (const auto& field : fields)
      remaining_ += field.kind == Kind::kInt || field.kind == Kind::kFloat
                        ? field.raw.size() / 4U
                        : 1U;
  }

  std::size_t Remaining() const noexcept { return remaining_; }

  std::uint32_t Integer(bool short_word = false) {
    const auto token = Next();
    std::uint32_t value = 0U;
    if (token.kind == Kind::kNumber) {
      auto number = Text(token.raw);
      if (!number.empty() && number.front() == '+') number.remove_prefix(1U);
      const auto base =
          number.starts_with("0x") || number.starts_with("0X") ? 16 : 10;
      if (base == 16) number.remove_prefix(2U);
      const auto parsed = std::from_chars(
          number.data(), number.data() + number.size(), value, base);
      Require(!number.empty() && parsed.ec == std::errc{} &&
                  parsed.ptr == number.data() + number.size(),
              "X invalid unsigned integer");
    } else {
      Require(token.kind == Kind::kInt, "X integer token type differs");
      value = Word(token.raw);
    }
    Require(!short_word || value <= 65535U, "X WORD exceeds range");
    Append(value, short_word ? 2U : 4U);
    return value;
  }

  void Floats(std::size_t count) {
    Require(count <= kMaximumValues && count <= remaining_,
            "X float array exceeds extent");
    for (std::size_t index = 0U; index < count; ++index) {
      const auto token = Next();
      float value = 0.0F;
      if (token.kind == Kind::kNumber) {
        auto number = Text(token.raw);
        if (!number.empty() && number.front() == '+') number.remove_prefix(1U);
        double wide = 0.0;
        const auto parsed =
            std::from_chars(number.data(), number.data() + number.size(), wide,
                            std::chars_format::general);
        Require(!number.empty() && parsed.ec == std::errc{} &&
                    parsed.ptr == number.data() + number.size(),
                "X invalid float text");
        value = static_cast<float>(wide);
      } else {
        Require(token.kind == Kind::kFloat, "X float token type differs");
        value = std::bit_cast<float>(Word(token.raw));
      }
      Require(std::isfinite(value), "X scalar is nonfinite");
      Append(std::bit_cast<std::uint32_t>(value), 4U);
    }
  }

  void Faces(std::uint32_t vertices) {
    const auto count = Integer();
    Require(count <= kMaximumValues && count <= remaining_,
            "X face count exceeds extent");
    for (std::uint32_t face = 0U; face < count; ++face) {
      const auto width = Integer();
      Require(width <= 1000000U && width <= remaining_,
              "X polygon exceeds extent");
      for (std::uint32_t index = 0U; index < width; ++index)
        Require(Integer() < vertices, "X vertex index is out of range");
    }
  }

  std::vector<std::uint8_t> Finish() {
    Require(remaining_ == 0U, "X numeric payload has trailing values");
    return std::move(output_);
  }

 private:
  Token Next() {
    while (field_ < fields_.size()) {
      const auto& source = fields_[field_];
      if (source.kind == Kind::kInt || source.kind == Kind::kFloat) {
        if (offset_ == source.raw.size()) {
          ++field_;
          offset_ = 0U;
          continue;
        }
        const auto raw = source.raw.subspan(offset_, 4U);
        offset_ += 4U;
        Require(remaining_ > 0U, "X missing numeric value");
        --remaining_;
        return {.kind = source.kind, .raw = raw};
      }
      ++field_;
      Require(remaining_ > 0U, "X missing numeric value");
      --remaining_;
      return source;
    }
    Invalid("X numeric payload is truncated");
  }

  void Append(std::uint32_t value, std::size_t width) {
    Require(width <= budget_, "X total numeric data exceeds budget");
    budget_ -= width;
    for (std::size_t index = 0U; index < width; ++index)
      output_.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }

  std::span<const Token> fields_;
  std::size_t& budget_;
  std::size_t field_{0U}, offset_{0U}, remaining_{0U};
  std::vector<std::uint8_t> output_;
};

std::optional<std::vector<std::uint8_t>> NumericData(
    std::string_view kind, std::span<const Token> fields, std::size_t& budget) {
  if (kind == "TextureFilename") {
    Require(fields.size() == 1U && fields[0].kind == Kind::kString,
            "X texture filename shape differs");
    return std::nullopt;
  }
  Numeric reader{fields, budget};
  if (kind == "Mesh" || kind == "MeshNormals") {
    const auto count = reader.Integer();
    reader.Floats(static_cast<std::size_t>(count) * 3U);
    reader.Faces(count);
  } else if (kind == "MeshTextureCoords") {
    reader.Floats(static_cast<std::size_t>(reader.Integer()) * 2U);
  } else if (kind == "MeshMaterialList") {
    const auto materials = reader.Integer();
    const auto count = reader.Integer();
    Require(count <= reader.Remaining(),
            "X material index array exceeds extent");
    for (std::uint32_t index = 0U; index < count; ++index)
      Require(reader.Integer() < materials, "X material index is out of range");
  } else if (kind == "MeshVertexColors") {
    const auto count = reader.Integer();
    Require(count <= reader.Remaining() / 5U, "X color array exceeds extent");
    for (std::uint32_t index = 0U; index < count; ++index) {
      reader.Integer();
      reader.Floats(4U);
    }
  } else if (kind == "FrameTransformMatrix") {
    reader.Floats(16U);
  } else if (kind == "Material") {
    reader.Floats(11U);
  } else if (kind == "Header") {
    reader.Integer(true);
    reader.Integer(true);
    reader.Integer();
  } else if (kind == "AnimationOptions" || kind == "AnimTicksPerSecond") {
    reader.Integer();
    if (kind == "AnimationOptions") reader.Integer();
  } else if (kind == "AnimationKey") {
    reader.Integer();
    const auto count = reader.Integer();
    Require(count <= reader.Remaining() / 2U,
            "X animation key count exceeds extent");
    for (std::uint32_t index = 0U; index < count; ++index) {
      reader.Integer();
      reader.Floats(reader.Integer());
    }
  } else if (kind == "FVFData") {
    reader.Integer();
    const auto count = reader.Integer();
    Require(count <= reader.Remaining(), "X FVF array exceeds extent");
    for (std::uint32_t index = 0U; index < count; ++index) reader.Integer();
  } else {
    Require(kind == "Frame" || kind == "Animation" || kind == "AnimationSet",
            "unsupported X object template");
  }
  return reader.Finish();
}

using Types = std::map<std::string, std::string, std::less<>>;

Types StandardTypes() {
  return {{"Header", "3d82ab43-62da-11cf-ab39-0020af71e433"},
          {"Material", "3d82ab4d-62da-11cf-ab39-0020af71e433"},
          {"TextureFilename", "a42790e1-7810-11cf-8f52-0040333594a3"},
          {"Mesh", "3d82ab44-62da-11cf-ab39-0020af71e433"},
          {"MeshMaterialList", "f6f23f42-7686-11cf-8f52-0040333594a3"},
          {"MeshNormals", "f6f23f43-7686-11cf-8f52-0040333594a3"},
          {"MeshTextureCoords", "f6f23f40-7686-11cf-8f52-0040333594a3"},
          {"Frame", "3d82ab46-62da-11cf-ab39-0020af71e433"},
          {"FrameTransformMatrix", "f6f23f41-7686-11cf-8f52-0040333594a3"},
          {"AnimationSet", "3d82ab50-62da-11cf-ab39-0020af71e433"},
          {"Animation", "3d82ab4f-62da-11cf-ab39-0020af71e433"},
          {"AnimationKey", "10dd46a8-775b-11cf-8f52-0040333594a3"},
          {"AnimationOptions", "e2bf56c0-840f-11cf-8f52-0040333594a3"},
          {"AnimTicksPerSecond", "9e415a43-7ba6-4a73-8743-b73d47e88476"},
          {"MeshVertexColors", "1630b821-7842-11cf-8f52-0040333594a3"}};
}

struct RawNode final {
  NativeXNode value;
  std::vector<Token> fields;
};

class Parser final {
 public:
  explicit Parser(const NativeXStream& stream)
      : lexer_{stream}, current_{lexer_.Next()} {}

  NativeXScene Parse() {
    while (current_.kind != Kind::kEnd) {
      if (current_.kind == Kind::kComma || current_.kind == Kind::kSemicolon)
        Pop();
      else if (current_.kind == Kind::kTemplate)
        Definition();
      else
        Object(std::nullopt, 0U);
    }
    NativeXScene result;
    result.nodes.reserve(nodes_.size());
    std::size_t budget = kMaximumNumericBytes;
    for (auto& raw : nodes_) {
      auto& node = raw.value;
      if (!node.reference)
        node.data = NumericData(node.type_name, raw.fields, budget);
      for (const auto& field : raw.fields)
        if (field.kind == Kind::kString)
          node.source_literals.emplace_back(Text(field.raw));
      result.nodes.push_back(std::move(node));
    }
    result.unresolved_references = std::move(ignored_);
    return result;
  }

 private:
  Token Pop() {
    const auto result = current_;
    current_ = lexer_.Next();
    return result;
  }

  Token Pop(Kind expected) {
    Require(current_.kind == expected, "X unexpected grammar token");
    return Pop();
  }

  std::size_t Add(RawNode node) {
    Require(nodes_.size() + ignored_.size() < kMaximumNodes,
            "X node count exceeds bound");
    const auto index = nodes_.size();
    nodes_.push_back(std::move(node));
    return index;
  }

  void Definition() {
    Pop(Kind::kTemplate);
    const std::string name{Text(Pop(Kind::kName).raw)};
    Pop(Kind::kLeftBrace);
    auto identity = Guid(Pop(Kind::kGuid).raw, lexer_.Binary());
    Require(types_.size() < 1024U || types_.contains(name),
            "X template count exceeds bound");
    types_.insert_or_assign(name, std::move(identity));
    std::size_t depth = 1U;
    while (depth) {
      Require(current_.kind != Kind::kEnd,
              "X template definition is truncated");
      if (current_.kind == Kind::kLeftBrace)
        ++depth;
      else if (current_.kind == Kind::kRightBrace)
        --depth;
      Require(depth <= kMaximumDepth, "X template nesting exceeds bound");
      Pop();
    }
  }

  void Reference(std::size_t parent) {
    Pop(Kind::kLeftBrace);
    RawNode raw{};
    raw.value.parent = parent;
    raw.value.reference = true;
    std::optional<std::string> identity;
    if (current_.kind == Kind::kName) {
      raw.value.name = std::string{Text(Pop().raw)};
      Require(!raw.value.name.empty(), "X reference name is empty");
    } else {
      identity = Guid(Pop(Kind::kGuid).raw, lexer_.Binary());
      Require(*identity != "00000000-0000-0000-0000-000000000000",
              "X reference GUID is null");
    }
    Pop(Kind::kRightBrace);
    const auto& registry = identity ? identities_ : names_;
    const auto target = registry.find(identity ? *identity : raw.value.name);
    if (target == registry.end()) {
      Require(nodes_.size() + ignored_.size() < kMaximumNodes,
              "X declaration count exceeds bound");
      ignored_.push_back({.parent = parent,
                          .name = std::move(raw.value.name),
                          .guid = std::move(identity)});
      return;
    }
    raw.value.reference_target = target->second;
    const auto& definition = nodes_[target->second].value;
    raw.value.type_name = definition.type_name;
    raw.value.type_guid = definition.type_guid;
    raw.value.object_guid = definition.object_guid;
    if (identity) raw.value.name = definition.name;
    Add(std::move(raw));
  }

  void Object(std::optional<std::size_t> parent, std::size_t depth) {
    Require(depth < kMaximumDepth, "X object nesting exceeds bound");
    RawNode raw{};
    raw.value.parent = parent;
    raw.value.type_name = std::string{Text(Pop(Kind::kName).raw)};
    const auto type = types_.find(raw.value.type_name);
    Require(type != types_.end(), "X object template has no GUID definition");
    raw.value.type_guid = type->second;
    if (current_.kind == Kind::kName)
      raw.value.name = std::string{Text(Pop().raw)};
    Pop(Kind::kLeftBrace);
    if (current_.kind == Kind::kGuid)
      raw.value.object_guid = Guid(Pop().raw, lexer_.Binary());
    const auto index = Add(std::move(raw));
    while (current_.kind != Kind::kRightBrace) {
      switch (current_.kind) {
        case Kind::kName:
          Object(index, depth + 1U);
          break;
        case Kind::kLeftBrace:
          Reference(index);
          break;
        case Kind::kInt:
        case Kind::kFloat:
        case Kind::kNumber:
        case Kind::kString:
        case Kind::kGuid:
          nodes_[index].fields.push_back(Pop());
          break;
        case Kind::kComma:
        case Kind::kSemicolon:
          Pop();
          break;
        default:
          Invalid("X object is truncated or has unsupported syntax");
      }
    }
    Pop(Kind::kRightBrace);
    if (!parent) {
      for (std::size_t item = index; item < nodes_.size(); ++item) {
        const auto& node = nodes_[item].value;
        if (node.reference) continue;
        if (!node.name.empty()) names_.try_emplace(node.name, item);
        if (node.object_guid) identities_.try_emplace(*node.object_guid, item);
      }
    }
  }

  Lexer lexer_;
  Token current_;
  Types types_{StandardTypes()};
  std::vector<RawNode> nodes_;
  std::map<std::string, std::size_t, std::less<>> names_, identities_;
  std::vector<NativeXUnresolvedReference> ignored_;
};
}  // namespace

NativeXScene ReadNativeX(std::span<const std::uint8_t> bytes) {
  const auto stream = ReadNativeXStream(bytes);
  return Parser{stream}.Parse();
}

const NativeXNode* NativeXScene::FindUniqueMesh(
    std::string_view name) const noexcept {
  const NativeXNode* result = nullptr;
  for (const auto& node : nodes)
    if (!node.reference && node.type_name == "Mesh" && node.name == name) {
      if (result) return nullptr;
      result = &node;
    }
  return result;
}

NativeXMesh GetNativeXMesh(const NativeXScene& scene, std::size_t index) {
  Require(index < scene.nodes.size(), "X mesh node index is out of range");
  const auto* node = &scene.nodes[index];
  if (node->reference) {
    Require(
        node->reference_target && *node->reference_target < scene.nodes.size(),
        "X mesh reference is invalid");
    node = &scene.nodes[*node->reference_target];
  }
  Require(
      !node->reference && node->type_name == "Mesh" && node->data.has_value(),
      "X node is not numeric Mesh data");
  protocol::PacketReader reader{*node->data};
  const auto count = reader.ReadU32();
  Require(count <= reader.RemainingBytes() / 12U,
          "X mesh vertices are truncated");
  NativeXMesh result;
  result.vertices.reserve(count);
  for (std::uint32_t vertex = 0U; vertex < count; ++vertex) {
    std::array<float, 3> position{};
    for (auto& value : position) {
      value = reader.ReadF32();
      Require(std::isfinite(value), "X mesh vertex is nonfinite");
    }
    result.vertices.push_back(position);
  }
  const auto faces = reader.ReadU32();
  Require(faces <= reader.RemainingBytes() / 4U, "X mesh faces are truncated");
  result.faces.reserve(faces);
  for (std::uint32_t face = 0U; face < faces; ++face) {
    const auto width = reader.ReadU32();
    Require(width <= reader.RemainingBytes() / 4U,
            "X mesh polygon is truncated");
    std::vector<std::uint32_t> indices;
    indices.reserve(width);
    for (std::uint32_t vertex = 0U; vertex < width; ++vertex) {
      const auto value = reader.ReadU32();
      Require(value < count, "X mesh index is out of range");
      indices.push_back(value);
    }
    result.faces.push_back(std::move(indices));
  }
  Require(reader.RemainingBytes() == 0U, "X mesh has trailing numeric data");
  return result;
}
}  // namespace bf::runtime
