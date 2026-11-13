#include "application/identifier.h"

namespace bf::domain {

std::string ToString(AccountId value) { return std::to_string(value.Value()); }

std::string ToString(PlayerId value) { return std::to_string(value.Value()); }

std::string ToString(RoomId value) { return std::to_string(value.Value()); }

}  // namespace bf::domain
