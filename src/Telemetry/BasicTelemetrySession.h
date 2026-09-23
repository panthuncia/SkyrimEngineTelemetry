#pragma once

namespace EngineTelemetry::BasicTelemetrySession
{
	// Starts the process-wide BasicTelemetry session when enabled in the INI.
	// Safe to call more than once.
	void Start();
}
