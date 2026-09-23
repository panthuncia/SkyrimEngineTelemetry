#pragma once

namespace EngineTelemetry::Util
{
	// Copies a_size bytes, returning false instead of faulting on bad memory.
	[[nodiscard]] bool SafeRead(const void* a_src, void* a_dst, std::size_t a_size) noexcept;

	template <class T>
	[[nodiscard]] bool SafeRead(const void* a_src, T& a_dst) noexcept
	{
		static_assert(std::is_trivially_copyable_v<T>);
		return SafeRead(a_src, std::addressof(a_dst), sizeof(T));
	}

	struct ImageRange
	{
		std::uintptr_t base{};
		std::uintptr_t end{};

		[[nodiscard]] bool Contains(std::uintptr_t a_address) const noexcept { return a_address >= base && a_address < end; }
		[[nodiscard]] bool Contains(const void* a_address) const noexcept { return Contains(reinterpret_cast<std::uintptr_t>(a_address)); }
	};

	[[nodiscard]] ImageRange ModuleImage(HMODULE a_module) noexcept;
	[[nodiscard]] const ImageRange& GameImage() noexcept;
	[[nodiscard]] const ImageRange& PluginImage() noexcept;

	// Returns the demangled RTTI class name of a polymorphic object whose vtable
	// lives in the game executable, or an empty string.
	[[nodiscard]] std::string GameRttiClassName(const void* a_object);

	// "SkyrimSE.exe+0x1A2B3C" style description, or a bare hex address.
	[[nodiscard]] std::string DescribeAddress(std::uintptr_t a_address);

	[[nodiscard]] std::string WideToUtf8(std::wstring_view a_text);
	[[nodiscard]] std::wstring Utf8ToWide(std::string_view a_text);
}
