#pragma once
#include <cstdint>
namespace mt {
// Shared by clipboard paste, gesture actions and mouse replay.
inline constexpr std::uintptr_t ownInputTag = 0x544F4F4C;
}
