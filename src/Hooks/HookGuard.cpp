#include "PCH.h"

#include "Hooks/HookGuard.h"

#include "Util/Memory.h"

namespace EngineTelemetry::HookGuard
{
	namespace
	{
		// The bytes an entry hook writes: a 14-byte absolute jump is the longest
		// common form (Detours' and CommonLib's are 5).
		constexpr std::size_t kEntryBytes = 14;
		using Entry = std::array<std::uint8_t, kEntryBytes>;

		std::mutex                                 g_mutex;
		std::unordered_map<std::uintptr_t, Entry> g_snapshots;

		std::optional<std::uintptr_t> Read64(std::uintptr_t a_address)
		{
			std::uintptr_t value{ 0 };
			if (!Util::SafeRead(reinterpret_cast<const void*>(a_address), value)) {
				return std::nullopt;
			}
			return value;
		}

		// Where an entry hook at a_address jumps, or 0 if it is not one:
		// jmp rel32, jmp rel8, jmp [rip+disp32], mov rax/r11, imm64; jmp reg,
		// push imm32; ret.
		std::uintptr_t JumpTarget(std::uintptr_t a_address)
		{
			std::array<std::uint8_t, 13> code{};
			if (!Util::SafeRead(reinterpret_cast<const void*>(a_address), code.data(), code.size())) {
				return 0;
			}
			const auto rel32 = [&](std::size_t a_at) {
				std::int32_t value{ 0 };
				std::memcpy(&value, code.data() + a_at, sizeof(value));
				return value;
			};
			if (code[0] == 0xE9) {
				return a_address + 5 + rel32(1);
			}
			if (code[0] == 0xEB) {
				return a_address + 2 + static_cast<std::int8_t>(code[1]);
			}
			if (code[0] == 0xFF && code[1] == 0x25) {
				return Read64(a_address + 6 + rel32(2)).value_or(0);
			}
			std::uint64_t imm{ 0 };
			std::memcpy(&imm, code.data() + 2, sizeof(imm));
			if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
				return imm;
			}
			if (code[0] == 0x49 && code[1] == 0xBB && code[10] == 0x41 && code[11] == 0xFF && code[12] == 0xE3) {
				return imm;
			}
			if (code[0] == 0x68 && code[5] == 0xC3) {
				return static_cast<std::uint32_t>(rel32(1));
			}
			return 0;
		}

		// Follows a short chain of jumps (an entry branch into a stub, the stub's
		// absolute jump to the hook) and names where it ends.
		std::string DescribeHook(std::uintptr_t a_address)
		{
			for (int hop = 0; hop < 4; ++hop) {
				const auto next = JumpTarget(a_address);
				if (!next) {
					break;
				}
				a_address = next;
			}
			return Util::DescribeAddress(a_address);
		}

		std::optional<Entry> ReadEntry(std::uintptr_t a_target)
		{
			Entry entry{};
			if (!Util::SafeRead(reinterpret_cast<const void*>(a_target), entry.data(), entry.size())) {
				return std::nullopt;
			}
			return entry;
		}
	}

	void Snapshot(std::uintptr_t a_target)
	{
		if (const auto entry = ReadEntry(a_target)) {
			const std::scoped_lock lock{ g_mutex };
			g_snapshots.try_emplace(a_target, *entry);
		}
	}

	std::string TakenBy(std::uintptr_t a_target)
	{
		const auto entry = ReadEntry(a_target);
		if (!entry) {
			return "its entry is not readable";
		}

		// Hooked before the snapshot (by a plugin that loaded first), or never
		// snapshotted.
		if (const auto target = JumpTarget(a_target); target && !Util::GameImage().Contains(target)) {
			return std::format("its entry already jumps to {}", DescribeHook(a_target));
		}
		if ((*entry)[0] == 0xCC) {
			return "its entry is a breakpoint (int3)";
		}

		// Patched since the snapshot: a hook within the game image (a branch
		// into SKSE's trampoline, an inline patch) still counts.
		const std::scoped_lock lock{ g_mutex };
		if (const auto it = g_snapshots.find(a_target); it != g_snapshots.end() && it->second != *entry) {
			const auto target = JumpTarget(a_target);
			return target ? std::format("its entry was hooked after load and jumps to {}", DescribeHook(a_target)) :
			                "its entry was patched after load";
		}
		return {};
	}
}
