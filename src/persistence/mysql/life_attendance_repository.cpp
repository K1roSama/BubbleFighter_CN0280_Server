/** @file life_attendance_repository.cpp
 * @brief Atomic attendance and once-only absence debits for original Life
 * items.
 */
#include "persistence/mysql/life_attendance_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
application::ServiceResult<std::string>
MySqlLifeAttendanceStore::JournalEpoch() {
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::ReadTransaction tx{lease.Connection()};
    const auto rows = tx.Exec(
        "SELECT `JournalEpoch` FROM `LifeAccountingEpoch` WHERE `Singleton`");
    if (rows.size() == 1) return rows[0][0].As<std::string>();
  } catch (const std::exception&) {
  }
  return application::ServiceError{
      .code = application::ErrorCode::kStorageUnavailable,
      .safe_message = "Life出席数据库纪元无法读取"};
}

std::optional<application::ServiceError> MySqlLifeAttendanceStore::Record(
    domain::PlayerId player, std::span<const std::chrono::sys_days> days,
    std::chrono::system_clock::time_point now, bool settle) {
  const auto today = application::LifeCalendarDay(now);
  if (!player.IsValid() || days.empty() ||
      std::ranges::any_of(days, [&](auto day) {
        return day > today || day.time_since_epoch().count() < 0;
      }))
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidInput,
        .safe_message = "Life出席日期或玩家无效"};
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    if (tx.Exec(
              "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
              bf::infrastructure::mysql::Parameters{tx, player.Value()})
            .empty())
      return application::ServiceError{
          .code = application::ErrorCode::kPermissionDenied,
          .safe_message = "Life出席玩家不存在"};
    for (const auto day : days)
      static_cast<void>(tx.Exec(
          "INSERT INTO `LifeAttendanceDays`(`PlayerId`,`AttendanceDate`) "
          "VALUES($1,DATE_ADD('1970-01-01',INTERVAL ($2) DAY)) ON CONFLICT DO NOTHING",
          bf::infrastructure::mysql::Parameters{
              tx, player.Value(), day.time_since_epoch().count()}));
    if (settle) {
      const auto rows = tx.Exec(
          "WITH RECURSIVE days(AttendanceDate) AS (SELECT MIN(c.NextDate) FROM LifeItemCursors c JOIN"
          " PlayerItems i USING(ItemInstanceId) WHERE i.PlayerId=$1 AND c.NextDate < "
          "DATE_ADD('1970-01-01',INTERVAL $2 DAY) UNION ALL SELECT DATE_ADD(AttendanceDate,INTERVAL 1"
          " DAY) FROM days WHERE AttendanceDate < DATE_ADD('1970-01-01',INTERVAL ($2-1) DAY)) SELECT "
          "/*+ SET_VAR(cte_max_recursion_depth=65536) */ "
          "i.ItemInstanceId,i.ItemId,i.Quantity,c.NextDate,i.ExpiresAt IS NULL, (SELECT COUNT(*) FROM"
          " days d WHERE d.AttendanceDate >= c.NextDate AND NOT EXISTS(SELECT 1 FROM "
          "LifeAttendanceDays a WHERE a.PlayerId=$1 AND a.AttendanceDate=d.AttendanceDate) AND NOT "
          "EXISTS(SELECT 1 FROM LifeGuardianPeriods g WHERE g.PlayerId=$1 AND g.StartsAt < "
          "CONVERT_TZ(DATE_ADD(d.AttendanceDate,INTERVAL 1 DAY),'+08:00','+00:00') AND g.EndsAt > "
          "CONVERT_TZ(d.AttendanceDate,'+08:00','+00:00') AND g.EndsAt > g.StartsAt)) FROM "
          "PlayerItems i JOIN LifeItemCursors c USING(ItemInstanceId) WHERE i.PlayerId=$1 AND "
          "c.NextDate<DATE_ADD('1970-01-01',INTERVAL $2 DAY) ORDER BY i.ItemId FOR UPDATE OF i,c",
          bf::infrastructure::mysql::Parameters{
              tx, player.Value(), today.time_since_epoch().count()});
      const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                             now.time_since_epoch())
                             .count();
      for (const auto& row : rows) {
        const auto before = row[2].As<int>();
        const auto absent = row[5].As<int>();
        if (!row[4].As<bool>() || before < 0 || before > 15)
          return application::ServiceError{
              .code = application::ErrorCode::kConflict,
              .safe_message = "Life实例数据异常，未执行缺席扣除"};
        const auto debit = std::min(before, absent);
        const auto instance = row[0].As<std::uint64_t>();
        const auto item = row[1].As<std::uint64_t>();
        const auto business = tx.Exec("SELECT UUID()")[0][0].As<std::string>();
        if (debit > 0) {
          static_cast<void>(tx.Exec(
              "UPDATE `PlayerItems` SET `Quantity`=`Quantity`-$2 WHERE `ItemInstanceId`=$1",
              bf::infrastructure::mysql::Parameters{tx, instance, debit}));
          static_cast<void>(tx.Exec(
              "INSERT INTO "
              "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
              " VALUES($1,$2,$3,$4,'life_absence',$5,FROM_UNIXTIME($6/1000000.0))",
              bf::infrastructure::mysql::Parameters{tx, player.Value(), item,
                                                    instance, -debit, business,
                                                    stamp}));
        }
        static_cast<void>(tx.Exec(
            "INSERT INTO "
            "`LifeItemSettlements`(`ItemInstanceId`,`PlayerId`,`ItemId`,`FirstDate`,`ThroughDate`,`AbsentDays`,`LifeBefore`,`LifeAfter`,`BusinessId`,`SettledAt`)"
            " VALUES($1,$2,$3,$4,DATE_ADD('1970-01-01',INTERVAL ($5-1) "
            "DAY),$6,$7,$8,$9,FROM_UNIXTIME($10/1000000.0))",
            bf::infrastructure::mysql::Parameters{
                tx, instance, player.Value(), item, row[3].As<std::string>(),
                today.time_since_epoch().count(), absent, before,
                before - debit, business, stamp}));
        static_cast<void>(tx.Exec(
            "UPDATE `LifeItemCursors` SET `NextDate`=DATE_ADD('1970-01-01',INTERVAL ($2) DAY) WHERE "
            "`ItemInstanceId`=$1",
            bf::infrastructure::mysql::Parameters{
                tx, instance, today.time_since_epoch().count()}));
      }
    }
    tx.Commit();
    return std::nullopt;
  } catch (const std::exception&) {
    spdlog::warn(
        "Life attendance transaction unconfirmed; login must not expose unsettled inventory");
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "Life出席结算暂不可用，请重新登录",
        .is_retryable = true};
  }
}
}  // namespace bf::infrastructure::mysql
