#pragma once
#include <cstdint>
namespace lab_siren {
struct Identity {
    std::uintptr_t owner{}, pool{};
    std::uint32_t slot{}, reference{};
    constexpr bool operator==(const Identity&) const = default;
};
enum class Decision { Wait, Bind, Keep, Retire };
// Position/color/intensity are deliberately not ownership: one private key is
// reserved for one validated vehicle lifetime and is never rebound this launch.
constexpr Decision Decide(bool bound, bool retired, bool requested, bool valid,
                          Identity expected, Identity observed) noexcept {
    if (retired) return Decision::Retire;
    if (!valid) return bound ? Decision::Retire : Decision::Wait;
    if (bound) return expected == observed ? Decision::Keep : Decision::Retire;
    return requested ? Decision::Bind : Decision::Wait;
}
constexpr std::uint64_t Generation(bool privatePoint, std::uint64_t geometry) noexcept {
    return privatePoint ? 1ull : geometry;
}
}
