#pragma once

namespace EngineTelemetry::ThreadHooks
{
	// Patches the game executable's CreateThread/_beginthread/_beginthreadex
	// imports so every engine thread passes through a start trampoline that
	// registers and names it, and installs a vectored handler that captures
	// names sent with the MSVC thread-naming exception. Call from SKSEPluginLoad.
	bool Install();

	[[nodiscard]] bool IsStartTrampoline(std::uintptr_t a_address) noexcept;
}
