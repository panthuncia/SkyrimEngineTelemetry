#include "PCH.h"

#include "Threads/ThreadHooks.h"

#include "Threads/ThreadRegistry.h"
#include "Util/Memory.h"

namespace EngineTelemetry::ThreadHooks
{
	namespace
	{
		constexpr DWORD kMsvcThreadNameException = 0x406D1388;

		struct StartInfo
		{
			std::uintptr_t routine;
			void*          parameter;
			const char*    createdBy;
		};

		void OnStart(const StartInfo& a_info)
		{
			ThreadRegistry::Get().OnThreadStarted(a_info.routine, a_info.createdBy, Util::GameRttiClassName(a_info.parameter));
		}

		void OnCreated(std::uintptr_t a_thread, const StartInfo& a_info)
		{
			const auto tid = GetThreadId(reinterpret_cast<HANDLE>(a_thread));
			if (tid != 0) {
				ThreadRegistry::Get().OnThreadCreated(tid, a_info.routine, a_info.createdBy, Util::GameRttiClassName(a_info.parameter));
			}
		}

		StartInfo TakeStartInfo(void* a_info)
		{
			const std::unique_ptr<StartInfo> info{ static_cast<StartInfo*>(a_info) };
			OnStart(*info);
			return *info;
		}

		DWORD WINAPI ThreadStart(LPVOID a_info)
		{
			const auto info = TakeStartInfo(a_info);
			return reinterpret_cast<LPTHREAD_START_ROUTINE>(info.routine)(info.parameter);
		}

		unsigned __stdcall ThreadStartEx(void* a_info)
		{
			const auto info = TakeStartInfo(a_info);
			return reinterpret_cast<unsigned(__stdcall*)(void*)>(info.routine)(info.parameter);
		}

		void __cdecl ThreadStartVoid(void* a_info)
		{
			const auto info = TakeStartInfo(a_info);
			reinterpret_cast<void(__cdecl*)(void*)>(info.routine)(info.parameter);
		}

		using CreateThread_t = decltype(&::CreateThread);
		using BeginThreadEx_t = std::uintptr_t(__cdecl*)(void*, unsigned, unsigned(__stdcall*)(void*), void*, unsigned, unsigned*);
		using BeginThread_t = std::uintptr_t(__cdecl*)(void(__cdecl*)(void*), unsigned, void*);

		CreateThread_t  g_createThread{ nullptr };
		BeginThreadEx_t g_beginThreadEx{ nullptr };
		BeginThread_t   g_beginThread{ nullptr };

		HANDLE WINAPI Hook_CreateThread(
			LPSECURITY_ATTRIBUTES  a_security,
			SIZE_T                 a_stackSize,
			LPTHREAD_START_ROUTINE a_routine,
			LPVOID                 a_parameter,
			DWORD                  a_flags,
			LPDWORD                a_threadId)
		{
			auto* info = new (std::nothrow) StartInfo{ reinterpret_cast<std::uintptr_t>(a_routine), a_parameter, "CreateThread" };
			if (!info) {
				return g_createThread(a_security, a_stackSize, a_routine, a_parameter, a_flags, a_threadId);
			}
			const auto createdInfo = *info;

			const auto thread = g_createThread(a_security, a_stackSize, ThreadStart, info, a_flags, a_threadId);
			if (!thread) {
				delete info;
			} else {
				OnCreated(reinterpret_cast<std::uintptr_t>(thread), createdInfo);
			}
			return thread;
		}

		std::uintptr_t __cdecl Hook_beginthreadex(
			void*    a_security,
			unsigned a_stackSize,
			unsigned(__stdcall* a_routine)(void*),
			void*     a_parameter,
			unsigned  a_flags,
			unsigned* a_threadId)
		{
			auto* info = new (std::nothrow) StartInfo{ reinterpret_cast<std::uintptr_t>(a_routine), a_parameter, "_beginthreadex" };
			if (!info) {
				return g_beginThreadEx(a_security, a_stackSize, a_routine, a_parameter, a_flags, a_threadId);
			}
			const auto createdInfo = *info;

			const auto thread = g_beginThreadEx(a_security, a_stackSize, ThreadStartEx, info, a_flags, a_threadId);
			if (thread == 0) {
				delete info;
			} else {
				OnCreated(thread, createdInfo);
			}
			return thread;
		}

		std::uintptr_t __cdecl Hook_beginthread(void(__cdecl* a_routine)(void*), unsigned a_stackSize, void* a_parameter)
		{
			auto* info = new (std::nothrow) StartInfo{ reinterpret_cast<std::uintptr_t>(a_routine), a_parameter, "_beginthread" };
			if (!info) {
				return g_beginThread(a_routine, a_stackSize, a_parameter);
			}
			const auto createdInfo = *info;

			const auto thread = g_beginThread(ThreadStartVoid, a_stackSize, info);
			if (thread == static_cast<std::uintptr_t>(-1)) {
				delete info;
			} else {
				OnCreated(thread, createdInfo);
			}
			return thread;
		}

		template <class T>
		bool PatchImport(const REX::FModule& a_module, std::string_view a_library, std::string_view a_function, T a_hook, T& a_original)
		{
			// FModule::SetImportFunctionPointer returns the IAT slot, not the
			// previous target, so read the slot before overwriting it.
			const auto slot = static_cast<void**>(a_module.GetImportFunctionPointer(a_function, a_library));
			if (!slot || !*slot) {
				REX::WARN("Import {}!{} not found; threads created through it will not be tracked", a_library, a_function);
				return false;
			}

			a_original = reinterpret_cast<T>(*slot);
			if (!REL::WriteSafeData(slot, reinterpret_cast<void*>(a_hook))) {
				REX::ERROR("Failed to patch import {}!{}", a_library, a_function);
				a_original = nullptr;
				return false;
			}

			REX::INFO("Patched import {}!{}", a_library, a_function);
			return true;
		}

		// Engine code may name threads with the debugger exception; it is raised
		// whether or not a debugger is attached, so a vectored handler sees it
		// before the engine's own __except swallows it.
		LONG CALLBACK ThreadNameHandler(EXCEPTION_POINTERS* a_info)
		{
			const auto* record = a_info->ExceptionRecord;
			if (record->ExceptionCode != kMsvcThreadNameException || record->NumberParameters < 3 ||
				record->ExceptionInformation[0] != 0x1000) {
				return EXCEPTION_CONTINUE_SEARCH;
			}

			const auto* source = reinterpret_cast<const char*>(record->ExceptionInformation[1]);
			char        name[64]{};
			for (std::size_t i = 0; source && i + 1 < std::size(name); ++i) {
				if (!Util::SafeRead(source + i, name[i]) || name[i] == '\0') {
					name[i] = '\0';
					break;
				}
			}

			auto tid = static_cast<DWORD>(record->ExceptionInformation[2]);
			if (tid == static_cast<DWORD>(-1)) {
				tid = GetCurrentThreadId();
			}
			if (name[0] != '\0') {
				ThreadRegistry::Get().SetEngineName(tid, name);
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

	bool Install()
	{
		const auto game = REX::FModule::GetExecutingModule();

		bool ok = true;
		ok &= PatchImport(game, "KERNEL32.dll"sv, "CreateThread"sv, &Hook_CreateThread, g_createThread);
		ok &= PatchImport(game, "api-ms-win-crt-runtime-l1-1-0.dll"sv, "_beginthreadex"sv, &Hook_beginthreadex, g_beginThreadEx);
		ok &= PatchImport(game, "api-ms-win-crt-runtime-l1-1-0.dll"sv, "_beginthread"sv, &Hook_beginthread, g_beginThread);

		if (!AddVectoredExceptionHandler(1, ThreadNameHandler)) {
			REX::WARN("Failed to install thread-name exception handler");
		}

		return ok;
	}

	bool IsStartTrampoline(std::uintptr_t a_address) noexcept
	{
		return a_address == reinterpret_cast<std::uintptr_t>(&ThreadStart) ||
		       a_address == reinterpret_cast<std::uintptr_t>(&ThreadStartEx) ||
		       a_address == reinterpret_cast<std::uintptr_t>(&ThreadStartVoid);
	}
}
