#pragma once
// vip_infiniteammo — the pack table built from the parsed config (engine-free, shared with the
// test).
// The production file is "InfiniteReserveAmmo" { "god" {...} "premium" {...} "standart" {...} }
// (ref: prod-27016/addons/configs/vip/vip_infiniteammo.ini). ParseKv1() returns that single
// top-level block, so its children are the packs and each pack's value-bearing children are:
//
//   "default"        the fallback for a weapon the pack does not list
//   "<classname>"    the per-weapon magazine count (weapon_ak47, weapon_awp, ...)
//
// This is exactly what the pisex::KeyValues loader of the first port built: FOR_EACH_SUBKEY over
// the section gave the packs, FOR_EACH_VALUE the values, and a pack that grants nothing (no
// default and no overrides) was dropped.
//
// Header-only: included by src/plugin.cpp and by tests/vip_infiniteammo_tests.cpp.

#include <map>
#include <string>
#include <string_view>

#include "ammo_logic.hpp"
#include "kv1.hpp"

namespace vip_infiniteammo {

// Builds the pack table from the flattened config root (the "InfiniteReserveAmmo" section).
inline std::map<std::string, AmmoPack> PacksFromKv(const KvNode& section) {
	std::map<std::string, AmmoPack> packs;
	for (const KvNode& packNode : section.children) {
		AmmoPack pack;
		pack.name = packNode.name;
		for (const KvNode& value : packNode.children) {
			if (!value.hasValue) {
				continue; // a nested block inside a pack is not a value
			}
			if (value.name == "default") {
				pack.defaultMagazines = value.IntValue();
			} else {
				pack.weaponOverrides[value.name] = value.IntValue();
			}
		}
		// The original's "default != 0 || !weaponOverrides.empty()": a pack that grants nothing is
		// not kept (it could never make PlanAmmo leave Excluded anyway).
		if (pack.defaultMagazines != 0 || !pack.weaponOverrides.empty()) {
			packs[pack.name] = pack;
		}
	}
	return packs;
}

} // namespace vip_infiniteammo
