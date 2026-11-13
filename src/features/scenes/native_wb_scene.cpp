#include "features/scenes/native_wb_scene.h"

#include <cmath>
#include <utility>

#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
constexpr std::size_t kMaximumBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumRecords = 131072U;
constexpr std::uint32_t kMaximumSubsets = 65536U;
constexpr std::uint32_t kMaximumKeys = 1048576U;

[[noreturn]] void Invalid(std::string_view message) {
  throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                message};
}

std::string Text(protocol::PacketReader& reader) {
  const auto bytes = reader.ReadBytes(reader.ReadU8());
  return {bytes.begin(), bytes.end()};
}

float Scalar(protocol::PacketReader& reader) {
  const auto value = reader.ReadF32();
  if (!std::isfinite(value)) Invalid("WB scalar is nonfinite");
  return value;
}

template <std::size_t Size>
std::array<float, Size> Scalars(protocol::PacketReader& reader) {
  std::array<float, Size> result{};
  for (auto& value : result) value = Scalar(reader);
  return result;
}

NativeWbMaterial Material(protocol::PacketReader& reader,
                          std::uint32_t& remaining_keys) {
  NativeWbMaterial result{.texture = Text(reader), .data = std::nullopt};
  if (result.texture == "No Texture") return result;
  NativeWbMaterialData data{};
  data.subset = reader.ReadU32();
  data.name = Text(reader);
  data.parameters = Scalars<9>(reader);
  data.properties = Text(reader);
  data.resource_flag = reader.ReadU32();
  if (data.resource_flag == 1U) data.resource = Text(reader);
  for (auto& flag : data.flags) flag = reader.ReadU32();
  for (auto& value : data.extra_values) value = reader.ReadU32();
  data.initial_value = Scalar(reader);
  for (auto& track : data.tracks) {
    const auto count = reader.ReadCount(remaining_keys);
    if (!count) continue;
    remaining_keys -= count;
    track.interpolation = reader.ReadU32();
    const auto width = *track.interpolation == 0U ? 16U : 8U;
    if (count > reader.RemainingBytes() / width)
      throw protocol::ProtocolError{
          protocol::ProtocolErrorCode::kTruncatedPayload,
          "WB key track is truncated"};
    track.keys.reserve(count);
    for (std::uint32_t index = 0U; index < count; ++index) {
      NativeWbFloatKey key{.time = Scalar(reader),
                           .value = Scalar(reader),
                           .bezier = std::nullopt};
      if (*track.interpolation == 0U) key.bezier = Scalars<2>(reader);
      track.keys.push_back(key);
    }
  }
  result.data = std::move(data);
  return result;
}
}  // namespace

const NativeWbDummy* NativeWbScene::FindDummy(
    std::string_view name) const noexcept {
  for (auto record = records.rbegin(); record != records.rend(); ++record)
    if (const auto* dummy = std::get_if<NativeWbDummy>(&record->value);
        dummy && dummy->name == name)
      return dummy;
  return nullptr;
}

NativeWbScene ReadNativeWb(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaximumBytes)
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kLengthLimit,
                                  "WB file exceeds resource limit"};
  protocol::PacketReader reader{bytes};
  if (reader.ReadU32() != 2U) Invalid("unsupported WB version");
  NativeWbScene result{.header_value = Scalar(reader), .records = {}};
  if (!reader.RemainingBytes()) Invalid("WB file has no records");
  auto remaining_keys = kMaximumKeys;
  while (reader.RemainingBytes()) {
    if (result.records.size() == kMaximumRecords)
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kLengthLimit,
                                    "WB record count exceeds resource limit"};
    const auto offset = bytes.size() - reader.RemainingBytes();
    auto name = Text(reader);
    const auto kind = reader.ReadU32();
    if (kind > 1U) Invalid("unsupported WB record kind");
    auto properties = Text(reader);
    if (kind == 1U) {
      const auto pose_offset = bytes.size() - reader.RemainingBytes();
      auto pose = Scalars<6>(reader);
      result.records.push_back(
          {.offset = offset,
           .end = bytes.size() - reader.RemainingBytes(),
           .value = NativeWbDummy{.name = std::move(name),
                                  .properties = std::move(properties),
                                  .pose = pose,
                                  .pose_offset = pose_offset}});
    } else {
      NativeWbMesh mesh{.name = std::move(name),
                        .properties = std::move(properties),
                        .parameters = Scalars<3>(reader),
                        .subsets = {},
                        .materials = {}};
      const auto count = reader.ReadCount(kMaximumSubsets);
      if (count > reader.RemainingBytes() / 5U)
        throw protocol::ProtocolError{
            protocol::ProtocolErrorCode::kTruncatedPayload,
            "WB subset array is truncated"};
      mesh.subsets.reserve(count);
      for (std::uint32_t index = 0U; index < count; ++index)
        mesh.subsets.push_back(reader.ReadU32());
      mesh.materials.reserve(count);
      for (std::uint32_t index = 0U; index < count; ++index)
        mesh.materials.push_back(Material(reader, remaining_keys));
      result.records.push_back({.offset = offset,
                                .end = bytes.size() - reader.RemainingBytes(),
                                .value = std::move(mesh)});
    }
  }
  return result;
}
}  // namespace bf::runtime
