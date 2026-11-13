#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ITEM_ATTRIBUTES_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ITEM_ATTRIBUTES_H_

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

#include "features/inventory/item_attribute.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {

// Read within the caller's repeatable-read snapshot; never join by template
// alone.
inline std::map<std::uint64_t, std::vector<domain::NativeItemAttribute>>
LoadItemAttributes(bf::infrastructure::mysql::TransactionBase& transaction,
                   std::uint64_t player_id) {
  const auto rows = transaction.Exec(
      "SELECT "
      "a.`ItemInstanceId`,a.`SourceItemId`,a.`DefinitionId`,a.`AttributeValues`,a.`Category`,a.`Label`,i.`ItemId`,i.`Quantity`,i.`ExpiresAt`"
      " IS NULL FROM `PlayerItemAttributes` a JOIN `PlayerItems` i ON "
      "i.`ItemInstanceId`=a.`ItemInstanceId` AND i.`PlayerId`=a.`PlayerId` WHERE a.`PlayerId`=$1 "
      "ORDER BY a.`ItemInstanceId`,a.`AttributeId`",
      bf::infrastructure::mysql::Parameters{transaction, player_id});
  std::map<std::uint64_t, std::vector<domain::NativeItemAttribute>> result;
  for (const auto& row : rows) {
    auto& attributes = result[row[0].As<std::uint64_t>()];
    attributes.push_back(
        {.source_item_id = row[1].As<std::uint32_t>(),
         .definition_id = row[2].As<std::uint32_t>(),
         .values = nlohmann::json::parse(row[3].As<std::string>())
                       .get<std::vector<std::int32_t>>(),
         .category = row[4].As<std::uint32_t>(),
         .label = row[5].As<std::string>()});
    if (row[7].As<std::int64_t>() != 1 || !row[8].As<bool>() ||
        !domain::ValidNativeItemAttributes(row[6].As<std::uint64_t>(),
                                           attributes))
      throw std::runtime_error{
          "stored native item attributes violate inventory shape"};
  }
  return result;
}

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ITEM_ATTRIBUTES_H_
