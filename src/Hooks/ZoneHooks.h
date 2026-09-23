#pragma once

#include "Telemetry/Zone.h"

// Signature-agnostic zone hooks. A detour on any engine function opens a
// SET_ZONE-equivalent zone (BasicTelemetry + Tracy + sampler zone stack) around
// the call without knowing the function's signature; see ZoneHookStub.asm for
// the calling-convention details and limits.

namespace EngineTelemetry::ZoneHooks
{
	// Detours the function starting at a_target (which must be a function entry
	// in the game executable) with a zone named a_name. Installs in its own
	// Detours transaction so one bad target cannot take others down with it.
	bool Install(std::uintptr_t a_target, std::string_view a_name, std::string_view a_category);

	// Returns the stable site for an installed hook at exactly a_target. This is
	// used as a thread-root fallback when a long-lived root began while the hook
	// catalog was still being installed and therefore never crossed its detour.
	[[nodiscard]] const ZoneSite* FindInstalledSite(std::uintptr_t a_target) noexcept;

	// Reads SkyrimEngineTelemetry.zones next to the plugin DLL and installs a
	// zone hook for each valid line. Format, one zone per line:
	//   <id> <category> <name...>        id for the running game's runtime
	//   <seId>,<aeId> <category> <name...>
	// Blank lines and lines starting with '#' or ';' are ignored.
	void InstallFromFile();
}
