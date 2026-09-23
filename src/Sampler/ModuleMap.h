#pragma once

namespace EngineTelemetry
{
	enum class ModuleKind : std::uint8_t
	{
		Game,
		Plugin,  // This DLL.
		Other,
	};

	struct ModuleInfo
	{
		std::uintptr_t          base{};
		std::uintptr_t          end{};
		const RUNTIME_FUNCTION* functions{ nullptr };
		std::uint32_t           functionCount{};
		std::string             name;
		ModuleKind              kind{ ModuleKind::Other };
		bool                    loaded{ true };
	};

	// Loaded-module table used by the sampler. Owned by the sampler thread.
	// Unlike RtlLookupFunctionEntry, lookups take no loader locks, so they are
	// safe while another thread is suspended.
	class ModuleMap
	{
	public:
		// Re-enumerates loaded modules. Indices into Modules() stay stable;
		// unloaded modules are kept and flagged.
		void Refresh();

		[[nodiscard]] const std::vector<ModuleInfo>& Modules() const noexcept { return _modules; }

		// Index into Modules() of the loaded module containing a_address, or -1.
		[[nodiscard]] std::int32_t IndexOf(std::uintptr_t a_address) const noexcept;

		// Allocation- and lock-free. Returns the RUNTIME_FUNCTION covering a_pc.
		[[nodiscard]] const RUNTIME_FUNCTION* LookupFunctionEntry(std::uintptr_t a_pc, std::uintptr_t& a_imageBase) const noexcept;

		// Follows chained unwind info back to the function's primary entry, so
		// split-off chunks resolve to the function that owns them.
		[[nodiscard]] static std::uint32_t PrimaryFunctionRva(std::uintptr_t a_imageBase, const RUNTIME_FUNCTION* a_function) noexcept;

	private:
		std::vector<ModuleInfo>   _modules;
		std::vector<std::int32_t> _sorted;  // Loaded module indices ordered by base.
	};
}
