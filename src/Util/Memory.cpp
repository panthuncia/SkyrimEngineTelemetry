#include "PCH.h"

#include "Util/Memory.h"

#include "Settings.h"

namespace EngineTelemetry::Util
{
	bool SafeRead(const void* a_src, void* a_dst, std::size_t a_size) noexcept
	{
		__try {
			std::memcpy(a_dst, a_src, a_size);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	ImageRange ModuleImage(HMODULE a_module) noexcept
	{
		if (!a_module) {
			return {};
		}

		const auto base = reinterpret_cast<std::uintptr_t>(a_module);
		const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		return { base, base + nt->OptionalHeader.SizeOfImage };
	}

	const ImageRange& GameImage() noexcept
	{
		static const auto range = ModuleImage(GetModuleHandleW(nullptr));
		return range;
	}

	const ImageRange& PluginImage() noexcept
	{
		static const auto range = ModuleImage(Settings::PluginModule());
		return range;
	}

	namespace
	{
		// ".?AVFoo@Bar@@" -> "Bar::Foo". Templated names are returned undecorated
		// only as far as the prefix and suffix.
		std::string DemangleTypeName(std::string_view a_name)
		{
			for (const auto prefix : { ".?AV"sv, ".?AU"sv }) {
				if (a_name.starts_with(prefix)) {
					a_name.remove_prefix(prefix.size());
					break;
				}
			}
			if (a_name.ends_with("@@"sv)) {
				a_name.remove_suffix(2);
			}
			if (a_name.find('?') != std::string_view::npos) {
				return std::string{ a_name };
			}

			std::vector<std::string_view> parts;
			for (std::size_t start = 0; start <= a_name.size();) {
				const auto at = a_name.find('@', start);
				const auto end = at == std::string_view::npos ? a_name.size() : at;
				parts.push_back(a_name.substr(start, end - start));
				start = end + 1;
			}

			std::string result;
			for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
				if (!result.empty()) {
					result += "::";
				}
				result += *it;
			}
			return result;
		}
	}

	std::string GameRttiClassName(const void* a_object)
	{
		const auto& game = GameImage();

		const void* vtable{ nullptr };
		if (!a_object || !SafeRead(a_object, vtable) || !game.Contains(vtable)) {
			return {};
		}

		const void* locatorAddress{ nullptr };
		if (!SafeRead(static_cast<const void* const*>(vtable) - 1, locatorAddress) || !game.Contains(locatorAddress)) {
			return {};
		}

		// x64 CompleteObjectLocator: signature, offset, cdOffset, typeDescriptor
		// RVA, classDescriptor RVA, self RVA.
		struct Locator
		{
			std::uint32_t signature;
			std::uint32_t offset;
			std::uint32_t ctorDispOffset;
			std::uint32_t typeDescriptor;
			std::uint32_t classDescriptor;
			std::uint32_t self;
		} locator{};
		if (!SafeRead(locatorAddress, locator) || locator.signature != 1 ||
			locator.self != reinterpret_cast<std::uintptr_t>(locatorAddress) - game.base) {
			return {};
		}

		// TypeDescriptor: vftable pointer, spare pointer, then the mangled name.
		const auto nameAddress = game.base + locator.typeDescriptor + 0x10;
		char       name[256]{};
		for (std::size_t i = 0; i + 1 < std::size(name); ++i) {
			if (!SafeRead(reinterpret_cast<const void*>(nameAddress + i), name[i])) {
				return {};
			}
			if (name[i] == '\0') {
				break;
			}
		}
		return DemangleTypeName(name);
	}

	std::string DescribeAddress(std::uintptr_t a_address)
	{
		HMODULE module{ nullptr };
		if (!GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(a_address),
				&module) ||
			!module) {
			return std::format("0x{:X}", a_address);
		}

		wchar_t path[MAX_PATH]{};
		const auto length = GetModuleFileNameW(module, path, MAX_PATH);
		const auto fileName = std::filesystem::path{ std::wstring_view{ path, length } }.filename().wstring();
		return std::format("{}+0x{:X}", WideToUtf8(fileName), a_address - reinterpret_cast<std::uintptr_t>(module));
	}

	std::string WideToUtf8(std::wstring_view a_text)
	{
		if (a_text.empty()) {
			return {};
		}
		const auto size = WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
		std::string result(static_cast<std::size_t>(size), '\0');
		WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), result.data(), size, nullptr, nullptr);
		return result;
	}

	std::wstring Utf8ToWide(std::string_view a_text)
	{
		if (a_text.empty()) {
			return {};
		}
		const auto size = MultiByteToWideChar(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
		std::wstring result(static_cast<std::size_t>(size), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), result.data(), size);
		return result;
	}
}
