#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SCENES_HIDE_SEEK_SCENE_ASSETS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SCENES_HIDE_SEEK_SCENE_ASSETS_H_

#include <filesystem>
#include <memory>

#include "features/battle/hide_seek_match.h"

namespace bf::runtime {
/** Exact source meshes and native WB spawn coordinates. Does not imply visual
 * approval. Development explicitly opts into an unreviewed profile; production
 * requires the receipt. */
[[nodiscard]] std::shared_ptr<const HideSeekScene> LoadHideSeekSceneAssets(
    const std::filesystem::path& directory, bool development = false);
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SCENES_HIDE_SEEK_SCENE_ASSETS_H_
