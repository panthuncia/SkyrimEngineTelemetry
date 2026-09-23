#pragma once

// SET_ZONE opens a BasicTelemetry/Tracy zone and also records it on a per-thread
// zone stack that the stack sampler can read while the thread is suspended.
// BasicTelemetry's own scope state is thread_local and not visible to other
// threads, so every engine zone should use SET_ZONE rather than BT_ZONE_*.

namespace EngineTelemetry
{
	struct ZoneSite
	{
		const char* name;
		const char* category;
	};

	struct ZoneStack
	{
		static constexpr std::uint32_t kMaxDepth = 32;

		// Written only by the owning thread. `depth` may exceed kMaxDepth; entries
		// past the end are not stored.
		std::atomic<std::uint32_t> depth{ 0 };
		const ZoneSite*            sites[kMaxDepth]{};
	};

	// Slow path: allocates the calling thread's stack and registers it with the
	// ThreadRegistry. Never returns null.
	ZoneStack* AcquireCurrentZoneStack() noexcept;

	inline thread_local ZoneStack* t_zoneStack{ nullptr };

	// Returns the stack the zone was pushed to; pass it to PopZone.
	inline ZoneStack* PushZone(const ZoneSite& a_site) noexcept
	{
		auto* stack = t_zoneStack ? t_zoneStack : AcquireCurrentZoneStack();
		const auto depth = stack->depth.load(std::memory_order_relaxed);
		if (depth < ZoneStack::kMaxDepth) {
			stack->sites[depth] = &a_site;
		}
		stack->depth.store(depth + 1, std::memory_order_release);
		return stack;
	}

	inline void PopZone(ZoneStack* a_stack) noexcept
	{
		a_stack->depth.store(a_stack->depth.load(std::memory_order_relaxed) - 1, std::memory_order_release);
	}

	class ZoneStackGuard
	{
	public:
		explicit ZoneStackGuard(const ZoneSite& a_site) noexcept :
			_stack(PushZone(a_site))
		{}

		~ZoneStackGuard() { PopZone(_stack); }

		ZoneStackGuard(const ZoneStackGuard&) = delete;
		ZoneStackGuard& operator=(const ZoneStackGuard&) = delete;

	private:
		ZoneStack* _stack;
	};
}

#define SET_ZONE_JOIN_IMPL(a, b) a##b
#define SET_ZONE_JOIN(a, b) SET_ZONE_JOIN_IMPL(a, b)

#define SET_ZONE(name, category)                                                                      \
	static constexpr ::EngineTelemetry::ZoneSite SET_ZONE_JOIN(_setZoneSite, __LINE__){ name, category }; \
	const ::EngineTelemetry::ZoneStackGuard      SET_ZONE_JOIN(_setZoneGuard, __LINE__){ SET_ZONE_JOIN(_setZoneSite, __LINE__) }; \
	BT_ZONE_SCOPE_CATEGORY(name, category)
