#include "PCH.h"

#include "Telemetry/Zone.h"

#include "Threads/ThreadRegistry.h"

namespace EngineTelemetry
{
	ZoneStack* AcquireCurrentZoneStack() noexcept
	{
		// Leaked on purpose: the sampler may still hold a pointer to it after the
		// thread exits.
		auto* stack = new ZoneStack{};
		t_zoneStack = stack;
		ThreadRegistry::Get().AttachZoneStack(stack);
		return stack;
	}
}
