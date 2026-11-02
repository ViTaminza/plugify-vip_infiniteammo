// vip_infiniteammo — the map-gated schema-offset cache (PITFALLS C8).
//
// s2sdk's GetSchemaOffset / GetSchemaChainOffset build their field list from the network
// serializer map, which does not exist before the first map is up and is torn down between
// OnMapEnd and the next OnMapStart. Calling them in either window segfaults inside the schema
// system (real crash: game_fixes calling it from OnPluginStart). The plugin no longer goes through
// pisexcompat, so the cache the ammo code relies on lives here:
//
//   * with the plugin's map gate closed the cache answers -1 without consulting its own entries and
//     without calling s2sdk at all;
//   * only a *successful* field lookup (offset >= 0) is remembered. GetSchemaOffset answers -1 for
//     an unknown field, and caching that -1 would hide the field for the whole session - a lookup
//     that ran a frame too early is retried on the next call instead of poisoning the entry;
//   * a chain offset is only remembered when it is > 0. GetSchemaChainOffset answers -1 for "the
//     object itself is the entity" (or an unknown class), and s2sdk's SafeNetworkStateChanged
//     reads a chain offset >= 0 as "the object's __m_pChainEntity sits at object + offset" - 0
//     would make the engine treat the object's vtable as a chainer. So every "none" is normalised
//     to -1, which is exactly what the entity-field write needs for a weapon;
//   * Clear() is called from OnPluginEnd, so `plg reload` resolves the fields again instead of
//     reusing offsets a previous instance happened to have found.
//
// The lookups are injected as function pointers, so the test drives the whole policy (gate first,
// failures retried, successes memoised, Clear() forgets) without an engine.
//
// Header-only: included by src/guards.hpp and by tests/vip_infiniteammo_tests.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace vip_infiniteammo {

class SchemaOffsetCache {
public:
	// The engine-side lookups: s2sdk::GetSchemaOffset / GetSchemaChainOffset (-1 for "unknown").
	using OffsetLookup = int32_t (*)(const char* className, const char* fieldName);
	using ChainLookup = int32_t (*)(const char* className);

	// >= 0 offset of className::fieldName, or -1 when no map is up / the field is unknown / the
	// caller passed an empty name. A cached hit never calls `lookup`.
	int32_t Offset(bool mapReady, const char* className, const char* fieldName, OffsetLookup lookup) {
		if (className == nullptr || fieldName == nullptr || *className == '\0' || *fieldName == '\0' ||
			lookup == nullptr) {
			return -1;
		}
		if (!mapReady) {
			// GetSchemaOffset dereferences the null serializer map before the first map and
			// between OnMapEnd and the next OnMapStart (PITFALLS B1/B2, C8). Even a remembered
			// offset is not handed out here: the caller uses -1 as "no schema read in this window".
			return -1;
		}
		std::string key(className);
		key.push_back('\0');
		key.append(fieldName);
		if (const auto cached = m_offsets.find(key); cached != m_offsets.end()) {
			return cached->second;
		}
		const int32_t offset = lookup(className, fieldName);
		if (offset >= 0) {
			m_offsets.emplace(std::move(key), offset); // -1 stays retryable
		}
		return offset;
	}

	// > 0 offset of className's __m_pChainEntity, or -1 for every "none" (no map, unknown class,
	// offset <= 0). Only a positive offset is cached.
	int32_t ChainOffset(bool mapReady, const char* className, ChainLookup lookup) {
		if (className == nullptr || *className == '\0' || lookup == nullptr) {
			return -1;
		}
		if (!mapReady) {
			return -1;
		}
		const std::string key(className);
		if (const auto cached = m_chainOffsets.find(key); cached != m_chainOffsets.end()) {
			return cached->second;
		}
		const int32_t offset = lookup(className);
		if (offset <= 0) {
			return -1; // never cached: a non-positive answer stays retryable
		}
		m_chainOffsets.emplace(key, offset);
		return offset;
	}

	// Forgets everything; the plugin calls this in OnPluginEnd (a reload re-resolves the fields).
	void Clear() {
		m_offsets.clear();
		m_chainOffsets.clear();
	}

	// Bookkeeping for the tests.
	std::size_t CachedOffsetCount() const {
		return m_offsets.size();
	}
	std::size_t CachedChainOffsetCount() const {
		return m_chainOffsets.size();
	}

private:
	std::unordered_map<std::string, int32_t> m_offsets;      // class + '\0' + field -> offset >= 0
	std::unordered_map<std::string, int32_t> m_chainOffsets; // class -> offset > 0
};

} // namespace vip_infiniteammo
