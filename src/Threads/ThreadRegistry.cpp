#include "PCH.h"

#include "Threads/ThreadRegistry.h"

#include "Settings.h"
#include "Threads/ThreadHooks.h"
#include "Util/Memory.h"

namespace EngineTelemetry
{
	namespace
	{
		constexpr ULONG kThreadBasicInformation = 0;
		constexpr ULONG kThreadQuerySetWin32StartAddress = 9;

		// x64 TEB offsets.
		constexpr std::size_t kTebStackBase = 0x08;
		constexpr std::size_t kTebDeallocationStack = 0x1478;

		constexpr DWORD kThreadAccess =
			THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SET_LIMITED_INFORMATION;

		struct ThreadBasicInformation
		{
			LONG      exitStatus;
			PVOID     tebBaseAddress;
			HANDLE    uniqueProcess;
			HANDLE    uniqueThread;
			ULONG_PTR affinityMask;
			LONG      priority;
			LONG      basePriority;
		};

		using NtQueryInformationThread_t = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

		NtQueryInformationThread_t NtQueryInformationThreadFn()
		{
			static const auto fn = reinterpret_cast<NtQueryInformationThread_t>(
				GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
			return fn;
		}

		std::uint64_t CreationTime(HANDLE a_thread)
		{
			FILETIME creation{}, exit{}, kernel{}, user{};
			if (!GetThreadTimes(a_thread, &creation, &exit, &kernel, &user)) {
				return 0;
			}
			return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
		}

		std::uintptr_t StartAddress(HANDLE a_thread)
		{
			const auto query = NtQueryInformationThreadFn();
			PVOID      start{ nullptr };
			if (!query || query(a_thread, kThreadQuerySetWin32StartAddress, &start, sizeof(start), nullptr) < 0) {
				return 0;
			}
			return reinterpret_cast<std::uintptr_t>(start);
		}

		void StackBounds(HANDLE a_thread, std::uintptr_t& a_reserveBase, std::uintptr_t& a_stackBase)
		{
			const auto             query = NtQueryInformationThreadFn();
			ThreadBasicInformation info{};
			if (!query || query(a_thread, kThreadBasicInformation, &info, sizeof(info), nullptr) < 0 || !info.tebBaseAddress) {
				return;
			}

			const auto teb = reinterpret_cast<std::uintptr_t>(info.tebBaseAddress);
			std::ignore = Util::SafeRead(reinterpret_cast<const void*>(teb + kTebStackBase), a_stackBase);
			std::ignore = Util::SafeRead(reinterpret_cast<const void*>(teb + kTebDeallocationStack), a_reserveBase);
		}

		std::string Description(HANDLE a_thread)
		{
			PWSTR       text{ nullptr };
			std::string result;
			if (SUCCEEDED(GetThreadDescription(a_thread, &text)) && text) {
				result = Util::WideToUtf8(text);
				LocalFree(text);
			}
			return result;
		}

		bool IsPluginInternalStart(std::uintptr_t a_start)
		{
			return Util::PluginImage().Contains(a_start) && !ThreadHooks::IsStartTrampoline(a_start);
		}
	}

	std::string_view ToString(ThreadOrigin a_origin) noexcept
	{
		switch (a_origin) {
		case ThreadOrigin::Existing:
			return "existing"sv;
		case ThreadOrigin::Created:
			return "created"sv;
		case ThreadOrigin::Discovered:
			return "discovered"sv;
		}
		return "unknown"sv;
	}

	std::string ThreadRecord::DisplayName() const
	{
		if (!engineName.empty()) {
			return engineName;
		}
		if (!description.empty()) {
			return description;
		}
		if (!rttiClass.empty()) {
			return rttiClass;
		}
		return startAddress ? Util::DescribeAddress(startAddress) : std::format("tid {}", tid);
	}

	ThreadRegistry& ThreadRegistry::Get()
	{
		static ThreadRegistry registry;
		return registry;
	}

	ThreadRecord* ThreadRegistry::FindCurrentLocked(DWORD a_tid) const
	{
		const auto it = _live.find(a_tid);
		return it != _live.end() ? it->second : nullptr;
	}

	ThreadRecord* ThreadRegistry::FindOrCreateLocked(DWORD a_tid, ThreadOrigin a_origin)
	{
		const auto handle = OpenThread(kThreadAccess, FALSE, a_tid);
		if (!handle) {
			return nullptr;
		}

		const auto creationTime = CreationTime(handle);
		if (auto* existing = FindCurrentLocked(a_tid)) {
			if (existing->creationTime == creationTime) {
				CloseHandle(handle);
				return existing;
			}
			// The TID was reused by a new thread.
			existing->alive.store(false, std::memory_order_relaxed);
		}

		auto record = std::make_unique<ThreadRecord>();
		record->tid = a_tid;
		record->creationTime = creationTime;
		record->handle = handle;
		record->origin = a_origin;
		record->startAddress = StartAddress(handle);
		record->description = Description(handle);
		record->excluded = IsPluginInternalStart(record->startAddress);
		StackBounds(handle, record->stackReserveBase, record->stackBase);

		auto* raw = record.get();
		_records.push_back(std::move(record));
		_live[a_tid] = raw;
		return raw;
	}

	void ThreadRegistry::NameIfUnnamedLocked(ThreadRecord& a_record, std::string_view a_name) const
	{
		if (!Settings::Get().nameThreads || a_record.excluded || a_name.empty() ||
			(!a_record.description.empty() && !a_record.autoNamed)) {
			return;
		}

		if (SUCCEEDED(SetThreadDescription(a_record.handle, Util::Utf8ToWide(a_name).c_str()))) {
			a_record.description = a_name;
			a_record.autoNamed = true;
		}
	}

	void ThreadRegistry::OnThreadStarted(std::uintptr_t a_startAddress, std::string_view a_createdBy, std::string a_rttiClass)
	{
		OnThreadCreated(GetCurrentThreadId(), a_startAddress, a_createdBy, std::move(a_rttiClass));
	}

	void ThreadRegistry::OnThreadCreated(DWORD a_tid, std::uintptr_t a_startAddress, std::string_view a_createdBy, std::string a_rttiClass)
	{
		const std::scoped_lock lock{ _mutex };

		auto* record = FindOrCreateLocked(a_tid, ThreadOrigin::Created);
		if (!record) {
			return;
		}

		// A rescan may have seen this thread first (with our trampoline as its
		// start address), so always overwrite with the real details.
		record->origin = ThreadOrigin::Created;
		record->startAddress = a_startAddress;
		record->createdBy = a_createdBy;
		record->rttiClass = std::move(a_rttiClass);
		record->excluded = false;

		NameIfUnnamedLocked(*record, record->rttiClass.empty() ? Util::DescribeAddress(a_startAddress) : record->rttiClass);
	}

	void ThreadRegistry::AttachZoneStack(ZoneStack* a_stack)
	{
		const std::scoped_lock lock{ _mutex };
		if (auto* record = FindOrCreateLocked(GetCurrentThreadId(), _initialScanDone ? ThreadOrigin::Discovered : ThreadOrigin::Existing)) {
			record->zoneStack.store(a_stack, std::memory_order_release);
		}
	}

	void ThreadRegistry::SetEngineName(DWORD a_tid, std::string a_name)
	{
		const std::scoped_lock lock{ _mutex };
		if (auto* record = FindOrCreateLocked(a_tid, _initialScanDone ? ThreadOrigin::Discovered : ThreadOrigin::Existing)) {
			if (Settings::Get().nameThreads && !record->excluded &&
				SUCCEEDED(SetThreadDescription(record->handle, Util::Utf8ToWide(a_name).c_str()))) {
				record->description = a_name;
				record->autoNamed = false;
			}
			record->engineName = std::move(a_name);
		}
	}

	void ThreadRegistry::SetCurrentThreadName(std::string_view a_name)
	{
		const std::scoped_lock lock{ _mutex };
		if (auto* record = FindOrCreateLocked(GetCurrentThreadId(), _initialScanDone ? ThreadOrigin::Discovered : ThreadOrigin::Existing)) {
			if (SUCCEEDED(SetThreadDescription(record->handle, Util::Utf8ToWide(a_name).c_str()))) {
				record->description = a_name;
				record->autoNamed = false;
			}
		}
	}

	void ThreadRegistry::MarkCurrentThreadExcluded()
	{
		const std::scoped_lock lock{ _mutex };
		if (auto* record = FindOrCreateLocked(GetCurrentThreadId(), ThreadOrigin::Discovered)) {
			record->excluded = true;
		}
	}

	void ThreadRegistry::Rescan()
	{
		const std::scoped_lock scanLock{ _scanMutex };

		const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			REX::WARN("CreateToolhelp32Snapshot failed: {}", GetLastError());
			return;
		}

		std::vector<DWORD> tids;
		const auto         pid = GetCurrentProcessId();
		THREADENTRY32      entry{ .dwSize = sizeof(THREADENTRY32) };
		for (auto ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
			if (entry.th32OwnerProcessID == pid) {
				tids.push_back(entry.th32ThreadID);
			}
		}
		CloseHandle(snapshot);

		const std::scoped_lock lock{ _mutex };
		const auto             origin = _initialScanDone ? ThreadOrigin::Discovered : ThreadOrigin::Existing;

		for (const auto tid : tids) {
			const bool known = _live.contains(tid);
			auto*      record = FindOrCreateLocked(tid, origin);
			if (!record) {
				continue;
			}

			record->description = Description(record->handle);
			if (!known && !ThreadHooks::IsStartTrampoline(record->startAddress) && record->startAddress) {
				NameIfUnnamedLocked(*record, Util::DescribeAddress(record->startAddress));
			}
		}

		for (auto it = _live.begin(); it != _live.end();) {
			if (std::ranges::find(tids, it->first) == tids.end()) {
				it->second->alive.store(false, std::memory_order_relaxed);
				it = _live.erase(it);
			} else {
				++it;
			}
		}

		_initialScanDone = true;
	}

	std::vector<ThreadRecord*> ThreadRegistry::Records() const
	{
		const std::scoped_lock lock{ _mutex };

		std::vector<ThreadRecord*> result;
		result.reserve(_records.size());
		for (const auto& record : _records) {
			result.push_back(record.get());
		}
		return result;
	}

	std::vector<ThreadRecord*> ThreadRegistry::SamplingTargets(DWORD a_self) const
	{
		const std::scoped_lock lock{ _mutex };

		std::vector<ThreadRecord*> result;
		for (const auto& [tid, record] : _live) {
			if (tid != a_self && !record->excluded && record->handle && record->alive.load(std::memory_order_relaxed)) {
				result.push_back(record);
			}
		}
		return result;
	}

	void ThreadRegistry::LogTable() const
	{
		const std::scoped_lock lock{ _mutex };

		REX::INFO("{} live threads:", _live.size());
		for (const auto& record : _records) {
			if (!record->alive.load(std::memory_order_relaxed)) {
				continue;
			}
			REX::INFO(
				"  tid {:>6} {:<10} {:<40} start={}{}{}{}",
				record->tid,
				ToString(record->origin),
				record->DisplayName(),
				record->startAddress ? Util::DescribeAddress(record->startAddress) : "?"s,
				record->rttiClass.empty() ? ""s : std::format(" rtti={}", record->rttiClass),
				record->createdBy.empty() ? ""s : std::format(" via={}", record->createdBy),
				record->excluded ? " (internal)"s : ""s);
		}
	}
}
