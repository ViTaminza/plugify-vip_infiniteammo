// vip_infiniteammo unit tests — the engine-independent parts: the shipped pack model ("default"
// plus per-weapon magazine counts) and the target computation of the partiusfabaa scheme
// (N magazines -> N * m_iMaxClip1 reserve rounds; N >= 999 -> infinite clip; N <= 0 -> excluded).
//
// The config *file* parsing needs pisex::KeyValues (and therefore the bindings); it is covered by
// a config load on a running server, this test drives the logic with the shipped pack values
// (ref/prod-27016/addons/configs/vip/vip_infiniteammo.ini).
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "ammo_logic.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using vip_infiniteammo::AmmoMode;
using vip_infiniteammo::AmmoPack;
using vip_infiniteammo::AmmoPlan;
using vip_infiniteammo::PlanAmmo;
using vip_infiniteammo::ReserveAmmoAt;
using vip_infiniteammo::TargetMagazines;

int g_checks = 0;
int g_failures = 0;

void CheckEq(int actual, int expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected, actual);
	}
}

void CheckEq(bool actual, bool expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected ? 1 : 0, actual ? 1 : 0);
	}
}

const char* ModeName(AmmoMode mode) {
	switch (mode) {
	case AmmoMode::Excluded:
		return "Excluded";
	case AmmoMode::InfiniteClip:
		return "InfiniteClip";
	case AmmoMode::Reserve:
		return "Reserve";
	}
	return "?";
}

void CheckMode(const AmmoPlan& plan, AmmoMode expected, const std::string& what) {
	++g_checks;
	if (plan.mode != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %s, got %s)\n", what.c_str(), ModeName(expected), ModeName(plan.mode));
	}
}

// The shipped "premium" pack: default 6 magazines, the M4A1 gets 8, the AWP 4.
AmmoPack PremiumPack() {
	AmmoPack pack;
	pack.name = "premium";
	pack.defaultMagazines = 6;
	pack.weaponOverrides["weapon_m4a1"] = 8;
	pack.weaponOverrides["weapon_awp"] = 4;
	return pack;
}

// The shipped "god" pack: the default is 999 magazines ("бесконечные обоймы").
AmmoPack GodPack() {
	AmmoPack pack;
	pack.name = "god";
	pack.defaultMagazines = 999;
	return pack;
}

void TestTargetMagazines() {
	const AmmoPack pack = PremiumPack();
	CheckEq(TargetMagazines(pack, "weapon_ak47"), 6, "the pack default");
	CheckEq(TargetMagazines(pack, "weapon_m4a1"), 8, "the per-weapon override");
	CheckEq(TargetMagazines(pack, "weapon_awp"), 4, "a second override");
	CheckEq(TargetMagazines(pack, "weapon_smokegrenade"), 6, "an unlisted weapon uses the default");

	// N <= 0 for one weapon excludes just that weapon (the format's replacement for DisabledGuns).
	AmmoPack mixed;
	mixed.defaultMagazines = 3;
	mixed.weaponOverrides["weapon_knife"] = 0;
	mixed.weaponOverrides["weapon_bayonet"] = -1;
	CheckEq(TargetMagazines(mixed, "weapon_knife"), 0, "a 0 override excludes the weapon");
	CheckEq(TargetMagazines(mixed, "weapon_bayonet"), -1, "a negative override excludes the weapon");
	CheckEq(TargetMagazines(mixed, "weapon_ak47"), 3, "the default still applies to the others");
}

void TestReserveTarget() {
	// N magazines of m_iMaxClip1 rounds each.
	CheckMode(PlanAmmo(6, 30), AmmoMode::Reserve, "6 magazines is reserve mode");
	CheckEq(PlanAmmo(6, 30).value, 180, "6 * 30 = 180 reserve rounds");
	CheckEq(PlanAmmo(3, 20).value, 60, "3 * 20 = 60 reserve rounds");
	CheckEq(PlanAmmo(2, 5).value, 10, "2 * 5 = 10 reserve rounds");
	CheckEq(PlanAmmo(1, 1).value, 1, "1 * 1 = 1 reserve round");

	// The shipped premium pack, as the config loader would build it.
	const AmmoPack premium = PremiumPack();
	CheckEq(PlanAmmo(premium, "weapon_ak47", 30).value, 180, "premium ak47: 6 * 30");
	CheckEq(PlanAmmo(premium, "weapon_m4a1", 30).value, 240, "premium m4a1 override: 8 * 30");
	CheckEq(PlanAmmo(premium, "weapon_awp", 5).value, 20, "premium awp override: 4 * 5");
}

void TestInfiniteClip() {
	// N >= 999: the reference's Type 1 - the clip is refilled to the weapon's max clip.
	const AmmoPlan god = PlanAmmo(999, 30);
	CheckMode(god, AmmoMode::InfiniteClip, "999 magazines is infinite clip mode");
	CheckEq(god.value, 30, "infinite clip fills to the weapon's max clip");
	CheckEq(PlanAmmo(1000, 13).value, 13, "1000 magazines fills to the max clip");
	CheckEq(PlanAmmo(GodPack(), "weapon_nova", 8).value, 8, "the shipped god pack -> infinite clip");

	// The boundary: 998 is still a finite reserve target, 999 flips to infinite clip.
	CheckMode(PlanAmmo(998, 1), AmmoMode::Reserve, "998 magazines is still reserve mode");
	CheckEq(PlanAmmo(998, 1).value, 998, "998 * 1");
	CheckMode(PlanAmmo(999, 1), AmmoMode::InfiniteClip, "999 magazines flips to infinite clip");
}

void TestExcluded() {
	CheckMode(PlanAmmo(0, 30), AmmoMode::Excluded, "N = 0 excludes the weapon");
	CheckMode(PlanAmmo(-5, 30), AmmoMode::Excluded, "N < 0 excludes the weapon");
	CheckMode(PlanAmmo(6, 0), AmmoMode::Excluded, "a non-firearm (max clip 0) is excluded");
	CheckMode(PlanAmmo(6, -1), AmmoMode::Excluded, "an invalid max clip is excluded");
	CheckMode(PlanAmmo(PremiumPack(), "weapon_smokegrenade", 0), AmmoMode::Excluded,
			  "an unlisted grenade with the default pack is still not a firearm");

	// A pack that excludes by default but grants one weapon.
	AmmoPack whitelist;
	whitelist.defaultMagazines = 0;
	whitelist.weaponOverrides["weapon_ak47"] = 4;
	CheckMode(PlanAmmo(whitelist, "weapon_glock", 20), AmmoMode::Excluded, "the pack default excludes");
	CheckEq(PlanAmmo(whitelist, "weapon_ak47", 30).value, 120, "the listed weapon gets 4 * 30");
}

void TestReserveAmmoAt() {
	// CBasePlayerWeapon::m_pReserveAmmo is an INLINE int32[2] (game schema dump: kind
	// "fixed_array", element_count 2, size 8, offset 4344). The fake weapon buffer proves the
	// accessor points AT the inline ints instead of dereferencing them as a stored pointer: the
	// old code read the two seeded ints as an address and would fault right here (the derived
	// "pointer" 0x70000000d is unmapped), where the fixed code reads and writes the bytes.
	constexpr int kOffset = 4344;
	alignas(8) unsigned char weapon[4360] = {};

	int32_t* reserve = ReserveAmmoAt(weapon, kOffset);
	CheckEq(reinterpret_cast<void*>(reserve) == static_cast<void*>(weapon + kOffset), true,
			"ReserveAmmoAt returns the address of the inline field");

	// The two elements of the array, [0..1], are the ints stored inline at the field offset.
	const int32_t seed[2] = {13, 7};
	std::memcpy(weapon + kOffset, seed, sizeof(seed));
	CheckEq(reserve[0], 13, "reserve[0] reads the first inline int");
	CheckEq(reserve[1], 7, "reserve[1] reads the second inline int");

	// A write through the accessor changes the inline bytes - and nothing outside the 8-byte array.
	weapon[kOffset + 8] = 0x5A;
	weapon[kOffset - 1] = 0xA5;
	reserve[0] = 6;
	int32_t readBack = 0;
	std::memcpy(&readBack, weapon + kOffset, sizeof(readBack));
	CheckEq(readBack, 6, "reserve[0] writes the first inline int");
	CheckEq(weapon[kOffset + 8], 0x5A, "the write stays inside the array (after)");
	CheckEq(weapon[kOffset - 1], 0xA5, "the write stays inside the array (before)");
}

} // namespace

int main() {
	std::printf("vip_infiniteammo tests\n");
	TestTargetMagazines();
	TestReserveTarget();
	TestInfiniteClip();
	TestExcluded();
	TestReserveAmmoAt();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
