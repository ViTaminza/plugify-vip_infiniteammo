#pragma once
// vip_infiniteammo — the plugin's own s2sdk safety guards (PITFALLS B1/B2, C2, C8).
//
// The plugin calls s2sdk directly (docs/PLUGIN_STANDARD.md §7: no pisexcompat), so every rule the
// old wrappers enforced lives here:
//
//   * Map gate (B1/B2). No client, entity or schema call is made before the first map and none in
//     the window from OnMapEnd until the next OnMapStart: the client list is torn down there and
//     s2sdk's client lookups read an emptied list, while the serializer map GetSchemaOffset walks
//     does not exist before the first map. InitMapGate() registers the s2sdk OnMapStart/OnMapEnd
//     listeners (OnPluginStart), PollMapGate() latches readiness on OnMapStart *or* on the first
//     OnPluginUpdate of a plugin loaded into a running map (when no OnMapEnd was seen),
//     ShutdownMapGate() removes the listeners again (OnPluginEnd, PITFALLS A7).
//     SetMapLifecycleCallbacks() lets the plugin reset its own per-map state (the once-per-map
//     VData log latch and the pending spawn flush) from the very same two events, without the gate
//     having to know about the plugin class.
//   * Client checks (C2). s2sdk exports do not validate their arguments, so IsAlive() layers the
//     map gate, the slot range, IsClientConnected, IsClientInGame, !IsClientSourceTV, a controller
//     entity pointer *and* handle, a pawn pointer *and* handle, and finally IsClientAlive. Nothing
//     below a failed layer reaches s2sdk.
//   * Entity handles (C1). An entity int is a CEntityHandle (index | serial << 15): 0 is the world,
//     -1 is INVALID_EHANDLE_INDEX, and a *valid* handle may be negative once the serial counter
//     passes 65535, so the numeric filter must not be "handle > 0".
//   * Schema offsets (C8). GetSchemaOffset/GetSchemaChainOffset are only called while the gate is
//     open; only a successful field offset (>= 0) and a positive chain offset (> 0) are cached, and
//     ClearSchemaOffsets() (OnPluginEnd) makes a reload resolve them again. A missing chain offset
//     is normalised to -1, the "the object itself is the entity" sentinel the field write needs.
//   * Field access. ReadIntField()/WriteIntField() wrap the handle-based s2sdk GetEntData/
//     SetEntData with the int32 width of every ammo field; the write passes changeState=true (the
//     equivalent of the original's SetStateChanged) and the class's chain offset.
//
// Header-only (inline): included by src/plugin.cpp and by tests/vip_infiniteammo_tests.cpp, which
// installs fake __s2sdk_* bindings and checks every layer without an engine.

#include <cstdint>
#include <string>

#include <plg/plugin.hpp>
#include <s2sdk/s2sdk.hpp>

#include "common.hpp"
#include "schema_cache.hpp"

namespace vip_infiniteammo {

// A plugin map-lifecycle hook, called by the OnMapStart/OnMapEnd listeners after the gate state has
// been updated. Plain function pointer (s2sdk listeners are C function pointers anyway) and
// noexcept in practice: the map-end hook runs while the client list is being torn down and must
// only drop state.
using MapLifecycleCallback = void (*)();

namespace map_gate_detail {

// The same three flags (and the same state machine) the sibling pure-s2sdk plugins keep:
//   g_mapReady        a map is live - the only state in which a client/entity/schema call is legal
//   g_mapStartSeen    an OnMapStart arrived since OnPluginStart / the last OnMapEnd
//   g_mapEndSeen      an OnMapEnd arrived since OnPluginStart
inline bool g_mapReady = false;
inline bool g_mapStartSeen = false;
inline bool g_mapEndSeen = false;
inline bool g_listenersRegistered = false;

// The plugin's own per-map state hooks (see guards.hpp's header comment).
inline MapLifecycleCallback g_onMapStart = nullptr;
inline MapLifecycleCallback g_onMapEnd = nullptr;

// s2sdk listeners: plain function pointers called on the game thread. Nothing here may throw and
// no client or entity call is made - only state changes. inline so that the plugin and the test
// share one address per program.
inline void OnMapStartListener() {
	// The engine calls this once the new map and its entity system exist: readiness is granted here
	// for everything that follows (PITFALLS B1). A plugin loaded into a running map never sees this
	// callback - PollMapGate() covers it.
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

inline void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart the client list is torn down and every
	// client query crashes - so the gate is dropped first and the hook only clears state.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

} // namespace map_gate_detail

// --- map gate ---------------------------------------------------------------------------------

// Installs (or clears) the plugin's per-map state hooks; pass nullptr to drop them on unload.
inline void SetMapLifecycleCallbacks(MapLifecycleCallback onMapStart, MapLifecycleCallback onMapEnd) {
	map_gate_detail::g_onMapStart = onMapStart;
	map_gate_detail::g_onMapEnd = onMapEnd;
}

// Registers the OnMapStart/OnMapEnd listeners. Touches no client; safe in OnPluginStart.
inline void InitMapGate() {
	map_gate_detail::g_mapReady = false;
	map_gate_detail::g_mapStartSeen = false;
	map_gate_detail::g_mapEndSeen = false;
	if (map_gate_detail::g_listenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		// Without both listeners nothing could ever close the gate again, so it is never opened
		// (fail safe, PITFALLS A1: log and stay inactive, never return Failed).
		LogError(std::string(kLogTag) + ": the s2sdk map listeners are unavailable - client calls stay disabled");
		return;
	}
	s2sdk::OnMapStart_Register(&map_gate_detail::OnMapStartListener);
	s2sdk::OnMapEnd_Register(&map_gate_detail::OnMapEndListener);
	map_gate_detail::g_listenersRegistered = true;
}

// Per-frame pump (OnPluginUpdate): opens the gate for a plugin that loaded into a running map.
inline void PollMapGate() {
	if (!map_gate_detail::g_listenersRegistered) {
		return;
	}
	// A plugin loaded into a running map never saw an OnMapStart: the first OnPluginUpdate after
	// OnPluginStart proves a live map (the host pumps updates from the server frame). After an
	// OnMapEnd only a new OnMapStart may re-open the gate - the host keeps pumping updates through
	// a changelevel, so "no OnMapEnd seen" must not be enough once one was seen.
	if (!map_gate_detail::g_mapReady &&
		(map_gate_detail::g_mapStartSeen || !map_gate_detail::g_mapEndSeen)) {
		map_gate_detail::g_mapReady = true;
	}
}

// Removes the map listeners and closes the gate; called from OnPluginEnd so `plg reload` leaves
// nothing pointing into the unloaded library (PITFALLS A7).
inline void ShutdownMapGate() {
	if (map_gate_detail::g_listenersRegistered) {
		map_gate_detail::g_listenersRegistered = false;
		if (__s2sdk_OnMapStart_Unregister) {
			s2sdk::OnMapStart_Unregister(&map_gate_detail::OnMapStartListener);
		}
		if (__s2sdk_OnMapEnd_Unregister) {
			s2sdk::OnMapEnd_Unregister(&map_gate_detail::OnMapEndListener);
		}
	}
	// Drop readiness last: nothing can reach a client through this plugin after OnPluginEnd.
	map_gate_detail::g_mapReady = false;
	map_gate_detail::g_mapStartSeen = false;
	map_gate_detail::g_mapEndSeen = false;
}

// True while a map is up and client/entity/schema calls are safe.
inline bool IsMapReady() {
	return map_gate_detail::g_mapReady;
}

// --- entity handles (PITFALLS C1/C2) ----------------------------------------------------------

// A usable (non-world, non-invalid, live) entity handle, only while a map is up: IsValidEntHandle
// walks the entity system, which is null before the first map (PITFALLS B1). The numeric filter
// drops 0 and -1 only: a valid handle can be negative once the serial counter passes 65535.
inline bool IsUsableEntityHandle(int32_t handle) {
	if (!IsMapReady() || handle == 0 || handle == -1 || __s2sdk_IsValidEntHandle == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsValidEntHandle(handle);
	} catch (...) {
		return false;
	}
}

// --- client checks (PITFALLS C2) --------------------------------------------------------------

// Alive means every layer s2sdk does not check itself: a live map, a slot in range, a connected
// client in game that is not the SourceTV proxy, a controller entity (pointer and handle) and a
// living pawn (pointer and handle). s2sdk::IsClientAlive dereferences the pawn, so it is asked last.
inline bool IsAlive(int32_t iSlot) {
	if (!IsMapReady() || !IsSlotInRange(iSlot)) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_IsClientAlive == nullptr ||
		__s2sdk_PlayerSlotToEntPointer == nullptr || __s2sdk_PlayerSlotToEntHandle == nullptr ||
		__s2sdk_GetClientPawn == nullptr || __s2sdk_EntPointerToEntHandle == nullptr ||
		__s2sdk_IsValidEntHandle == nullptr || __s2sdk_IsValidEntPointer == nullptr) {
		return false;
	}
	try {
		if (!s2sdk::IsClientConnected(iSlot) || !s2sdk::IsClientInGame(iSlot) ||
			s2sdk::IsClientSourceTV(iSlot)) {
			return false;
		}
		// The controller entity: the pointer must exist and its handle must be valid.
		if (s2sdk::PlayerSlotToEntPointer(iSlot) == nullptr) {
			return false;
		}
		if (!IsUsableEntityHandle(s2sdk::PlayerSlotToEntHandle(iSlot))) {
			return false;
		}
		// The pawn: s2sdk::GetClientPawn answers the pointer, EntPointerToEntHandle its handle.
		void* pawn = s2sdk::GetClientPawn(iSlot);
		if (pawn == nullptr) {
			return false;
		}
		if (!IsUsableEntityHandle(s2sdk::EntPointerToEntHandle(pawn)) || !s2sdk::IsValidEntPointer(pawn)) {
			return false;
		}
		return s2sdk::IsClientAlive(iSlot);
	} catch (...) {
		return false;
	}
}

// --- schema offsets (PITFALLS C8) --------------------------------------------------------------

inline SchemaOffsetCache& SchemaOffsets() {
	static SchemaOffsetCache cache; // one instance per program, shared by every caller
	return cache;
}

inline int32_t LookupSchemaOffset(const char* className, const char* fieldName) {
	if (__s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	try {
		return s2sdk::GetSchemaOffset(className, fieldName); // -1: not found
	} catch (...) {
		return -1;
	}
}

inline int32_t LookupSchemaChainOffset(const char* className) {
	if (__s2sdk_GetSchemaChainOffset == nullptr) {
		return -1;
	}
	try {
		return s2sdk::GetSchemaChainOffset(className); // -1: no chainer / unknown class
	} catch (...) {
		return -1;
	}
}

// The map-gated field lookup, the replacement of pisex::SchemaOffset: -1 while the gate is closed
// (the serializer map does not exist then), only offsets >= 0 cached.
inline int32_t SchemaOffset(const char* className, const char* fieldName) {
	if (!IsMapReady() || __s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	try {
		return SchemaOffsets().Offset(true, className, fieldName, &LookupSchemaOffset);
	} catch (...) {
		return -1; // the cache allocates; a weapon event may not unwind into the engine
	}
}

// The map-gated chain lookup: -1 for "the object itself is the entity" (and while the gate is
// closed), only a positive offset cached.
inline int32_t SchemaChainOffset(const char* className) {
	if (!IsMapReady() || __s2sdk_GetSchemaChainOffset == nullptr) {
		return -1;
	}
	try {
		return SchemaOffsets().ChainOffset(true, className, &LookupSchemaChainOffset);
	} catch (...) {
		return -1;
	}
}

// Called from OnPluginEnd so a reload resolves the fields against the live map again (PITFALLS C8).
inline void ClearSchemaOffsets() {
	SchemaOffsets().Clear();
}

// --- handle-based integer fields --------------------------------------------------------------

// Reads an int32 field of an entity handle (PITFALLS C3: handle first, never a raw entity pointer).
// Only legal while a map is up - the entity system is null before OnMapStart.
inline bool ReadIntField(int32_t handle, int32_t offset, int& out) {
	if (!IsMapReady() || offset < 0 || __s2sdk_GetEntData == nullptr) {
		return false;
	}
	try {
		out = static_cast<int>(s2sdk::GetEntData(handle, offset, kInt32Size));
		return true;
	} catch (...) {
		return false;
	}
}

// changeState=true is the s2sdk equivalent of the original's SetStateChanged: the engine sends the
// new value to the clients. chainOffset is the class's chain offset, or -1 for "the entity itself".
inline bool WriteIntField(int32_t handle, int32_t offset, int value, int32_t chainOffset) {
	if (!IsMapReady() || offset < 0 || __s2sdk_SetEntData == nullptr) {
		return false;
	}
	try {
		s2sdk::SetEntData(handle, offset, value, kInt32Size, /*changeState=*/true, chainOffset);
		return true;
	} catch (...) {
		return false;
	}
}

} // namespace vip_infiniteammo
