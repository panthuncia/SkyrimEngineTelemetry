#pragma once

#include "Sampler/ModuleMap.h"

namespace EngineTelemetry
{
	enum class CallKind : std::uint8_t
	{
		None,         // Leaf frame, or bytes before the return address are not a recognized call.
		Direct,       // E8 rel32
		RipIndirect,  // FF 15 [rip+disp32] (imports, function pointers in .data)
		MemIndirect,  // FF /2 [reg+disp] (virtual calls: disp / 8 is the vtable slot)
		Register,     // FF /2 reg
	};

	[[nodiscard]] std::string_view ToString(CallKind a_kind) noexcept;

	struct FrameInfo
	{
		std::int32_t                 module{ -1 };
		std::uint64_t                rva{};
		std::optional<std::uint32_t> functionRva;
		std::optional<std::uint64_t> id;  // Address Library ID of functionRva (game module only).

		// For return addresses: the call instruction that produced this frame.
		CallKind                     callKind{ CallKind::None };
		std::uint64_t                callRva{};
		std::optional<std::uint64_t> callTargetRva;  // Direct calls within the same module.
		std::int32_t                 callDisplacement{};
	};

	// commonlib's offset -> ID index (sorted by offset), built on first use.
	// Costs ~16 MB, so only the sampler and the zones file request it.
	[[nodiscard]] const REL::Offset2ID& AddressLibraryReverseIndex();

	// Exact reverse lookup; Offset2ID::get_id returns the nearest entry instead.
	[[nodiscard]] std::optional<std::uint64_t> AddressLibraryIdForOffset(std::uint64_t a_offset);

	// Turns raw addresses into module/function/Address Library coordinates.
	// Owned by the sampler thread; only used outside the suspended window.
	class Symbolizer
	{
	public:
		explicit Symbolizer(const ModuleMap& a_modules) :
			_modules(a_modules)
		{}

		void LoadAddressLibrary();

		[[nodiscard]] const FrameInfo& Resolve(std::uintptr_t a_address, bool a_isReturnAddress);

		[[nodiscard]] std::optional<std::uint64_t> IdForGameOffset(std::uint64_t a_offset) const;

	private:
		[[nodiscard]] FrameInfo Compute(std::uintptr_t a_address, bool a_isReturnAddress) const;

		const ModuleMap&                                     _modules;
		const REL::Offset2ID*                                _offsetToId{ nullptr };  // Sorted by offset.
		std::unordered_map<std::uintptr_t, FrameInfo>        _cache[2];
	};
}
