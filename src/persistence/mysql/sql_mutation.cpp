#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <utility>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {
struct TableKey {
  std::string_view table;
  std::string_view columns;
  std::string_view identity;
};

constexpr TableKey kTableKeys[] = {
#include "persistence/mysql/generated/table_keys.inc"
};

std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool PrefixEqual(std::string_view text, std::string_view keyword) {
  if (text.size() < keyword.size()) {
    return false;
  }
  return std::equal(keyword.begin(), keyword.end(), text.begin(),
                    [](char a, char b) {
                      return a == std::toupper(static_cast<unsigned char>(b));
                    });
}

std::size_t FindSql(std::string_view text, std::string_view keyword,
                    std::size_t start = 0) {
  unsigned depth = 0;
  char quote = 0;
  for (std::size_t i = start; i < text.size(); ++i) {
    const char token = text[i];
    if (quote) {
      if (token == quote && i + 1 < text.size() && text[i + 1] == quote) {
        ++i;
      } else if (token == quote) {
        quote = 0;
      }
      continue;
    }
    if (depth == 0 && PrefixEqual(text.substr(i), keyword)) {
      return i;
    }
    if (token == '\'' || token == '"' || token == 96) {
      quote = token;
    } else if (token == '(') {
      ++depth;
    } else if (token == ')' && depth > 0) {
      --depth;
    }
  }
  return std::string_view::npos;
}

std::vector<std::string_view> SplitSql(std::string_view list) {
  std::vector<std::string_view> values;
  while (!list.empty()) {
    const auto comma = FindSql(list, ",");
    values.push_back(Trim(list.substr(0, comma)));
    if (comma == std::string_view::npos) {
      break;
    }
    list.remove_prefix(comma + 1);
  }
  return values;
}

std::string_view Group(std::string_view expression) {
  expression = Trim(expression);
  if (expression.empty() || expression.front() != '(') {
    throw ConversionError("Expected a SQL value list");
  }
  expression.remove_prefix(1);
  const auto end = FindSql(expression, ")");
  if (end == std::string_view::npos) {
    throw ConversionError("Unclosed SQL value list");
  }
  return expression.substr(0, end);
}

std::string_view Unquote(std::string_view name) {
  name = Trim(name);
  if (name.size() >= 2 && name.front() == 96 && name.back() == 96) {
    name.remove_prefix(1);
    name.remove_suffix(1);
  }
  return name;
}

std::string Quote(std::string_view name) {
  return char(96) + std::string(Unquote(name)) + char(96);
}

std::string_view Identifier(std::string_view source) {
  source = Trim(source);
  if (source.empty()) {
    return {};
  }
  if (source.front() == 96) {
    const auto close = source.find(char(96), 1);
    if (close == std::string_view::npos) {
      throw ConversionError("Unclosed MySQL identifier");
    }
    return source.substr(0, close + 1);
  }
  return source.substr(0, source.find_first_of(" \t\r\n(,"));
}

const TableKey& FindKey(std::string_view name) {
  const auto found =
      std::ranges::find(kTableKeys, Unquote(name), &TableKey::table);
  if (found == std::end(kTableKeys)) {
    throw ConversionError("Returning requires a registered MySQL primary key");
  }
  return *found;
}

std::string FilterByKey(std::string_view qualifier,
                        const std::vector<std::string_view>& columns,
                        const Row& row, Parameters& parameters) {
  std::string filter;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i != 0) {
      filter += " AND ";
    }
    parameters.Append(row[i].As<std::string>());
    filter += std::string(qualifier) + "." + Quote(columns[i]) + "=$" +
              std::to_string(parameters.Values().size());
  }
  return filter;
}

std::string KeyExpressions(std::string_view qualifier,
                           const std::vector<std::string_view>& columns) {
  std::string list;
  for (auto column : columns) {
    if (!list.empty()) {
      list += ", ";
    }
    list += std::string(qualifier) + "." + Quote(column);
  }
  return list;
}

std::string FindInsertKey(std::string_view statement, std::string_view table,
                          const std::vector<std::string_view>& key_columns) {
  const auto rest =
      Trim(statement.substr(statement.find(table) + table.size()));
  const auto columns = SplitSql(Group(rest));
  const auto values_at = FindSql(rest, " VALUES");
  if (values_at == std::string_view::npos) {
    throw ConversionError(
        "Returning without an identity requires INSERT VALUES");
  }
  const auto values = SplitSql(Group(rest.substr(values_at + 7)));
  if (columns.size() != values.size()) {
    throw ConversionError("MySQL insert column/value lists differ");
  }
  std::string predicate;
  for (auto key : key_columns) {
    const auto found = std::ranges::find_if(columns, [key](auto candidate) {
      return Unquote(key) == Unquote(candidate);
    });
    if (found == columns.end()) {
      throw ConversionError(
          "Returning requires the primary key in INSERT VALUES");
    }
    if (!predicate.empty()) {
      predicate += " AND ";
    }
    predicate +=
        Quote(key) + "=(" + std::string(values[found - columns.begin()]) + ")";
  }
  return predicate;
}

QueryResult Insert(Connection& connection, std::string_view statement,
                   std::string_view projection, const Parameters& params) {
  const auto name = Identifier(statement.substr(12));
  const auto conflict = FindSql(statement, " ON CONFLICT");
  const auto input = Trim(statement.substr(0, conflict));
  const auto action = conflict == std::string_view::npos
                          ? std::string_view{}
                          : Trim(statement.substr(conflict + 12));
  const auto ignore = FindSql(action, "DO NOTHING");
  const bool skip_duplicate = ignore != std::string_view::npos;
  const auto update = FindSql(action, "DO UPDATE SET ");
  if (!action.empty() && !skip_duplicate && update == std::string_view::npos) {
    throw ConversionError("Unsupported MySQL conflict operation");
  }
  std::string mysql_statement(input);
  if (update != std::string_view::npos) {
    mysql_statement += " ON DUPLICATE KEY UPDATE ";
    mysql_statement += action.substr(update + 14);
    if (!projection.empty()) {
      const auto& key = FindKey(name);
      if (!key.identity.empty()) {
        const auto id = Quote(key.identity);
        mysql_statement += ", " + id + "=LAST_INSERT_ID(" + id + ")";
      }
    }
  }
  QueryResult write;
  try {
    write = connection.Execute(mysql_statement, params);
  } catch (const UniqueViolation&) {
    if (!skip_duplicate) {
      throw;
    }
    if (action.front() == '(') {
      const auto target = Group(action);
      const auto predicate =
          Trim(action.substr(target.size() + 2, ignore - target.size() - 2));
      auto filter = FindInsertKey(input, name, SplitSql(target));
      if (!predicate.empty()) {
        if (!PrefixEqual(predicate, "WHERE ")) {
          throw ConversionError("Unsupported MySQL conflict predicate");
        }
        filter += " AND (" + std::string(predicate.substr(6)) + ")";
      }
      if (connection
              .Execute("SELECT 1 FROM " + std::string(name) + " WHERE " +
                           filter + " FOR UPDATE",
                       params)
              .empty()) {
        throw;
      }
    }
    return {};
  }
  if (projection.empty() || write.AffectedRows() == 0) {
    return write;
  }
  if (projection == "1") {
    return connection.Execute("SELECT 1");
  }
  const auto& key = FindKey(name);
  Parameters returned(params);
  std::string filter;
  if (!key.identity.empty()) {
    if (write.InsertId() == 0) {
      throw ConversionError("MySQL did not return the inserted identity");
    }
    returned.Append(write.InsertId());
    filter =
        Quote(key.identity) + "=$" + std::to_string(returned.Values().size());
  } else {
    filter = FindInsertKey(input, name, SplitSql(key.columns));
  }
  return connection.Execute("SELECT " + std::string(projection) + " FROM " +
                                std::string(name) + " WHERE " + filter,
                            returned);
}

QueryResult Update(Connection& connection, std::string_view statement,
                   std::string_view projection, const Parameters& params) {
  const auto name = Identifier(statement.substr(7));
  const auto before_set = FindSql(statement, " SET ");
  const auto before_where = FindSql(statement, " WHERE ");
  if (before_set == std::string_view::npos ||
      before_where == std::string_view::npos) {
    throw ConversionError("Returning update requires SET and WHERE clauses");
  }
  const auto relations = Trim(statement.substr(7, before_set - 7));
  const auto target = Trim(relations.substr(0, FindSql(relations, ",")));
  auto alias = Trim(target.substr(name.size()));
  if (PrefixEqual(alias, "AS ")) {
    alias.remove_prefix(3);
  }
  const auto qualifier = alias.empty() ? name : Identifier(alias);
  const auto& key = FindKey(name);
  const auto columns = SplitSql(key.columns);
  const auto selected = connection.Execute(
      "SELECT " + KeyExpressions(qualifier, columns) + " FROM " +
          std::string(relations) + std::string(statement.substr(before_where)) +
          " FOR UPDATE",
      params);
  if (selected.empty()) {
    return selected;
  }
  if (selected.size() != 1) {
    throw ConversionError("Returning update must address one MySQL row");
  }
  const auto write = connection.Execute(statement, params);
  if (write.AffectedRows() == 0) {
    return write;
  }
  Parameters returned(params);
  const auto filter = FilterByKey(qualifier, columns, selected[0], returned);
  return connection.Execute("SELECT " + std::string(projection) + " FROM " +
                                std::string(target) + " WHERE " + filter,
                            returned);
}
}  // namespace

QueryResult TransactionBase::Exec(std::string_view query,
                                  const Parameters& params) {
  query = Trim(query);
  const auto returning = FindSql(query, " RETURNING ");
  const auto statement = Trim(query.substr(0, returning));
  const auto projection = returning == std::string_view::npos
                              ? std::string_view{}
                              : Trim(query.substr(returning + 11));
  if (PrefixEqual(statement, "INSERT INTO ") &&
      (returning != std::string_view::npos ||
       FindSql(statement, " ON CONFLICT") != std::string_view::npos)) {
    if (!active) {
      throw ConversionError("A MySQL write transaction is required");
    }
    return Insert(connection, statement, projection, params);
  }
  if (returning != std::string_view::npos) {
    if (!active || !PrefixEqual(statement, "UPDATE ")) {
      throw ConversionError("Unsupported MySQL returning operation");
    }
    return Update(connection, statement, projection, params);
  }
  return connection.Execute(query, params);
}

}  // namespace bf::infrastructure::mysql
