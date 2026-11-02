// vip_infiniteammo_tests — the test binary of the pure-s2sdk vip_infiniteammo plugin.
//
// Two halves, neither of which links pisexcompat:
//
//   1. Engine-free logic: the target computation of the partiusfabaa scheme (ammo_logic.hpp: N
//      magazines -> N * m_iMaxClip1 reserve rounds, N >= 999 -> infinite clip, N <= 0 -> excluded,
//      plus the inline-int32[2] layout proof of m_pReserveAmmo), the self-contained KV1 reader
//      (kv1.hpp), the pack table built from the production-shaped config in tests/data
//      (config.hpp) and the config path candidate order (common.hpp).
//   2. The plugin's own guards with fake __s2sdk_* bindings (guards.hpp / schema_cache.hpp, the
//      technique vip_items_tests and vip_regen_armor_tests use): no client, entity or schema query
//      before the first map and after OnMapEnd (PITFALLS B1/B2), the IsAlive chain of C2, the
//      entity-handle filter of C1, the map-gated success-only-cached SchemaOffset/SchemaChainOffset
//      of C8, and the guarded int32 field read/write the ammo writes go through.
//
// Run by ../../test.sh (build/tests/vip_infiniteammo_tests), or directly from the build.

#include <plg/plugin.hpp>
#include <s2sdk/s2sdk.hpp>

#include "ammo_logic.hpp"
#include "common.hpp"
#include "config.hpp"
#include "guards.hpp"
#include "kv1.hpp"
#include "schema_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef VIP_INFINITEAMMO_TEST_DATA_DIR
#define VIP_INFINITEAMMO_TEST_DATA_DIR "."
#endif

// A real plugin gets these from PLUGIFY_PLUGIN; the test leaves them null like a test binary
// without the Plugify module, except Log and GetConfigsDir, which it drives itself.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

using vip_infiniteammo::AmmoMode;
using vip_infiniteammo::AmmoPack;
using vip_infiniteammo::AmmoPlan;
using vip_infiniteammo::PlanAmmo;
using vip_infiniteammo::ReserveAmmoAt;
using vip_infiniteammo::TargetMagazines;

int g_checks = 0;
int g_failures = 0;

std::vector<std::string> g_logLines;
plg::Severity g_lastSeverity = plg::Severity::Unknown;

// The configs dir the fake plg::GetConfigsDir answers; the path candidate test drives it.
std::string g_fakeConfigsDir;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	g_logLines.emplace_back(message);
	g_lastSeverity = severity;
}

plg::string FakeGetConfigsDir() {
	return plg::string(g_fakeConfigsDir.data(), g_fakeConfigsDir.size());
}

// --- check helpers -----------------------------------------------------------------------------

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %lld, got %lld)\n", what.c_str(), static_cast<long long>(expected),
					static_cast<long long>(actual));
	}
}

void CheckEqStr(std::string_view actual, std::string_view expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected '%.*s', got '%.*s')\n", what.c_str(), static_cast<int>(expected.size()),
					expected.data(), static_cast<int>(actual.size()), actual.data());
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

// ==============================================================================================
// Part 1: the engine-free logic
// ==============================================================================================

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
	CheckEqInt(TargetMagazines(pack, "weapon_ak47"), 6, "the pack default");
	CheckEqInt(TargetMagazines(pack, "weapon_m4a1"), 8, "the per-weapon override");
	CheckEqInt(TargetMagazines(pack, "weapon_awp"), 4, "a second override");
	CheckEqInt(TargetMagazines(pack, "weapon_smokegrenade"), 6, "an unlisted weapon uses the default");

	// N <= 0 for one weapon excludes just that weapon (the format's replacement for DisabledGuns).
	AmmoPack mixed;
	mixed.defaultMagazines = 3;
	mixed.weaponOverrides["weapon_knife"] = 0;
	mixed.weaponOverrides["weapon_bayonet"] = -1;
	CheckEqInt(TargetMagazines(mixed, "weapon_knife"), 0, "a 0 override excludes the weapon");
	CheckEqInt(TargetMagazines(mixed, "weapon_bayonet"), -1, "a negative override excludes the weapon");
	CheckEqInt(TargetMagazines(mixed, "weapon_ak47"), 3, "the default still applies to the others");
}

void TestReserveTarget() {
	// N magazines of m_iMaxClip1 rounds each.
	CheckMode(PlanAmmo(6, 30), AmmoMode::Reserve, "6 magazines is reserve mode");
	CheckEqInt(PlanAmmo(6, 30).value, 180, "6 * 30 = 180 reserve rounds");
	CheckEqInt(PlanAmmo(3, 20).value, 60, "3 * 20 = 60 reserve rounds");
	CheckEqInt(PlanAmmo(2, 5).value, 10, "2 * 5 = 10 reserve rounds");
	CheckEqInt(PlanAmmo(1, 1).value, 1, "1 * 1 = 1 reserve round");

	// The shipped premium pack, as the config loader would build it.
	const AmmoPack premium = PremiumPack();
	CheckEqInt(PlanAmmo(premium, "weapon_ak47", 30).value, 180, "premium ak47: 6 * 30");
	CheckEqInt(PlanAmmo(premium, "weapon_m4a1", 30).value, 240, "premium m4a1 override: 8 * 30");
	CheckEqInt(PlanAmmo(premium, "weapon_awp", 5).value, 20, "premium awp override: 4 * 5");
}

void TestInfiniteClip() {
	// N >= 999: the reference's Type 1 - the clip is refilled to the weapon's max clip.
	const AmmoPlan god = PlanAmmo(999, 30);
	CheckMode(god, AmmoMode::InfiniteClip, "999 magazines is infinite clip mode");
	CheckEqInt(god.value, 30, "infinite clip fills to the weapon's max clip");
	CheckEqInt(PlanAmmo(1000, 13).value, 13, "1000 magazines fills to the max clip");
	CheckEqInt(PlanAmmo(GodPack(), "weapon_nova", 8).value, 8, "the shipped god pack -> infinite clip");

	// The boundary: 998 is still a finite reserve target, 999 flips to infinite clip.
	CheckMode(PlanAmmo(998, 1), AmmoMode::Reserve, "998 magazines is still reserve mode");
	CheckEqInt(PlanAmmo(998, 1).value, 998, "998 * 1");
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
	CheckEqInt(PlanAmmo(whitelist, "weapon_ak47", 30).value, 120, "the listed weapon gets 4 * 30");
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
	Check(reinterpret_cast<void*>(reserve) == static_cast<void*>(weapon + kOffset),
		  "ReserveAmmoAt returns the address of the inline field");

	// The two elements of the array, [0..1], are the ints stored inline at the field offset.
	const int32_t seed[2] = {13, 7};
	std::memcpy(weapon + kOffset, seed, sizeof(seed));
	CheckEqInt(reserve[0], 13, "reserve[0] reads the first inline int");
	CheckEqInt(reserve[1], 7, "reserve[1] reads the second inline int");

	// A write through the accessor changes the inline bytes - and nothing outside the 8-byte array.
	weapon[kOffset + 8] = 0x5A;
	weapon[kOffset - 1] = 0xA5;
	reserve[0] = 6;
	int32_t readBack = 0;
	std::memcpy(&readBack, weapon + kOffset, sizeof(readBack));
	CheckEqInt(readBack, 6, "reserve[0] writes the first inline int");
	CheckEqInt(weapon[kOffset + 8], 0x5A, "the write stays inside the array (after)");
	CheckEqInt(weapon[kOffset - 1], 0xA5, "the write stays inside the array (before)");
}

// --- KV1 reader and the pack table -------------------------------------------------------------

void TestKv1Reader() {
	using vip_infiniteammo::ParseKv1;

	// The production shape: one root block, comments (including Cyrillic ones) between the keys.
	const std::string production =
		"\"InfiniteReserveAmmo\"\n"
		"{\n"
		"    // ===========================================\n"
		"    // Пак \"god\" - бесконечные обоймы\n"
		"    // ===========================================\n"
		"    \"god\"\n"
		"    {\n"
		"        \"default\"               \"999\"\n"
		"    }\n"
		"}\n";
	const auto config = ParseKv1(production);
	Check(config.has_value(), "the production-shaped section parses");
	if (config) {
		CheckEqStr(config->name, "InfiniteReserveAmmo", "the single top-level block is the root");
		const auto* god = config->Find("god");
		Check(god != nullptr, "the 'god' pack is found");
		if (god != nullptr) {
			CheckEqInt(god->GetInt("default"), 999, "the pack default");
		}
		CheckEqInt(config->GetInt("missing", 7), 7, "a missing key keeps the default");
	}

	// Key lookup is case-insensitive, like Valve's loader.
	const auto mixedCase = ParseKv1("\"InfiniteReserveAmmo\" { \"default\" \"4\" }");
	Check(mixedCase.has_value(), "a mixed-case key parses");
	if (mixedCase) {
		CheckEqInt(mixedCase->GetInt("DEFAULT"), 4, "key lookup is case-insensitive");
	}

	// Unquoted tokens.
	const auto unquoted = ParseKv1("InfiniteReserveAmmo { god { default 5 } }");
	Check(unquoted.has_value(), "unquoted tokens parse");
	if (unquoted) {
		CheckEqInt(unquoted->GetInt("god"), 0, "a block child has no value of its own");
		const auto* god = unquoted->Find("god");
		Check(god != nullptr, "the unquoted pack is found");
		if (god != nullptr) {
			CheckEqInt(god->GetInt("default"), 5, "the unquoted value");
		}
	}

	// Comments: line comments after a value and block comments between the tokens.
	const auto comments = ParseKv1("\"InfiniteReserveAmmo\"\n{\n  // the pack comment\n"
								   "  \"default\" /* inline */ \"3\" /* after */\n}\n");
	Check(comments.has_value(), "comments are skipped");
	if (comments) {
		CheckEqInt(comments->GetInt("default"), 3, "the value behind the comments");
	}

	// #base / #include / #define are skipped, not included.
	const auto includes = ParseKv1("#include \"other.ini\"\n\"InfiniteReserveAmmo\" { \"default\" \"6\" }\n");
	Check(includes.has_value(), "an #include line does not invalidate the file");
	if (includes) {
		CheckEqInt(includes->GetInt("default"), 6, "the value next to an #include line");
	}

	// A file without the single "Root" block: the keys live at the top level.
	const auto flat = ParseKv1("\"default\" \"7\"\n");
	Check(flat.has_value(), "a flat file parses");
	if (flat) {
		CheckEqInt(flat->GetInt("default"), 7, "a top-level key is found");
	}

	// Empty file: a valid, empty tree (pisexcompat's behaviour; the caller keeps its defaults).
	const auto empty = ParseKv1("");
	Check(empty.has_value(), "an empty file is a valid empty tree");
	if (empty) {
		CheckEqInt(empty->GetInt("default", 9), 9, "an empty tree keeps the default");
	}

	// Value parsing: whitespace trimmed, Valve's atoi fallback.
	const auto values = ParseKv1("P { \"trimmed\" \" 5 \" \"junk\" \"abc\" \"leading\" \"12abc\" }");
	Check(values.has_value(), "the values file parses");
	if (values) {
		CheckEqInt(values->GetInt("trimmed"), 5, "the value is trimmed");
		CheckEqInt(values->GetInt("junk"), 0, "a non-number reads as 0 (atoi)");
		CheckEqInt(values->GetInt("leading"), 12, "a leading number wins (atoi)");
		CheckEqInt(values->GetInt("absent", 8), 8, "a missing key keeps the default");
	}

	// Escapes inside quotes.
	const auto escapes = ParseKv1("\"P\" { \"note\" \"a\\\"b\\\\c\" }");
	Check(escapes.has_value(), "escaped quotes and backslashes parse");
	if (escapes) {
		CheckEqStr(escapes->GetString("note"), "a\"b\\c", "the escapes are decoded");
	}

	// Malformed files are rejected as a whole (no half-read tree).
	Check(!ParseKv1("\"P\" { \"default\" \"2\"").has_value(), "a missing '}' is rejected");
	Check(!ParseKv1("\"P\" { \"default\" \"2\" } }").has_value(), "a stray '}' is rejected");
	Check(!ParseKv1("\"P\" { \"default\" \"2 }").has_value(), "an unterminated string is rejected");
}

void TestPackTable() {
	using vip_infiniteammo::KvNode;
	using vip_infiniteammo::PacksFromKv;
	using vip_infiniteammo::ParseKv1;

	const auto root = ParseKv1("\"InfiniteReserveAmmo\" {"
							   "  \"grantless\" { }"
							   "  \"zerodefault\" { \"default\" \"0\" }"
							   "  \"excludedweapon\" { \"weapon_ak47\" \"0\" }"
							   "  \"nested\" { \"default\" \"2\" \"inner\" { \"weapon_ak47\" \"9\" } }"
							   "}");
	Check(root.has_value(), "the pack table parses");
	if (!root) {
		return;
	}
	const std::map<std::string, AmmoPack> packs = PacksFromKv(*root);
	CheckEqInt(static_cast<int64_t>(packs.size()), 2, "only the packs that grant something are kept");
	Check(packs.find("grantless") == packs.end(), "a pack without values is dropped");
	Check(packs.find("zerodefault") == packs.end(), "a pack with a 0 default and no overrides is dropped");

	const auto excluded = packs.find("excludedweapon");
	Check(excluded != packs.end(), "an explicit per-weapon exclusion keeps the pack");
	if (excluded != packs.end()) {
		CheckEqInt(excluded->second.defaultMagazines, 0, "no default for the exclusion pack");
		CheckEqInt(static_cast<int64_t>(excluded->second.weaponOverrides.size()), 1, "one override");
		CheckMode(PlanAmmo(excluded->second, "weapon_ak47", 30), AmmoMode::Excluded,
				  "the 0 override excludes the weapon");
	}

	const auto nested = packs.find("nested");
	Check(nested != packs.end(), "a pack with a nested block is kept");
	if (nested != packs.end()) {
		CheckEqInt(nested->second.defaultMagazines, 2, "the default of the nested pack");
		CheckEqInt(static_cast<int64_t>(nested->second.weaponOverrides.size()), 0,
				   "the nested block is not read as a weapon value");
		CheckEqInt(PlanAmmo(nested->second, "weapon_ak47", 30).value, 60, "2 * 30");
	}

	// An empty section has no packs at all (the plugin stays idle, exactly like the pisex loader).
	const auto emptyRoot = ParseKv1("\"InfiniteReserveAmmo\" { }");
	Check(emptyRoot.has_value(), "an empty section parses");
	if (emptyRoot) {
		CheckEqInt(static_cast<int64_t>(PacksFromKv(*emptyRoot).size()), 0, "no packs");
	}
}

void TestShippedConfig() {
	const std::string path = std::string(VIP_INFINITEAMMO_TEST_DATA_DIR) + "/vip_infiniteammo.ini";
	const std::optional<std::string> text = vip_infiniteammo::ReadTextFile(path);
	Check(text.has_value(), "tests/data/vip_infiniteammo.ini is readable");
	if (!text) {
		return;
	}
	Check(!vip_infiniteammo::ReadTextFile(std::string(VIP_INFINITEAMMO_TEST_DATA_DIR) + "/does-not-exist.ini")
			   .has_value(),
		  "a missing config file is reported");

	const std::optional<vip_infiniteammo::KvNode> root = vip_infiniteammo::ParseKv1(*text);
	Check(root.has_value(), "the production config parses");
	if (!root) {
		return;
	}
	const std::map<std::string, AmmoPack> packs = vip_infiniteammo::PacksFromKv(*root);
	CheckEqInt(static_cast<int64_t>(packs.size()), 3, "the production config has the three packs");

	const auto find = [&packs](const char* name) -> const AmmoPack* {
		const auto it = packs.find(name);
		return it != packs.end() ? &it->second : nullptr;
	};

	const AmmoPack* god = find("god");
	Check(god != nullptr, "the 'god' pack loads");
	if (god != nullptr) {
		CheckEqInt(god->defaultMagazines, 999, "god: 999 magazines by default");
		CheckEqInt(static_cast<int64_t>(god->weaponOverrides.size()), 0, "god: no per-weapon overrides");
		CheckMode(PlanAmmo(*god, "weapon_ak47", 30), AmmoMode::InfiniteClip,
				  "god + ak47 is the infinite-clip mode");
		CheckEqInt(PlanAmmo(*god, "weapon_ak47", 30).value, 30, "god fills the clip to the max clip");
	}

	const AmmoPack* premium = find("premium");
	Check(premium != nullptr, "the 'premium' pack loads");
	if (premium != nullptr) {
		CheckEqInt(premium->defaultMagazines, 6, "premium: 6 magazines by default");
		CheckEqInt(TargetMagazines(*premium, "weapon_ak47"), 6, "premium ak47");
		CheckEqInt(TargetMagazines(*premium, "weapon_m4a1"), 8, "premium m4a1 override");
		CheckEqInt(TargetMagazines(*premium, "weapon_awp"), 4, "premium awp override");
		CheckEqInt(TargetMagazines(*premium, "weapon_glock"), 6, "premium glock");
		CheckEqInt(TargetMagazines(*premium, "weapon_unlisted"), 6, "an unlisted weapon uses the default");
		CheckEqInt(PlanAmmo(*premium, "weapon_ak47", 30).value, 180, "premium ak47: 6 * 30");
		CheckEqInt(PlanAmmo(*premium, "weapon_m4a1", 30).value, 240, "premium m4a1: 8 * 30");
		CheckEqInt(PlanAmmo(*premium, "weapon_awp", 5).value, 20, "premium awp: 4 * 5");
	}

	const AmmoPack* standart = find("standart");
	Check(standart != nullptr, "the 'standart' pack loads");
	if (standart != nullptr) {
		CheckEqInt(standart->defaultMagazines, 3, "standart: 3 magazines by default");
		CheckEqInt(TargetMagazines(*standart, "weapon_m4a1"), 4, "standart m4a1 override");
		CheckEqInt(TargetMagazines(*standart, "weapon_ak47"), 3, "standart ak47");
		CheckEqInt(PlanAmmo(*standart, "weapon_negev", 150).value, 300, "standart negev: 2 * 150");
	}
}

void TestConfigPaths() {
	plg::GetConfigsDir = &FakeGetConfigsDir;

	g_fakeConfigsDir = "/test/configs";
	const std::vector<std::string> candidates = vip_infiniteammo::ConfigCandidates();
	CheckEqInt(static_cast<int64_t>(candidates.size()), 3, "three candidates with a configs dir");
	if (candidates.size() == 3) {
		CheckEqStr(candidates[0], "vip_infiniteammo.ini", "the working-directory candidate comes first");
		CheckEqStr(candidates[1], "/test/configs/vip_infiniteammo/vip_infiniteammo.ini",
				   "the packaged config location");
		CheckEqStr(candidates[2], "/test/configs/vip_infiniteammo.ini", "the configs-root fallback");
	}
	CheckEqStr(vip_infiniteammo::PrimaryConfigPath(), "/test/configs/vip_infiniteammo/vip_infiniteammo.ini",
			   "the primary path is the packaged one");

	g_fakeConfigsDir = "/test/configs/";
	CheckEqStr(vip_infiniteammo::ConfigCandidates()[1], "/test/configs/vip_infiniteammo/vip_infiniteammo.ini",
			   "a trailing slash is not doubled");

	g_fakeConfigsDir.clear();
	CheckEqInt(static_cast<int64_t>(vip_infiniteammo::ConfigCandidates().size()), 1,
			   "an empty configs dir leaves only the bare file");
	CheckEqStr(vip_infiniteammo::PrimaryConfigPath(), "vip_infiniteammo.ini",
			   "the primary path is the bare file name");

	plg::GetConfigsDir = nullptr;
	CheckEqInt(static_cast<int64_t>(vip_infiniteammo::ConfigCandidates().size()), 1,
			   "a missing configs-dir binding leaves only the bare file");
	plg::GetConfigsDir = &FakeGetConfigsDir;
}

// ==============================================================================================
// Part 2: the guards with fake s2sdk bindings
// ==============================================================================================

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool alive = false;
	bool controllerPointer = false; // PlayerSlotToEntPointer() answers non-null
	bool controllerHandleValid = false;
	bool pawnPointer = false; // GetClientPawn() answers non-null
	bool pawnHandleValid = false;
	bool pawnPointerValid = false;
};

std::vector<FakeClient> g_clients(vip_infiniteammo::kMaxSlots);

int g_connectedCalls = 0;
int g_inGameCalls = 0;
int g_sourceTVCalls = 0;
int g_aliveCalls = 0;
int g_controllerPointerCalls = 0;
int g_controllerHandleCalls = 0;
int g_pawnCalls = 0;
int g_entPointerToHandleCalls = 0;
int g_validHandleCalls = 0;
int g_validPointerCalls = 0;
int g_schemaOffsetCalls = 0;
int g_schemaChainOffsetCalls = 0;
int g_getEntDataCalls = 0;
int g_setEntDataCalls = 0;

std::vector<int32_t> g_validHandles;

int32_t g_schemaOffsetResult = 72;
int32_t g_schemaChainResult = -1;
std::string g_lastSchemaClass;
std::string g_lastSchemaField;

int64_t g_entDataValue = 0;
int32_t g_lastGetHandle = 0;
int32_t g_lastGetOffset = -1;
int32_t g_lastGetSize = 0;

int32_t g_lastSetHandle = 0;
int32_t g_lastSetOffset = -1;
int64_t g_lastSetValue = 0;
int32_t g_lastSetSize = 0;
bool g_lastSetChangeState = false;
int32_t g_lastSetChainOffset = 0;

int g_observerStartCalls = 0;
int g_observerEndCalls = 0;
bool g_observerStartSawReady = false;
bool g_observerEndSawReady = false;

FakeClient& Client(int32_t slot) {
	return g_clients[static_cast<std::size_t>(slot)];
}

int32_t ControllerHandle(int32_t slot) {
	return 1000 + slot;
}

int32_t PawnHandle(int32_t slot) {
	return 2000 + slot;
}

void* ControllerPointer(int32_t slot) {
	return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000 + slot));
}

void* PawnPointer(int32_t slot) {
	return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x5000 + slot));
}

int32_t SlotFromPawnPointer(void* entity) {
	const std::uintptr_t base = 0x5000;
	const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(entity);
	if (value < base || value >= base + static_cast<std::uintptr_t>(vip_infiniteammo::kMaxSlots)) {
		return -1;
	}
	return static_cast<int32_t>(value - base);
}

bool IsRegisteredHandle(int32_t handle) {
	return std::find(g_validHandles.begin(), g_validHandles.end(), handle) != g_validHandles.end();
}

// A client that passes every layer of IsAlive.
void AliveClient(int32_t slot) {
	FakeClient& client = Client(slot);
	client = FakeClient{};
	client.connected = true;
	client.inGame = true;
	client.alive = true;
	client.controllerPointer = true;
	client.controllerHandleValid = true;
	client.pawnPointer = true;
	client.pawnHandleValid = true;
	client.pawnPointerValid = true;
	g_validHandles.push_back(ControllerHandle(slot));
	g_validHandles.push_back(PawnHandle(slot));
}

int ClientQueries() {
	return g_connectedCalls + g_inGameCalls + g_sourceTVCalls + g_aliveCalls + g_controllerPointerCalls +
		   g_controllerHandleCalls + g_pawnCalls + g_entPointerToHandleCalls + g_validHandleCalls +
		   g_validPointerCalls;
}

// --- fake s2sdk exports ------------------------------------------------------------------------

bool FakeIsClientConnected(int32_t slot) {
	++g_connectedCalls;
	return Client(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_inGameCalls;
	return Client(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_sourceTVCalls;
	return Client(slot).sourceTV;
}

bool FakeIsClientAlive(int32_t slot) {
	++g_aliveCalls;
	return Client(slot).alive;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_controllerPointerCalls;
	return Client(slot).controllerPointer ? ControllerPointer(slot) : nullptr;
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_controllerHandleCalls;
	return Client(slot).controllerHandleValid ? ControllerHandle(slot) : 0;
}

void* FakeGetClientPawn(int32_t slot) {
	++g_pawnCalls;
	return Client(slot).pawnPointer ? PawnPointer(slot) : nullptr;
}

int32_t FakeEntPointerToEntHandle(void* entity) {
	++g_entPointerToHandleCalls;
	const int32_t slot = SlotFromPawnPointer(entity);
	if (slot < 0) {
		return -1;
	}
	return Client(slot).pawnHandleValid ? PawnHandle(slot) : 0;
}

bool FakeIsValidEntHandle(int32_t handle) {
	++g_validHandleCalls;
	return IsRegisteredHandle(handle);
}

bool FakeIsValidEntPointer(void* entity) {
	++g_validPointerCalls;
	const int32_t slot = SlotFromPawnPointer(entity);
	return slot >= 0 && Client(slot).pawnPointerValid;
}

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& memberName) {
	++g_schemaOffsetCalls;
	g_lastSchemaClass.assign(className.data(), className.size());
	g_lastSchemaField.assign(memberName.data(), memberName.size());
	return g_schemaOffsetResult;
}

int32_t FakeGetSchemaChainOffset(const plg::string& className) {
	++g_schemaChainOffsetCalls;
	g_lastSchemaClass.assign(className.data(), className.size());
	return g_schemaChainResult;
}

int64_t FakeGetEntData(int32_t handle, int32_t offset, int32_t size) {
	++g_getEntDataCalls;
	g_lastGetHandle = handle;
	g_lastGetOffset = offset;
	g_lastGetSize = size;
	return g_entDataValue;
}

void FakeSetEntData(int32_t handle, int32_t offset, int64_t value, int32_t size, bool changeState,
					int32_t chainOffset) {
	++g_setEntDataCalls;
	g_lastSetHandle = handle;
	g_lastSetOffset = offset;
	g_lastSetValue = value;
	g_lastSetSize = size;
	g_lastSetChangeState = changeState;
	g_lastSetChainOffset = chainOffset;
}

// --- fake map listeners ------------------------------------------------------------------------

s2sdk::OnMapStartCallback g_mapStartCallback = nullptr;
s2sdk::OnMapEndCallback g_mapEndCallback = nullptr;
int g_mapStartRegistrations = 0;
int g_mapStartUnregistrations = 0;
int g_mapEndRegistrations = 0;
int g_mapEndUnregistrations = 0;

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_mapStartCallback = callback;
	++g_mapStartRegistrations;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_mapStartCallback == callback) {
		g_mapStartCallback = nullptr;
	}
	++g_mapStartUnregistrations;
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_mapEndCallback = callback;
	++g_mapEndRegistrations;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_mapEndCallback == callback) {
		g_mapEndCallback = nullptr;
	}
	++g_mapEndUnregistrations;
}

void FireMapStart() {
	if (g_mapStartCallback != nullptr) {
		g_mapStartCallback();
	}
}

void FireMapEnd() {
	if (g_mapEndCallback != nullptr) {
		g_mapEndCallback();
	}
}

void FakeMapStartObserver() {
	++g_observerStartCalls;
	g_observerStartSawReady = vip_infiniteammo::IsMapReady();
}

void FakeMapEndObserver() {
	++g_observerEndCalls;
	g_observerEndSawReady = vip_infiniteammo::IsMapReady();
}

void InstallFakes() {
	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_EntPointerToEntHandle = &FakeEntPointerToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetSchemaChainOffset = &FakeGetSchemaChainOffset;
	__s2sdk_GetEntData = &FakeGetEntData;
	__s2sdk_SetEntData = &FakeSetEntData;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	g_mapStartCallback = nullptr;
	g_mapEndCallback = nullptr;
}

void ResetCounters() {
	g_connectedCalls = 0;
	g_inGameCalls = 0;
	g_sourceTVCalls = 0;
	g_aliveCalls = 0;
	g_controllerPointerCalls = 0;
	g_controllerHandleCalls = 0;
	g_pawnCalls = 0;
	g_entPointerToHandleCalls = 0;
	g_validHandleCalls = 0;
	g_validPointerCalls = 0;
	g_schemaOffsetCalls = 0;
	g_schemaChainOffsetCalls = 0;
	g_getEntDataCalls = 0;
	g_setEntDataCalls = 0;
	g_mapStartRegistrations = 0;
	g_mapStartUnregistrations = 0;
	g_mapEndRegistrations = 0;
	g_mapEndUnregistrations = 0;
	g_lastSchemaClass.clear();
	g_lastSchemaField.clear();
	g_lastGetHandle = 0;
	g_lastGetOffset = -1;
	g_lastGetSize = 0;
	g_lastSetHandle = 0;
	g_lastSetOffset = -1;
	g_lastSetValue = 0;
	g_lastSetSize = 0;
	g_lastSetChangeState = false;
	g_lastSetChainOffset = 0;
}

// Every guard test starts from a clean gate, clean fakes and clean counters.
void BeginGateTest() {
	vip_infiniteammo::ShutdownMapGate();
	vip_infiniteammo::SetMapLifecycleCallbacks(nullptr, nullptr);
	vip_infiniteammo::ClearSchemaOffsets();
	InstallFakes();
	std::fill(g_clients.begin(), g_clients.end(), FakeClient{});
	g_validHandles.clear();
	g_schemaOffsetResult = 72;
	g_schemaChainResult = -1;
	g_entDataValue = 0;
	g_observerStartCalls = 0;
	g_observerEndCalls = 0;
	g_observerStartSawReady = false;
	g_observerEndSawReady = false;
	g_logLines.clear();
	g_lastSeverity = plg::Severity::Unknown;
	ResetCounters();
}

// --- map gate (PITFALLS B1/B2) -----------------------------------------------------------------

void TestMapGateLifecycle() {
	BeginGateTest();

	vip_infiniteammo::SetMapLifecycleCallbacks(&FakeMapStartObserver, &FakeMapEndObserver);
	vip_infiniteammo::InitMapGate();
	CheckEqInt(g_mapStartRegistrations, 1, "OnMapStart_Register is called once");
	CheckEqInt(g_mapEndRegistrations, 1, "OnMapEnd_Register is called once");
	Check(g_mapStartCallback != nullptr && g_mapEndCallback != nullptr, "both listeners are registered");
	Check(!vip_infiniteammo::IsMapReady(), "no map is ready right after registration");

	// A plugin loaded into a running map never sees OnMapStart: its first update latches the gate.
	vip_infiniteammo::PollMapGate();
	Check(vip_infiniteammo::IsMapReady(), "the first update of a mid-map load opens the gate");
	CheckEqInt(g_observerStartCalls, 0, "the mid-map latch does not fake an OnMapStart event");

	// OnMapStart: the gate is already open when the plugin's own hook runs.
	FireMapStart();
	Check(vip_infiniteammo::IsMapReady(), "OnMapStart keeps the gate open");
	CheckEqInt(g_observerStartCalls, 1, "OnMapStart calls the plugin hook once");
	Check(g_observerStartSawReady, "the OnMapStart hook runs with the gate already open");

	// OnMapEnd: PITFALLS B2 - the gate must be dropped before the hook runs.
	FireMapEnd();
	Check(!vip_infiniteammo::IsMapReady(), "OnMapEnd drops the gate");
	CheckEqInt(g_observerEndCalls, 1, "OnMapEnd calls the plugin hook once");
	Check(!g_observerEndSawReady, "the OnMapEnd hook runs with the gate already closed");

	// Through a changelevel the host keeps pumping updates: only a new OnMapStart may reopen.
	vip_infiniteammo::PollMapGate();
	Check(!vip_infiniteammo::IsMapReady(), "an update after OnMapEnd does not reopen the gate");
	FireMapStart();
	Check(vip_infiniteammo::IsMapReady(), "the next OnMapStart reopens the gate");

	// Unload: the listeners are removed and a stale update cannot open the gate again.
	vip_infiniteammo::ShutdownMapGate();
	Check(!vip_infiniteammo::IsMapReady(), "ShutdownMapGate closes the gate");
	CheckEqInt(g_mapStartUnregistrations, 1, "OnMapStart_Unregister is called once");
	CheckEqInt(g_mapEndUnregistrations, 1, "OnMapEnd_Unregister is called once");
	Check(g_mapStartCallback == nullptr && g_mapEndCallback == nullptr, "both listeners are unregistered");
	vip_infiniteammo::PollMapGate();
	Check(!vip_infiniteammo::IsMapReady(), "no update opens the gate after the listeners are gone");

	// A second InitMapGate after a shutdown registers again (a fresh OnPluginStart).
	vip_infiniteammo::InitMapGate();
	CheckEqInt(g_mapStartRegistrations, 2, "a second load registers the listeners again");
	vip_infiniteammo::ShutdownMapGate();
	vip_infiniteammo::SetMapLifecycleCallbacks(nullptr, nullptr);
}

void TestMapGateWithoutListeners() {
	BeginGateTest();

	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	g_logLines.clear();
	vip_infiniteammo::InitMapGate();
	CheckEqInt(static_cast<int64_t>(g_logLines.size()), 1, "a missing listener binding is logged once");
	Check(g_lastSeverity == plg::Severity::Error, "the missing listener is an error");
	vip_infiniteammo::PollMapGate();
	Check(!vip_infiniteammo::IsMapReady(), "without listeners the gate never opens");

	// Restore the bindings for the tests that follow.
	InstallFakes();
}

// --- client checks (PITFALLS C2) --------------------------------------------------------------

void TestIsAlive() {
	BeginGateTest();
	AliveClient(3);

	// Gate closed: nothing reaches s2sdk, not even a connected client.
	Check(!vip_infiniteammo::IsAlive(3), "no client is alive before the first map");
	CheckEqInt(ClientQueries(), 0, "no client query happens with the gate closed");

	vip_infiniteammo::InitMapGate();
	FireMapStart();
	Check(vip_infiniteammo::IsAlive(3), "a connected, in-game, alive client with a controller and pawn is alive");

	// Slot range first: an out-of-range slot never reaches an s2sdk export.
	const int beforeRangeChecks = ClientQueries();
	Check(!vip_infiniteammo::IsAlive(-1), "a negative slot is not alive");
	Check(!vip_infiniteammo::IsAlive(64), "slot 64 is not alive");
	Check(!vip_infiniteammo::IsAlive(100000), "a far out-of-range slot is not alive");
	CheckEqInt(ClientQueries(), beforeRangeChecks, "an out-of-range slot never reaches s2sdk");

	// Each layer of the chain rejects on its own.
	const auto reject = [](auto mutate, const char* what) {
		AliveClient(5);
		mutate(Client(5));
		Check(!vip_infiniteammo::IsAlive(5), what);
		Client(5) = FakeClient{};
	};
	reject([](FakeClient& client) { client.connected = false; }, "a disconnected client is not alive");
	reject([](FakeClient& client) { client.inGame = false; }, "a client that is not in game is not alive");
	reject([](FakeClient& client) { client.sourceTV = true; }, "the SourceTV proxy is not alive");
	reject([](FakeClient& client) { client.controllerPointer = false; },
		   "a missing controller entity pointer is not alive");
	reject([](FakeClient& client) { client.controllerHandleValid = false; },
		   "an invalid controller entity handle is not alive");
	reject([](FakeClient& client) { client.pawnPointer = false; }, "a missing pawn is not alive");
	reject([](FakeClient& client) { client.pawnHandleValid = false; }, "an invalid pawn handle is not alive");
	reject([](FakeClient& client) { client.pawnPointerValid = false; },
		   "an invalid pawn pointer is not alive");
	reject([](FakeClient& client) { client.alive = false; }, "a dead client is not alive");

	// A missing binding refuses the client instead of calling through a null pointer.
	__s2sdk_GetClientPawn = nullptr;
	Check(!vip_infiniteammo::IsAlive(3), "a missing binding refuses the client");
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	Check(vip_infiniteammo::IsAlive(3), "the client is alive again once the binding is back");

	// After OnMapEnd the same client is refused without touching s2sdk (PITFALLS B2).
	FireMapEnd();
	const int beforeShutdownChecks = ClientQueries();
	Check(!vip_infiniteammo::IsAlive(3), "no client is alive after OnMapEnd");
	CheckEqInt(ClientQueries(), beforeShutdownChecks, "no client query happens after OnMapEnd");
}

// --- entity handles (PITFALLS C1/C2) ----------------------------------------------------------

void TestUsableEntityHandle() {
	BeginGateTest();
	g_validHandles = {7, 1000, static_cast<int32_t>(0x80000001u)};

	// The entity system does not exist before the first map (PITFALLS B1): no handle is usable.
	Check(!vip_infiniteammo::IsUsableEntityHandle(7), "no handle is usable before the first map");
	CheckEqInt(g_validHandleCalls, 0, "no validity check happens with the gate closed");

	vip_infiniteammo::InitMapGate();
	FireMapStart();
	Check(!vip_infiniteammo::IsUsableEntityHandle(0), "handle 0 (the world) is filtered out");
	Check(!vip_infiniteammo::IsUsableEntityHandle(-1), "handle -1 (INVALID_EHANDLE_INDEX) is filtered out");
	Check(vip_infiniteammo::IsUsableEntityHandle(7), "a live handle is usable");
	// Handles are index | serial << 15: once the serial passes 65535 a *valid* handle is negative
	// (PITFALLS C1). The filter must not be "> 0", the test locks that in.
	const int32_t highSerialHandle = static_cast<int32_t>(0x80000001u);
	Check(highSerialHandle < 0, "the high-serial handle really is negative");
	Check(vip_infiniteammo::IsUsableEntityHandle(highSerialHandle), "a valid negative handle is usable");
	Check(!vip_infiniteammo::IsUsableEntityHandle(999), "a handle the entity system rejects is not usable");

	__s2sdk_IsValidEntHandle = nullptr;
	Check(!vip_infiniteammo::IsUsableEntityHandle(7), "a missing validity binding rejects every handle");
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	Check(vip_infiniteammo::IsUsableEntityHandle(7), "the handle is usable again once the binding is back");

	FireMapEnd();
	Check(!vip_infiniteammo::IsUsableEntityHandle(7), "no handle is usable after OnMapEnd");
}

// --- schema offsets (PITFALLS C8) -------------------------------------------------------------

void TestSchemaOffsetGateAndCache() {
	BeginGateTest();
	g_schemaOffsetResult = 4336; // CBasePlayerWeapon::m_iClip1 in the live schema

	CheckEqInt(vip_infiniteammo::SchemaOffset("CBasePlayerWeapon", "m_iClip1"), -1,
			   "SchemaOffset is -1 before the first map");
	CheckEqInt(g_schemaOffsetCalls, 0, "no schema lookup happens with the gate closed");

	vip_infiniteammo::InitMapGate();
	FireMapStart();
	CheckEqInt(vip_infiniteammo::SchemaOffset("CBasePlayerWeapon", "m_iClip1"), 4336, "the field offset");
	CheckEqInt(g_schemaOffsetCalls, 1, "the lookup ran once");
	CheckEqStr(g_lastSchemaClass, "CBasePlayerWeapon", "the class name is forwarded");
	CheckEqStr(g_lastSchemaField, "m_iClip1", "the field name is forwarded");

	// The success is cached: a second call does not touch s2sdk.
	CheckEqInt(vip_infiniteammo::SchemaOffset("CBasePlayerWeapon", "m_iClip1"), 4336, "the cached offset");
	CheckEqInt(g_schemaOffsetCalls, 1, "a cached offset does not call s2sdk");
	CheckEqInt(static_cast<int64_t>(vip_infiniteammo::SchemaOffsets().CachedOffsetCount()), 1,
			   "one offset is cached");

	// A failed lookup is not cached: it stays retryable.
	g_schemaOffsetResult = -1;
	CheckEqInt(vip_infiniteammo::SchemaOffset("CBasePlayerWeapon", "m_iReserveAmmo"), -1, "a failed lookup");
	CheckEqInt(vip_infiniteammo::SchemaOffset("CBasePlayerWeapon", "m_iReserveAmmo"), -1,
			   "the failed lookup is retried");
	CheckEqInt(g_schemaOffsetCalls, 3, "a -1 lookup is retried on the next call");
	CheckEqInt(static_cast<int64_t>(vip_infiniteammo::SchemaOffsets().CachedOffsetCount()), 1,
			   "only the successful offset is cached");

	// A cached offset is not handed out outside a live map.
	g_schemaOffsetResult = 4336;
	FireMapEnd();
	CheckEqInt(vip_infiniteammo::SchemaOffset("CBasePlayerWeapon", "m_iClip1"), -1,
			   "a cached offset is refused after OnMapEnd");

	// OnPluginEnd (ClearSchemaOffsets) makes the next map resolve the fields again.
	vip_infiniteammo::ClearSchemaOffsets();
	FireMapStart();
	CheckEqInt(vip_infiniteammo::SchemaOffset("CBasePlayerWeapon", "m_iClip1"), 4336,
			   "the offset is resolved again after Clear()");
	CheckEqInt(g_schemaOffsetCalls, 4, "Clear() forgets the cached offsets");

	// A missing binding answers -1 instead of calling through a null pointer.
	__s2sdk_GetSchemaOffset = nullptr;
	CheckEqInt(vip_infiniteammo::SchemaOffset("CBasePlayerWeapon", "m_iClip2"), -1,
			   "a missing schema binding answers -1");
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
}

void TestSchemaChainOffsetGateAndCache() {
	BeginGateTest();
	g_schemaChainResult = -1; // CBasePlayerWeapon is an entity: no __m_pChainEntity

	CheckEqInt(vip_infiniteammo::SchemaChainOffset("CBasePlayerWeapon"), -1,
			   "SchemaChainOffset is -1 before the first map");
	CheckEqInt(g_schemaChainOffsetCalls, 0, "no chain lookup happens with the gate closed");

	vip_infiniteammo::InitMapGate();
	FireMapStart();
	CheckEqInt(vip_infiniteammo::SchemaChainOffset("CBasePlayerWeapon"), -1,
			   "no chainer is normalised to -1 (the entity itself)");
	CheckEqInt(vip_infiniteammo::SchemaChainOffset("CBasePlayerWeapon"), -1, "the -1 is retried");
	CheckEqInt(g_schemaChainOffsetCalls, 2, "a -1 chain offset is not cached");
	CheckEqInt(static_cast<int64_t>(vip_infiniteammo::SchemaOffsets().CachedChainOffsetCount()), 0,
			   "nothing is cached for -1");

	// 0 must never reach SetEntData: s2sdk would read the object's vtable as a chainer.
	g_schemaChainResult = 0;
	CheckEqInt(vip_infiniteammo::SchemaChainOffset("CBasePlayerWeapon"), -1, "a 0 chain offset is -1");
	CheckEqInt(static_cast<int64_t>(vip_infiniteammo::SchemaOffsets().CachedChainOffsetCount()), 0,
			   "0 is not cached");

	// A real chain offset (> 0) is cached.
	g_schemaChainResult = 8;
	CheckEqInt(vip_infiniteammo::SchemaChainOffset("CBasePlayerWeapon"), 8, "a positive chain offset");
	CheckEqInt(vip_infiniteammo::SchemaChainOffset("CBasePlayerWeapon"), 8, "the cached chain offset");
	CheckEqInt(g_schemaChainOffsetCalls, 4, "a cached chain offset does not call s2sdk");
	CheckEqInt(static_cast<int64_t>(vip_infiniteammo::SchemaOffsets().CachedChainOffsetCount()), 1,
			   "the positive chain offset is cached");

	FireMapEnd();
	CheckEqInt(vip_infiniteammo::SchemaChainOffset("CBasePlayerWeapon"), -1,
			   "a cached chain offset is refused after OnMapEnd");
}

int32_t g_directOffsetResult = 42;
int g_directOffsetCalls = 0;
int32_t g_directChainResult = -1;
int g_directChainCalls = 0;

int32_t DirectOffsetLookup(const char* /*className*/, const char* /*fieldName*/) {
	++g_directOffsetCalls;
	return g_directOffsetResult;
}

int32_t DirectChainLookup(const char* /*className*/) {
	++g_directChainCalls;
	return g_directChainResult;
}

void TestSchemaCachePolicy() {
	// The cache is driven directly here, with captureless (function-pointer) lookups.
	vip_infiniteammo::SchemaOffsetCache cache;

	g_directOffsetCalls = 0;
	g_directOffsetResult = 42;
	CheckEqInt(cache.Offset(false, "C", "f", &DirectOffsetLookup), -1, "a closed gate answers -1");
	CheckEqInt(g_directOffsetCalls, 0, "a closed gate never calls the lookup");
	CheckEqInt(cache.Offset(true, "C", "f", &DirectOffsetLookup), 42, "the lookup result");
	CheckEqInt(cache.Offset(true, "C", "f", &DirectOffsetLookup), 42, "the cached result");
	CheckEqInt(g_directOffsetCalls, 1, "a cached hit does not call the lookup");
	CheckEqInt(static_cast<int64_t>(cache.CachedOffsetCount()), 1, "one offset is cached");

	g_directOffsetResult = -1;
	CheckEqInt(cache.Offset(true, "D", "g", &DirectOffsetLookup), -1, "a failed lookup");
	CheckEqInt(cache.Offset(true, "D", "g", &DirectOffsetLookup), -1, "the failed lookup again");
	CheckEqInt(g_directOffsetCalls, 3, "a -1 is not cached");
	CheckEqInt(static_cast<int64_t>(cache.CachedOffsetCount()), 1, "the failure added no entry");

	CheckEqInt(cache.Offset(true, "", "g", &DirectOffsetLookup), -1, "an empty class name");
	CheckEqInt(cache.Offset(true, "D", "", &DirectOffsetLookup), -1, "an empty field name");
	CheckEqInt(cache.Offset(true, "D", "g", nullptr), -1, "a null lookup");

	g_directChainCalls = 0;
	g_directChainResult = -1;
	CheckEqInt(cache.ChainOffset(true, "C", &DirectChainLookup), -1, "no chainer");
	CheckEqInt(cache.ChainOffset(false, "C", &DirectChainLookup), -1, "a closed gate answers -1");
	g_directChainResult = 0;
	CheckEqInt(cache.ChainOffset(true, "C", &DirectChainLookup), -1, "0 is normalised to -1");
	CheckEqInt(static_cast<int64_t>(cache.CachedChainOffsetCount()), 0, "nothing is cached");
	g_directChainResult = 16;
	CheckEqInt(cache.ChainOffset(true, "C", &DirectChainLookup), 16, "a positive chain offset");
	CheckEqInt(cache.ChainOffset(true, "C", &DirectChainLookup), 16, "the cached chain offset");
	CheckEqInt(g_directChainCalls, 3, "only the successful chain lookup is cached");

	cache.Clear();
	CheckEqInt(static_cast<int64_t>(cache.CachedOffsetCount()), 0, "Clear() forgets the offsets");
	CheckEqInt(static_cast<int64_t>(cache.CachedChainOffsetCount()), 0, "Clear() forgets the chain offsets");

	// The plugin's shared instance was not touched by this local cache.
	CheckEqInt(static_cast<int64_t>(vip_infiniteammo::SchemaOffsets().CachedOffsetCount()), 0,
			   "the shared cache stays empty");
}

// --- int32 field access -----------------------------------------------------------------------

void TestIntFieldAccess() {
	BeginGateTest();

	// No entity field may be touched before the first map (PITFALLS B1).
	int value = 0;
	Check(!vip_infiniteammo::ReadIntField(77, 4336, value), "no field is read before the first map");
	Check(!vip_infiniteammo::WriteIntField(77, 4336, 30, -1), "no field is written before the first map");
	CheckEqInt(g_getEntDataCalls, 0, "no read happens with the gate closed");
	CheckEqInt(g_setEntDataCalls, 0, "no write happens with the gate closed");

	vip_infiniteammo::InitMapGate();
	FireMapStart();

	__s2sdk_GetEntData = nullptr;
	Check(!vip_infiniteammo::ReadIntField(77, 4336, value), "a missing read binding is refused");
	__s2sdk_GetEntData = &FakeGetEntData;
	Check(!vip_infiniteammo::ReadIntField(77, -1, value), "a negative offset is refused");
	CheckEqInt(g_getEntDataCalls, 0, "no read happens for an invalid request");

	g_entDataValue = 42;
	Check(vip_infiniteammo::ReadIntField(77, 4336, value), "the int32 field is read");
	CheckEqInt(value, 42, "the read value");
	CheckEqInt(g_lastGetHandle, 77, "the handle is forwarded");
	CheckEqInt(g_lastGetOffset, 4336, "the offset is forwarded");
	CheckEqInt(g_lastGetSize, 4, "an int32 is read");

	__s2sdk_SetEntData = nullptr;
	Check(!vip_infiniteammo::WriteIntField(77, 4336, 30, -1), "a missing write binding is refused");
	__s2sdk_SetEntData = &FakeSetEntData;
	Check(!vip_infiniteammo::WriteIntField(77, -1, 30, -1), "a negative offset is refused");
	CheckEqInt(g_setEntDataCalls, 0, "no write happens for an invalid request");

	Check(vip_infiniteammo::WriteIntField(77, 4336, 30, -1), "the int32 field is written");
	CheckEqInt(g_lastSetHandle, 77, "the write handle is forwarded");
	CheckEqInt(g_lastSetOffset, 4336, "the write offset is forwarded");
	CheckEqInt(g_lastSetValue, 30, "the write value");
	CheckEqInt(g_lastSetSize, 4, "an int32 is written");
	Check(g_lastSetChangeState, "the write is networked (the original's SetStateChanged)");
	CheckEqInt(g_lastSetChainOffset, -1, "the chain offset is forwarded (-1: the entity itself)");

	Check(vip_infiniteammo::WriteIntField(77, 4344, 180, 8), "the reserve field is written");
	CheckEqInt(g_lastSetOffset, 4344, "the inline m_pReserveAmmo offset");
	CheckEqInt(g_lastSetValue, 180, "the reserve value");
	CheckEqInt(g_lastSetChainOffset, 8, "a positive chain offset is forwarded unchanged");
}

} // namespace

int main() {
	std::printf("vip_infiniteammo tests\n");

	// Part 1: the engine-free logic.
	TestTargetMagazines();
	TestReserveTarget();
	TestInfiniteClip();
	TestExcluded();
	TestReserveAmmoAt();
	TestKv1Reader();
	TestPackTable();
	TestShippedConfig();
	TestConfigPaths();

	// Part 2: the guards with fake s2sdk bindings.
	TestMapGateLifecycle();
	TestMapGateWithoutListeners();
	TestIsAlive();
	TestUsableEntityHandle();
	TestSchemaOffsetGateAndCache();
	TestSchemaChainOffsetGateAndCache();
	TestSchemaCachePolicy();
	TestIntFieldAccess();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
