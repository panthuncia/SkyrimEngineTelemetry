#pragma once

#include "Telemetry/Zone.h"

namespace EngineTelemetry
{
	enum class ThreadOrigin : std::uint8_t
	{
		Existing,    // Already running when the plugin loaded.
		Created,     // Created through a hooked CreateThread/_beginthread(ex) in the game exe.
		Discovered,  // Found by a later rescan (threads created by other modules).
	};

	[[nodiscard]] std::string_view ToString(ThreadOrigin a_origin) noexcept;

	struct ThreadSampleCounts
	{
		std::uint64_t idle{};      // No CPU cycles since the previous sample.
		std::uint64_t blocked{};   // Running since the last sample, but suspended inside a syscall.
		std::uint64_t zoned{};     // On-CPU inside at least one SET_ZONE.
		std::uint64_t unscoped{};  // On-CPU outside every SET_ZONE.
		std::uint64_t failed{};    // Suspend/context/unwind failure.
	};

	// Records are never freed, so raw pointers stay valid for the process
	// lifetime. Thread handles are likewise never closed.
	struct ThreadRecord
	{
		// Immutable after creation.
		DWORD          tid{};
		std::uint64_t  creationTime{};
		HANDLE         handle{ nullptr };
		std::uintptr_t stackReserveBase{};
		std::uintptr_t stackBase{};

		// Guarded by the registry mutex.
		ThreadOrigin   origin{ ThreadOrigin::Existing };
		std::uintptr_t startAddress{};  // The real routine, not our start trampoline.
		std::string    createdBy;       // Hooked API that created the thread, if any.
		std::string    rttiClass;       // RTTI class of the start parameter (e.g. a BSThread subclass).
		std::string    engineName;      // Name the engine gave via the MSVC thread-naming exception.
		std::string    description;     // GetThreadDescription at last scan.
		bool           autoNamed{ false };  // `description` was assigned by us and may be replaced.
		bool           excluded{ false };  // Plugin-internal (sampler, Tracy); never sampled.

		std::atomic<ZoneStack*> zoneStack{ nullptr };
		std::atomic<bool>       alive{ true };

		// Owned by the sampler thread.
		bool               cyclesInitialized{ false };
		std::uint64_t      firstCycles{};
		std::uint64_t      lastSampledCycles{};
		std::uint64_t      latestCycles{};
		ThreadSampleCounts counts;

		[[nodiscard]] std::string DisplayName() const;
	};

	class ThreadRegistry
	{
	public:
		[[nodiscard]] static ThreadRegistry& Get();

		// Called on the new thread by the thread-creation hooks' start trampoline.
		void OnThreadStarted(std::uintptr_t a_startAddress, std::string_view a_createdBy, std::string a_rttiClass);

		// Called by the creating thread as soon as the OS handle and TID exist. It
		// closes the race where a rescan observes our trampoline before the new
		// thread gets scheduled and reaches OnThreadStarted.
		void OnThreadCreated(DWORD a_tid, std::uintptr_t a_startAddress, std::string_view a_createdBy, std::string a_rttiClass);

		// Called on the current thread the first time it enters a SET_ZONE.
		void AttachZoneStack(ZoneStack* a_stack);

		void SetEngineName(DWORD a_tid, std::string a_name);
		void SetCurrentThreadName(std::string_view a_name);
		void MarkCurrentThreadExcluded();

		// Enumerates the process's threads: adds new ones, marks exited ones dead.
		void Rescan();

		[[nodiscard]] std::vector<ThreadRecord*> Records() const;

		// Live, non-excluded threads other than a_self.
		[[nodiscard]] std::vector<ThreadRecord*> SamplingTargets(DWORD a_self) const;
		[[nodiscard]] std::unique_lock<std::mutex> Lock() const { return std::unique_lock{ _mutex }; }

		void LogTable() const;

	private:
		ThreadRegistry() = default;

		ThreadRecord* FindOrCreateLocked(DWORD a_tid, ThreadOrigin a_origin);
		ThreadRecord* FindCurrentLocked(DWORD a_tid) const;
		void          NameIfUnnamedLocked(ThreadRecord& a_record, std::string_view a_name) const;

		mutable std::mutex                         _mutex;
		std::mutex                                 _scanMutex;
		std::unordered_map<DWORD, ThreadRecord*>   _live;
		std::vector<std::unique_ptr<ThreadRecord>> _records;
		bool                                       _initialScanDone{ false };
	};
}
