#pragma once

// Telemetry defers to other plugins: it never detours an engine function that
// another plugin has already hooked or patched, since some plugins refuse to
// install over a foreign hook. Targets are snapshotted at plugin load, before
// any plugin's post-load hooks, and checked again just before installing.

namespace EngineTelemetry::HookGuard
{
	// Records the bytes at a_target's entry as the engine's.
	void Snapshot(std::uintptr_t a_target);

	// Empty while a_target's entry is still the engine's; otherwise why it is
	// not (who hooked or patched it), for the log.
	[[nodiscard]] std::string TakenBy(std::uintptr_t a_target);
}
