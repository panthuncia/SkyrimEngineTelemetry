#include "PCH.h"

#include "Hooks/FrameHooks.h"
#include "Hooks/ZoneHooks.h"
#include "Sampler/StackSampler.h"
#include "Settings.h"
#include "Telemetry/BasicTelemetrySession.h"
#include "Threads/ThreadHooks.h"
#include "Threads/ThreadRegistry.h"

namespace
{
	using namespace std::literals;

	constexpr auto         PLUGIN_NAME = "SkyrimEngineTelemetry"sv;
	constexpr REL::Version PLUGIN_VERSION{ SET_PLUGIN_VERSION_MAJOR, SET_PLUGIN_VERSION_MINOR, SET_PLUGIN_VERSION_PATCH, 0 };

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}

		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			// Install as early as possible so loading screens and the main menu
			// are covered too, not just gameplay after kDataLoaded.
			if (!EngineTelemetry::FrameHooks::Install()) {
				REX::CRITICAL("Failed to install frame hooks"sv);
			}
			EngineTelemetry::ZoneHooks::InstallFromFile();
			if (EngineTelemetry::Settings::Get().samplerEnabled) {
				EngineTelemetry::StackSampler::Start();
			}
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			if (EngineTelemetry::Settings::Get().logThreads) {
				auto& registry = EngineTelemetry::ThreadRegistry::Get();
				registry.Rescan();
				registry.LogTable();
			}
			break;
		default:
			break;
		}
	}
}

SKSEPluginVersion = []() {
	SKSE::PluginVersionData data;

	data.PluginVersion(PLUGIN_VERSION);
	data.PluginName(PLUGIN_NAME);
	data.AuthorName("Panthuncia"sv);
	data.UsesAddressLibrary();
	data.UsesUpdatedStructs();
	data.MinimumRequiredXSEVersion({ 2, 2, 3, 0 });

	return data;
}();

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	REX::INFO("Loading {} {}"sv, PLUGIN_NAME, PLUGIN_VERSION.string());

	EngineTelemetry::Settings::Load();
	EngineTelemetry::BasicTelemetrySession::Start();

	// Hook thread creation before the engine starts most of its threads, then
	// record whatever is already running.
	auto& registry = EngineTelemetry::ThreadRegistry::Get();
	if (!EngineTelemetry::ThreadHooks::Install()) {
		REX::WARN("Some thread-creation hooks failed; those threads will only be found by rescans"sv);
	}
	registry.Rescan();
	if (EngineTelemetry::Settings::Get().nameThreads) {
		registry.SetCurrentThreadName("Skyrim Main");
	}

	const auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
		REX::CRITICAL("Failed to register SKSE messaging listener"sv);
		return false;
	}

	return true;
}
