#pragma once

#include "Telemetry/Zone.h"
#include "Threads/ThreadRegistry.h"

namespace EngineTelemetry
{
	struct StackKey
	{
		ThreadRecord*                thread{ nullptr };
		bool                         blocked{ false };    // Suspended inside a syscall.
		bool                         truncated{ false };  // Unwind stopped before reaching the thread's root.
		std::uint32_t                zoneDepth{ 0 };      // Full depth; `zones` holds at most ZoneStack::kMaxDepth.
		std::vector<const ZoneSite*> zones;               // Outermost first.
		std::vector<std::uintptr_t>  frames;              // Leaf first; frames[0] is the interrupted PC.

		bool operator==(const StackKey&) const = default;
	};

	struct StackKeyHash
	{
		std::size_t operator()(const StackKey& a_key) const noexcept;
	};

	struct SampleSession
	{
		std::chrono::system_clock::time_point startedAt{ std::chrono::system_clock::now() };
		std::chrono::steady_clock::time_point startedSteady{ std::chrono::steady_clock::now() };
		std::uint32_t                         intervalUs{};
		std::uint64_t                         rounds{};
		std::uint64_t                         samples{};
		std::uint64_t                         droppedStacks{};
		double                                maxSuspendUs{};
		double                                totalSuspendUs{};

		std::unordered_map<StackKey, std::uint64_t, StackKeyHash> stacks;
	};

	namespace StackSampler
	{
		// Starts the background sampling thread. Call once, after the zone hooks
		// are installed.
		bool Start();
	}
}
