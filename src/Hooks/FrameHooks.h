#pragma once

namespace EngineTelemetry::FrameHooks
{
	// Whole-function zones for the main-thread frame loop and top-level render
	// passes. Must be called after Address Library is available (kPostLoad or later).
	bool Install();
}
