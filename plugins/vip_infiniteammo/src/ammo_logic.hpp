#pragma once
// Engine-free logic of the VIP_InfiniteAmmo port: the pack config model and the reserve-ammo
// state machine of the original ProcessPlayerWeapons().
//
// The state machine (the original's per-weapon rules, "expected" = iExpectedReserve):
//   current > target        -> clamp to target, expected = target
//   expected < 0 (first)    -> set target, expected = target
//   current == expected - 1 -> the player fired one round: expected = current, change nothing
//   current < expected - 1  -> restore to max(expected - 1, 0), expected = that
//   otherwise               -> expected = current (quiet resync)

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>

namespace vip_infiniteammo {

struct AmmoPack {
	std::string name;
	int defaultReserve = 0;
	std::map<std::string, int> weaponOverrides;
};

// Address of the weapon's reserve-ammo array (two inline int32s despite the m_p name).
//
// CBasePlayerWeapon::m_pReserveAmmo is declared `int32 m_pReserveAmmo[2]` in the game schema
// (kind "fixed_array", element_count 2, size 8, offset 4344) - the field stores the two ints
// INLINE. The original's field-pointer accessor returned the ADDRESS of the field (base plus
// offset); it never dereferenced a stored pointer. Reinterpreting the two ammo ints as a pointer
// and dereferencing them faults the moment the value is read.
inline std::int32_t* ReserveAmmoAt(void* weapon, int offset) {
	return reinterpret_cast<std::int32_t*>(static_cast<char*>(weapon) + offset);
}

// Reserve target of one weapon classname inside a pack.
inline int TargetReserve(const AmmoPack& pack, const std::string& weaponName) {
	if (const auto it = pack.weaponOverrides.find(weaponName); it != pack.weaponOverrides.end()) {
		return it->second;
	}
	return pack.defaultReserve;
}

struct ReserveDecision {
	bool set = false;  // write `value` into m_pReserveAmmo[0]
	int value = 0;     // value to write (when set)
	int expected = 0;  // new iExpectedReserve (also updated when nothing is written)
};

inline ReserveDecision DecideReserve(int current, int expected, int target) {
	if (current > target) {
		return {true, target, target};
	}
	if (expected < 0) {
		return {true, target, target};
	}
	if (current == expected - 1) {
		return {false, current, current};
	}
	if (current < expected - 1) {
		const int restored = std::max(expected - 1, 0);
		return {true, restored, restored};
	}
	return {false, current, current};
}

} // namespace vip_infiniteammo
