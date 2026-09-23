#include "PCH.h"

#include "Sampler/StackSampler.h"

#include "Report/ReportWriter.h"
#include "Hooks/ZoneHooks.h"
#include "Sampler/ModuleMap.h"
#include "Sampler/Symbolizer.h"
#include "Settings.h"
#include "Util/Memory.h"

namespace EngineTelemetry
{
	std::size_t StackKeyHash::operator()(const StackKey& a_key) const noexcept
	{
		std::uint64_t hash = 14695981039346656037ull;
		const auto    mix = [&](std::uint64_t a_value) {
            hash ^= a_value;
            hash *= 1099511628211ull;
		};

		mix(reinterpret_cast<std::uintptr_t>(a_key.thread));
		mix((a_key.blocked ? 1u : 0u) | (a_key.truncated ? 2u : 0u));
		mix(a_key.zoneDepth);
		for (const auto* zone : a_key.zones) {
			mix(reinterpret_cast<std::uintptr_t>(zone));
		}
		for (const auto frame : a_key.frames) {
			mix(frame);
		}
		return static_cast<std::size_t>(hash);
	}

	namespace
	{
		constexpr std::uint32_t kMaxFrames = 64;
		constexpr std::size_t   kMaxUniqueStacks = 250'000;
		constexpr auto          kRescanInterval = 1s;

		struct RawSample
		{
			std::uint32_t   zoneDepth;
			const ZoneSite* zones[ZoneStack::kMaxDepth];
			std::uint32_t   frameCount;
			std::uintptr_t  frames[kMaxFrames];
			bool            inSyscall;
			bool            truncated;
		};

		// Runs while the target is suspended: no allocation, no logging, and no
		// locks another thread could hold (the target may own the heap lock).
		bool CaptureUnguarded(HANDLE a_thread, const ThreadRecord& a_record, const ModuleMap& a_modules, RawSample& a_out)
		{
			CONTEXT context{};
			context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
			if (!GetThreadContext(a_thread, &context)) {
				return false;
			}

			a_out.zoneDepth = 0;
			a_out.frameCount = 0;
			a_out.inSyscall = false;
			a_out.truncated = false;

			if (const auto* stack = a_record.zoneStack.load(std::memory_order_acquire)) {
				const auto depth = stack->depth.load(std::memory_order_acquire);
				a_out.zoneDepth = depth;
				for (std::uint32_t i = 0; i < depth && i < ZoneStack::kMaxDepth; ++i) {
					a_out.zones[i] = stack->sites[i];
				}
			}

			std::uint8_t code[2]{};
			a_out.inSyscall = Util::SafeRead(reinterpret_cast<const void*>(context.Rip - 2), code, sizeof(code)) &&
			                  code[0] == 0x0F && code[1] == 0x05;

			const auto stackLow = a_record.stackReserveBase;
			const auto stackHigh = a_record.stackBase;
			const bool haveBounds = stackLow && stackHigh > stackLow;

			for (;;) {
				const auto pc = static_cast<std::uintptr_t>(context.Rip);
				if (!pc) {
					break;
				}
				if (a_out.frameCount == kMaxFrames) {
					a_out.truncated = true;
					break;
				}
				a_out.frames[a_out.frameCount++] = pc;

				if (haveBounds && (context.Rsp < stackLow || context.Rsp >= stackHigh)) {
					a_out.truncated = true;
					break;
				}

				const bool     leaf = a_out.frameCount == 1;
				std::uintptr_t imageBase{ 0 };
				const auto*    function = a_modules.LookupFunctionEntry(leaf ? pc : pc - 1, imageBase);
				if (!function) {
					// Only the interrupted frame may be a leaf function without
					// unwind data; its return address is at [rsp].
					DWORD64 returnAddress{ 0 };
					if (!leaf || !Util::SafeRead(reinterpret_cast<const void*>(context.Rsp), returnAddress)) {
						a_out.truncated = true;
						break;
					}
					context.Rip = returnAddress;
					context.Rsp += 8;
					continue;
				}

				const auto previousRsp = context.Rsp;
				PVOID      handlerData{ nullptr };
				DWORD64    establisherFrame{ 0 };
				RtlVirtualUnwind(
					UNW_FLAG_NHANDLER,
					imageBase,
					pc,
					const_cast<PRUNTIME_FUNCTION>(function),
					&context,
					&handlerData,
					&establisherFrame,
					nullptr);

				if (context.Rsp <= previousRsp) {
					a_out.truncated = context.Rip != 0;
					break;
				}
			}
			return true;
		}

		bool Capture(HANDLE a_thread, const ThreadRecord& a_record, const ModuleMap& a_modules, RawSample& a_out) noexcept
		{
			__try {
				return CaptureUnguarded(a_thread, a_record, a_modules, a_out);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		std::filesystem::path ReportPath(std::chrono::system_clock::time_point a_startedAt)
		{
			auto directory = SKSE::log::log_directory().value_or(std::filesystem::current_path());
			directory /= "SkyrimEngineTelemetry";

			const auto seconds = std::chrono::floor<std::chrono::seconds>(a_startedAt);
			try {
				const auto local = std::chrono::zoned_time{ std::chrono::current_zone(), seconds };
				return directory / std::format("attribution-{:%Y%m%d-%H%M%S}.json", local);
			} catch (const std::exception&) {
				// No time zone database; fall back to UTC.
				return directory / std::format("attribution-{:%Y%m%d-%H%M%S}Z.json", seconds);
			}
		}

		class Sampler
		{
		public:
			void Run()
			{
				ThreadRegistry::Get().MarkCurrentThreadExcluded();
				ThreadRegistry::Get().SetCurrentThreadName("SkyrimEngineTelemetry Sampler");

				// Keep suspension windows short: never get preempted mid-sample.
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

				_session.intervalUs = Settings::Get().samplerIntervalUs;
				_reportPath = ReportPath(_session.startedAt);
				_symbolizer.LoadAddressLibrary();

				const auto timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
				if (!timer) {
					REX::WARN("High-resolution timer unavailable ({}); sampling interval will be coarse", GetLastError());
				}

				REX::INFO("Sampler running every {} us; reports go to {}", _session.intervalUs, _reportPath.string());

				const auto reportInterval = std::chrono::seconds{ Settings::Get().reportIntervalSec };
				auto       nextRescan = std::chrono::steady_clock::now();
				auto       nextReport = nextRescan + reportInterval;

				for (;;) {
					const auto roundStart = std::chrono::steady_clock::now();
					if (roundStart >= nextRescan) {
						RefreshTargets();
						nextRescan = roundStart + kRescanInterval;
					}

					for (auto* record : _targets) {
						SampleThread(*record);
					}
					++_session.rounds;

					if (roundStart >= nextReport) {
						WriteReport();
						nextReport = std::chrono::steady_clock::now() + reportInterval;
					}

					Wait(timer, roundStart);
				}
			}

		private:
			void Wait(HANDLE a_timer, std::chrono::steady_clock::time_point a_roundStart) const
			{
				const auto elapsed = std::chrono::steady_clock::now() - a_roundStart;
				const auto remaining = std::chrono::microseconds{ _session.intervalUs } - elapsed;
				if (remaining <= 0us) {
					return;
				}

				if (a_timer) {
					LARGE_INTEGER due{};
					due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100;
					if (SetWaitableTimer(a_timer, &due, 0, nullptr, nullptr, FALSE)) {
						WaitForSingleObject(a_timer, INFINITE);
						return;
					}
				}
				Sleep(static_cast<DWORD>(std::max<long long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count())));
			}

			void RefreshTargets()
			{
				auto& registry = ThreadRegistry::Get();
				registry.Rescan();
				_modules.Refresh();

				_targets = registry.SamplingTargets(GetCurrentThreadId());
			}

			void SampleThread(ThreadRecord& a_record)
			{
				ULONG64 cycles{ 0 };
				if (!QueryThreadCycleTime(a_record.handle, &cycles)) {
					return;
				}

				a_record.latestCycles = cycles;
				if (!a_record.cyclesInitialized) {
					a_record.cyclesInitialized = true;
					a_record.firstCycles = cycles;
					a_record.lastSampledCycles = cycles;
					return;
				}

				// No CPU time since the last sample: the thread is parked, and its
				// stack would only show the wait.
				if (cycles == a_record.lastSampledCycles) {
					++a_record.counts.idle;
					return;
				}
				a_record.lastSampledCycles = cycles;

				const auto suspendStart = std::chrono::steady_clock::now();
				if (SuspendThread(a_record.handle) == static_cast<DWORD>(-1)) {
					++a_record.counts.failed;
					return;
				}
				const bool captured = Capture(a_record.handle, a_record, _modules, _raw);
				ResumeThread(a_record.handle);

				const auto suspendUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - suspendStart).count();
				_session.totalSuspendUs += suspendUs;
				_session.maxSuspendUs = std::max(_session.maxSuspendUs, suspendUs);

				if (!captured || _raw.frameCount == 0) {
					++a_record.counts.failed;
					return;
				}
				Record(a_record);
			}

			void Record(ThreadRecord& a_record)
			{
				// A long-lived thread root can begin while its hook is being installed.
				// It will never cross that entry detour, so supply the same stable site
				// as an ambient root whenever no real nested zone is active.
				if (_raw.zoneDepth == 0) {
					if (const auto* root = ZoneHooks::FindInstalledSite(a_record.startAddress)) {
						_raw.zones[0] = root;
						_raw.zoneDepth = 1;
					}
				}

				++_session.samples;
				if (_raw.inSyscall) {
					++a_record.counts.blocked;
				} else if (_raw.zoneDepth > 0) {
					++a_record.counts.zoned;
				} else {
					++a_record.counts.unscoped;
				}

				StackKey key;
				key.thread = &a_record;
				key.blocked = _raw.inSyscall;
				key.truncated = _raw.truncated;
				key.zoneDepth = _raw.zoneDepth;
				key.zones.assign(_raw.zones, _raw.zones + std::min(_raw.zoneDepth, ZoneStack::kMaxDepth));
				key.frames.assign(_raw.frames, _raw.frames + _raw.frameCount);

				if (const auto it = _session.stacks.find(key); it != _session.stacks.end()) {
					++it->second;
				} else if (_session.stacks.size() < kMaxUniqueStacks) {
					_session.stacks.emplace(std::move(key), 1);
				} else {
					++_session.droppedStacks;
				}
			}

			void WriteReport()
			{
				// Serializing can take a while; don't hold a core at time-critical
				// priority while doing it. Nothing is suspended at this point.
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

				ThreadRegistry::Get().Rescan();
				_modules.Refresh();

				std::string error;
				if (!ReportWriter::Write(_reportPath, _session, _modules, _symbolizer, error)) {
					REX::WARN("Failed to write attribution report: {}", error);
				}

				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			}

			ModuleMap                  _modules;
			Symbolizer                 _symbolizer{ _modules };
			SampleSession              _session;
			std::vector<ThreadRecord*> _targets;
			RawSample                  _raw{};
			std::filesystem::path      _reportPath;
		};

		std::atomic_bool g_started{ false };
	}

	namespace StackSampler
	{
		bool Start()
		{
			if (g_started.exchange(true)) {
				return true;
			}

			try {
				std::thread([] {
					static Sampler sampler;
					sampler.Run();
				}).detach();
			} catch (const std::system_error& e) {
				REX::ERROR("Failed to start sampler thread: {}", e.what());
				return false;
			}
			return true;
		}
	}
}
