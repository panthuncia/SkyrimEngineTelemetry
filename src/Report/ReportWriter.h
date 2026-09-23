#pragma once

#include "Sampler/ModuleMap.h"
#include "Sampler/StackSampler.h"
#include "Sampler/Symbolizer.h"

namespace EngineTelemetry::ReportWriter
{
	// Writes the cumulative session as JSON (via a temp file and atomic rename).
	// Schema is documented in tools/attribution/README.md.
	bool Write(
		const std::filesystem::path& a_path,
		const SampleSession&         a_session,
		const ModuleMap&             a_modules,
		Symbolizer&                  a_symbolizer,
		std::string&                 a_error);
}
