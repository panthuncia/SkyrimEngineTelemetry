#include "PCH.h"

#include "Sampler/Symbolizer.h"

#include "Util/Memory.h"

namespace EngineTelemetry
{
	namespace
	{
		bool PrimaryFunctionRvaGuarded(std::uintptr_t a_imageBase, const RUNTIME_FUNCTION* a_function, std::uint32_t& a_rva) noexcept
		{
			__try {
				a_rva = ModuleMap::PrimaryFunctionRva(a_imageBase, a_function);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Decodes the call instruction that ends immediately before a return
		// address. Longest encodings are checked first so that bytes inside a
		// longer instruction are not mistaken for a shorter one.
		void DecodeCall(std::uintptr_t a_returnAddress, FrameInfo& a_frame, std::uintptr_t a_moduleBase, std::uintptr_t a_moduleEnd)
		{
			std::uint8_t bytes[7]{};
			if (!Util::SafeRead(reinterpret_cast<const void*>(a_returnAddress - sizeof(bytes)), bytes, sizeof(bytes))) {
				return;
			}
			const auto at = [&](int a_offsetFromEnd) { return bytes[sizeof(bytes) - a_offsetFromEnd]; };
			const auto disp32 = [&](int a_offsetFromEnd) {
				std::int32_t value{};
				std::memcpy(&value, &bytes[sizeof(bytes) - a_offsetFromEnd], sizeof(value));
				return value;
			};
			const auto isCallModRm = [](std::uint8_t a_modrm, std::uint8_t a_mod) {
				return (a_modrm >> 6) == a_mod && ((a_modrm >> 3) & 7) == 2;
			};

			const auto rva = a_returnAddress - a_moduleBase;

			if (at(5) == 0xE8) {
				a_frame.callKind = CallKind::Direct;
				a_frame.callRva = rva - 5;
				const auto target = a_returnAddress + static_cast<std::intptr_t>(disp32(4));
				if (target >= a_moduleBase && target < a_moduleEnd) {
					a_frame.callTargetRva = target - a_moduleBase;
				}
			} else if (at(6) == 0xFF && at(5) == 0x15) {
				a_frame.callKind = CallKind::RipIndirect;
				a_frame.callRva = rva - 6;
				a_frame.callDisplacement = disp32(4);
			} else if (at(6) == 0xFF && isCallModRm(at(5), 2) && (at(5) & 7) != 4) {
				a_frame.callKind = CallKind::MemIndirect;  // [reg+disp32]
				a_frame.callRva = rva - 6 - ((at(7) & 0xF0) == 0x40 ? 1 : 0);
				a_frame.callDisplacement = disp32(4);
			} else if (at(3) == 0xFF && isCallModRm(at(2), 1) && (at(2) & 7) != 4) {
				a_frame.callKind = CallKind::MemIndirect;  // [reg+disp8]
				a_frame.callRva = rva - 3 - ((at(4) & 0xF0) == 0x40 ? 1 : 0);
				a_frame.callDisplacement = static_cast<std::int8_t>(at(1));
			} else if (at(4) == 0xFF && isCallModRm(at(3), 1) && (at(3) & 7) == 4) {
				a_frame.callKind = CallKind::MemIndirect;  // [reg+SIB+disp8]
				a_frame.callRva = rva - 4 - ((at(5) & 0xF0) == 0x40 ? 1 : 0);
				a_frame.callDisplacement = static_cast<std::int8_t>(at(1));
			} else if (at(2) == 0xFF && isCallModRm(at(1), 3)) {
				a_frame.callKind = CallKind::Register;
				a_frame.callRva = rva - 2 - ((at(3) & 0xF0) == 0x40 ? 1 : 0);
			} else if (at(2) == 0xFF && isCallModRm(at(1), 0) && (at(1) & 7) != 4 && (at(1) & 7) != 5) {
				a_frame.callKind = CallKind::MemIndirect;  // [reg]
				a_frame.callRva = rva - 2 - ((at(3) & 0xF0) == 0x40 ? 1 : 0);
			}
		}
	}

	std::string_view ToString(CallKind a_kind) noexcept
	{
		switch (a_kind) {
		case CallKind::Direct:
			return "direct"sv;
		case CallKind::RipIndirect:
			return "rip-indirect"sv;
		case CallKind::MemIndirect:
			return "mem-indirect"sv;
		case CallKind::Register:
			return "register"sv;
		case CallKind::None:
			break;
		}
		return "none"sv;
	}

	const REL::Offset2ID& AddressLibraryReverseIndex()
	{
		static const REL::Offset2ID& index = [] -> const REL::Offset2ID& {
			const auto start = std::chrono::steady_clock::now();

			auto* offset2id = REL::Offset2ID::GetSingleton();
			if (offset2id->size() == 0) {
				offset2id->load_v2();
			}
			if (offset2id->size() == 0) {
				offset2id->load_v5();
			}

			REX::INFO(
				"Loaded {} Address Library IDs for reverse lookup in {} ms",
				offset2id->size(),
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
			return *offset2id;
		}();
		return index;
	}

	std::optional<std::uint64_t> AddressLibraryIdForOffset(std::uint64_t a_offset)
	{
		const auto& index = AddressLibraryReverseIndex();
		const auto  it = std::lower_bound(
            index.begin(), index.end(), a_offset,
            [](const REL::IDDB::MAPPING& a_mapping, std::uint64_t a_value) { return a_mapping.offset < a_value; });
		if (it == index.end() || it->offset != a_offset) {
			return std::nullopt;
		}
		return it->id;
	}

	void Symbolizer::LoadAddressLibrary()
	{
		_offsetToId = &AddressLibraryReverseIndex();
	}

	std::optional<std::uint64_t> Symbolizer::IdForGameOffset(std::uint64_t a_offset) const
	{
		if (!_offsetToId) {
			return std::nullopt;
		}
		return AddressLibraryIdForOffset(a_offset);
	}

	const FrameInfo& Symbolizer::Resolve(std::uintptr_t a_address, bool a_isReturnAddress)
	{
		auto& cache = _cache[a_isReturnAddress ? 1 : 0];
		if (const auto it = cache.find(a_address); it != cache.end()) {
			return it->second;
		}
		return cache.emplace(a_address, Compute(a_address, a_isReturnAddress)).first->second;
	}

	FrameInfo Symbolizer::Compute(std::uintptr_t a_address, bool a_isReturnAddress) const
	{
		FrameInfo frame;
		frame.module = _modules.IndexOf(a_address);
		if (frame.module < 0) {
			frame.rva = a_address;
			return frame;
		}

		const auto& module = _modules.Modules()[frame.module];
		frame.rva = a_address - module.base;

		// A return address can sit one past the end of its function when the
		// call was the last instruction, so look up the byte before it.
		std::uintptr_t imageBase{ 0 };
		const auto     lookupAddress = a_isReturnAddress ? a_address - 1 : a_address;
		if (const auto* function = _modules.LookupFunctionEntry(lookupAddress, imageBase)) {
			std::uint32_t functionRva{ 0 };
			if (PrimaryFunctionRvaGuarded(imageBase, function, functionRva)) {
				frame.functionRva = functionRva;
				if (module.kind == ModuleKind::Game) {
					frame.id = IdForGameOffset(functionRva);
				}
			}
		}

		if (a_isReturnAddress) {
			DecodeCall(a_address, frame, module.base, module.end);
		}
		return frame;
	}
}
