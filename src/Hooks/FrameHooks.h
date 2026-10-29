#pragma once

namespace EngineTelemetry::FrameHooks
{
	// Records the hooked functions' entries for HookGuard, at plugin load.
	void Snapshot();

	// Whole-function zones for the main-thread frame loop and top-level render
	// passes. Installed at kPostPostLoad; a function another plugin has hooked
	// is skipped.
	bool Install();
}
