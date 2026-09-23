#pragma once

namespace EngineTelemetry
{
	// Batches detour attachments into a single Detours transaction so every
	// hook in a group is installed atomically with respect to the game threads.
	class DetourTransaction
	{
	public:
		DetourTransaction();
		~DetourTransaction();
		DetourTransaction(const DetourTransaction&) = delete;
		DetourTransaction& operator=(const DetourTransaction&) = delete;

		void Attach(void** a_target, void* a_detour, std::string_view a_name);

		// Returns false (and aborts every attachment) if any step failed.
		bool Commit();

	private:
		std::vector<std::string_view> _names;
		bool                          _open{ false };
		bool                          _failed{ false };
	};

	// Resolves Hook::id through Address Library, points Hook::original at it and
	// queues Hook::thunk as the detour. Hook must provide:
	//   static constexpr std::string_view name;
	//   static constexpr REL::ID id;
	//   static <ret> thunk(<args>);
	//   static inline decltype(&thunk) original;
	template <class Hook>
	void AttachFunctionDetour(DetourTransaction& a_transaction)
	{
		REL::Relocation<std::uintptr_t> target{ Hook::id };
		Hook::original = reinterpret_cast<decltype(Hook::original)>(target.address());
		a_transaction.Attach(reinterpret_cast<void**>(&Hook::original), reinterpret_cast<void*>(&Hook::thunk), Hook::name);
	}
}
