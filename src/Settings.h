#pragma once

namespace EngineTelemetry::Settings
{
	struct Values
	{
		// [Threads]
		bool nameThreads{ true };  // Set descriptions (and so Tracy names) on unnamed threads.
		bool logThreads{ true };   // Log the thread table at kDataLoaded.

		// [Sampler]
		bool          samplerEnabled{ false };
		std::uint32_t samplerIntervalUs{ 1'000 };  // One sampling round across all active threads per interval.
		std::uint32_t reportIntervalSec{ 30 };
	};

	// Reads SkyrimEngineTelemetry.ini next to the plugin DLL. Missing keys keep defaults.
	void Load();

	[[nodiscard]] const Values& Get() noexcept;

	[[nodiscard]] HMODULE PluginModule() noexcept;

	// The plugin DLL's path with its extension replaced, e.g. L".ini".
	[[nodiscard]] std::filesystem::path PluginSidecarPath(std::wstring_view a_extension);
}
