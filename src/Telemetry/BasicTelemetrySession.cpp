#include "PCH.h"

#include "Telemetry/BasicTelemetrySession.h"

#include "Settings.h"
#include "Threads/ThreadRegistry.h"

#include <BasicTelemetry/Artifacts.h>

#include <condition_variable>

namespace EngineTelemetry::BasicTelemetrySession
{
	namespace
	{
		class Runtime
		{
		public:
			explicit Runtime(const Settings::Values& a_settings) :
				_options{
					.outputDirectory = ResolveOutputDirectory(a_settings),
					.writeSqlite = a_settings.basicTelemetryWriteSqlite,
					.writeMarkdown = a_settings.basicTelemetryWriteMarkdown },
				_interval{ a_settings.basicTelemetrySnapshotIntervalSec }
			{
				basic_telemetry::SessionConfig config;
				config.mode = a_settings.basicTelemetryTrace ? basic_telemetry::CaptureMode::Trace : basic_telemetry::CaptureMode::Summary;
				config.maximumTraceEvents = a_settings.basicTelemetryMaximumTraceEvents;
				config.measureThreadCpuTime = a_settings.basicTelemetryMeasureThreadCpuTime;
				config.metadata.emplace("application", "Skyrim Special Edition");
				config.metadata.emplace("plugin", "SkyrimEngineTelemetry");
				config.metadata.emplace("plugin_version", std::format("{}.{}.{}", SET_PLUGIN_VERSION_MAJOR, SET_PLUGIN_VERSION_MINOR, SET_PLUGIN_VERSION_PATCH));

				_session = std::make_unique<basic_telemetry::Session>(
					std::move(config),
					std::vector<std::shared_ptr<basic_telemetry::Sink>>{ basic_telemetry::MakeArtifactSink(_options) });
				_writer = std::jthread([this](std::stop_token a_stop) { WriterLoop(a_stop); });
				REX::INFO("BasicTelemetry {} capture writing to {} every {} seconds",
					a_settings.basicTelemetryTrace ? "Trace" : "Summary", _options.outputDirectory.string(), _interval.count());
			}

			~Runtime()
			{
				_writer.request_stop();
				_wake.notify_all();
				if (_writer.joinable()) {
					_writer.join();
				}
				_session.reset();
			}

		private:
			static std::filesystem::path ResolveOutputDirectory(const Settings::Values& a_settings)
			{
				if (!a_settings.basicTelemetryOutputDirectory.empty()) {
					return a_settings.basicTelemetryOutputDirectory;
				}
				auto directory = Settings::PluginSidecarPath(L"").parent_path() / "SkyrimEngineTelemetry-BasicTelemetry";
				return directory;
			}

			void WriterLoop(std::stop_token a_stop)
			{
				ThreadRegistry::Get().SetCurrentThreadName("SET Artifact Writer");
				std::unique_lock lock{ _mutex };
				while (!_wake.wait_for(lock, a_stop, _interval, [] { return false; })) {
					lock.unlock();
					std::string error;
					if (!basic_telemetry::WriteArtifacts(_session->Snapshot(), _options, &error)) {
						REX::ERROR("BasicTelemetry artifact write failed: {}", error);
					}
					lock.lock();
				}
			}

			basic_telemetry::ArtifactOptions          _options;
			std::chrono::seconds                      _interval;
			std::unique_ptr<basic_telemetry::Session> _session;
			std::jthread                              _writer;
			std::mutex                                _mutex;
			std::condition_variable_any               _wake;
		};
	}

	void Start()
	{
		static std::unique_ptr<Runtime> runtime;
		const auto& settings = Settings::Get();
		if (settings.basicTelemetryEnabled && !runtime) {
			runtime = std::make_unique<Runtime>(settings);
		}
	}
}
