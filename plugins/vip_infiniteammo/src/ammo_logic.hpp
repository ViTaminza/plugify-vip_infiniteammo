#pragma once
// Engine-free logic of the vip_infiniteammo rewrite: the pack config model and the target
// computation of the partiusfabaa module (ref/src/partius-vip-infiniteammo/VIP_InfiniteAmmo.cs).
//
// The production config (<configs>/vip_infiniteammo/vip_infiniteammo.ini) keeps its format: the
// "InfiniteReserveAmmo" section holds one subkey per pack, each with "default" and per-weapon
// values. The values are numbers of MAGAZINES in the reserve, as the shipped file says
// ("Значения - количество ОБОЙМ в резерве", "Пак god - бесконечные обоймы"):
//
//   N <= 0        -> the weapon is excluded (this replaces the reference's DisabledGuns list;
//                    an absent per-weapon key falls back to the pack's "default")
//   N >= 999      -> infinite CLIP mode (the reference's Type 1): m_iClip1 is kept at the weapon's
//                    VData m_iMaxClip1
//   0 < N < 999   -> reserve mode (the reference's Type 2): m_pReserveAmmo[0] is kept at
//                    N * m_iMaxClip1 rounds, i.e. N magazines.
//
// Only firearms (m_iMaxClip1 > 0) are ever touched; the caller filters them through the VData
// lookup. Everything here is pure so it is unit-testable (tests/vip_infiniteammo_tests.cpp).

#include <cstdint>
#include <limits>
#include <map>
#include <string>

namespace vip_infiniteammo {

struct AmmoPack {
	std::string name;
	int defaultMagazines = 0; // magazines granted to a weapon the pack does not list
	std::map<std::string, int> weaponOverrides;
};

// Address of the weapon's reserve-ammo array (two inline int32s despite the m_p name).
//
// CBasePlayerWeapon::m_pReserveAmmo is declared `int32 m_pReserveAmmo[2]` in the game schema
// (kind "fixed_array", element_count 2, size 8, offset 4344) - the field stores the two ints
// INLINE. The original's field-pointer accessor returned the ADDRESS of the field (base plus
// offset); it never dereferenced a stored pointer. Reinterpreting the two ammo ints as a pointer
// and dereferencing them faults the moment the value is read. The plugin writes the field through
// the handle-based s2sdk API at this same offset (SetEntData/GetEntData); this helper keeps the
// inline layout explicit and is covered by the unit test.
inline std::int32_t* ReserveAmmoAt(void* weapon, int offset) {
	return reinterpret_cast<std::int32_t*>(static_cast<char*>(weapon) + offset);
}

// Magazines the pack grants to one weapon classname: the per-weapon value, else the pack default.
inline int TargetMagazines(const AmmoPack& pack, const std::string& weaponName) {
	if (const auto it = pack.weaponOverrides.find(weaponName); it != pack.weaponOverrides.end()) {
		return it->second;
	}
	return pack.defaultMagazines;
}

// "god" and anything above it: the pack means "infinite magazines", implemented as an infinite
// clip (the reference's Type 1).
inline constexpr int kInfiniteClipMagazines = 999;

enum class AmmoMode {
	Excluded,	  // not a firearm, or N <= 0: leave the weapon alone
	InfiniteClip, // N >= 999: keep m_iClip1 at the weapon's max clip
	Reserve,	  // 0 < N < 999: keep m_pReserveAmmo[0] at N * max clip
};

struct AmmoPlan {
	AmmoMode mode = AmmoMode::Excluded;
	// The value to write: m_iClip1 for InfiniteClip, m_pReserveAmmo[0] for Reserve.
	int value = 0;
};

// The pure target computation. maxClip is the weapon's VData m_iMaxClip1 (the caller reads it;
// only firearms with maxClip > 0 reach here, the guard is repeated for safety).
inline AmmoPlan PlanAmmo(int magazines, int maxClip) {
	if (maxClip <= 0) {
		return {}; // not a firearm
	}
	if (magazines <= 0) {
		return {}; // excluded weapon (N <= 0)
	}
	if (magazines >= kInfiniteClipMagazines) {
		return {AmmoMode::InfiniteClip, maxClip};
	}
	// 0 < magazines < 999 and a sane max clip: the product fits an int by a wide margin. The
	// clamp only guards against a corrupted VData read (the engine's values are far below it).
	const long long reserve = static_cast<long long>(magazines) * maxClip;
	const int value = reserve > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
															   : static_cast<int>(reserve);
	return {AmmoMode::Reserve, value};
}

inline AmmoPlan PlanAmmo(const AmmoPack& pack, const std::string& weaponName, int maxClip) {
	return PlanAmmo(TargetMagazines(pack, weaponName), maxClip);
}

} // namespace vip_infiniteammo
