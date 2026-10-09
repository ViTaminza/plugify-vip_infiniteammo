// vip_infiniteammo unit tests — the engine-independent parts: the pack model (default plus
// per-weapon overrides, like the production vip_infiniteammo.ini) and the reserve-ammo state
// machine of the original ProcessPlayerWeapons().
//
// The config *file* parsing needs pisex::KeyValues (and therefore the bindings); it is covered by
// the live-server config load, this test drives the logic with the production pack values.
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "ammo_logic.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using vip_infiniteammo::AmmoPack;
using vip_infiniteammo::DecideReserve;
using vip_infiniteammo::ReserveAmmoAt;
using vip_infiniteammo::TargetReserve;

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

// The production "premium" pack: default 6, an override for the M4A1 (8) and the AWP (4).
AmmoPack PremiumPack() {
	AmmoPack pack;
	pack.name = "premium";
	pack.defaultReserve = 6;
	pack.weaponOverrides["weapon_m4a1"] = 8;
	pack.weaponOverrides["weapon_awp"] = 4;
	return pack;
}

void TestTargetReserve() {
	const AmmoPack pack = PremiumPack();
	CheckEq(TargetReserve(pack, "weapon_ak47"), 6, "the pack default");
	CheckEq(TargetReserve(pack, "weapon_m4a1"), 8, "the per-weapon override");
	CheckEq(TargetReserve(pack, "weapon_awp"), 4, "a second override");
	CheckEq(TargetReserve(pack, "weapon_smokegrenade"), 6, "grenades use the default too (the caller filters)");

	AmmoPack god;
	god.name = "god";
	god.defaultReserve = 999;
	CheckEq(TargetReserve(god, "weapon_nova"), 999, "the god pack");
}

void TestFirstContact() {
	// expected < 0 = the weapon was never seen: it is snapped to the target.
	const auto first = DecideReserve(13, -1, 3);
	CheckEq(first.set, true, "first contact writes");
	CheckEq(first.value, 3, "first contact writes the target");
	CheckEq(first.expected, 3, "first contact expects the target");
}

void TestClampDown() {
	// The player carries more than the pack allows (bought before becoming a VIP, or another
	// pack): clamp down to the target.
	const auto clamp = DecideReserve(10, 10, 3);
	CheckEq(clamp.set, true, "above the target is clamped");
	CheckEq(clamp.value, 3, "clamped to the target");
	CheckEq(clamp.expected, 3, "expected follows the clamp");
}

void TestShotFired() {
	// current == expected - 1: the player fired one round - expected follows, nothing is written.
	const auto fired = DecideReserve(2, 3, 3);
	CheckEq(fired.set, false, "firing one round writes nothing");
	CheckEq(fired.expected, 2, "the expected reserve follows the shot");
}

void TestRefill() {
	// Dropped further than one (e.g. a pickup or a script): restore to expected - 1.
	const auto refill = DecideReserve(0, 3, 3);
	CheckEq(refill.set, true, "a bigger drop is restored");
	CheckEq(refill.value, 2, "restored to expected - 1 (the reloaded magazine)");
	CheckEq(refill.expected, 2, "expected follows the restore");

	// expected - 1 never goes below zero.
	const auto floor = DecideReserve(0, 0, 6);
	CheckEq(floor.set, false, "expected 0 with current 0 changes nothing");
	CheckEq(floor.expected, 0, "and expected stays 0");
}

void TestQuietResync() {
	// Anything else (a scoped value, a different magazine size): remember the current value.
	const auto resync = DecideReserve(5, 4, 6);
	CheckEq(resync.set, false, "an unrelated value is not written");
	CheckEq(resync.expected, 5, "expected is resynced to it");
}

void TestReserveAmmoAt() {
	// CBasePlayerWeapon::m_pReserveAmmo is an INLINE int32[2] (live schema dump: kind
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
	TestTargetReserve();
	TestFirstContact();
	TestClampDown();
	TestShotFired();
	TestRefill();
	TestQuietResync();
	TestReserveAmmoAt();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
