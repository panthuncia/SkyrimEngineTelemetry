#include "PCH.h"

#include "Sampler/ModuleMap.h"

#include "Util/Memory.h"

namespace EngineTelemetry
{
	void ModuleMap::Refresh()
	{
		const auto process = GetCurrentProcess();

		std::vector<HMODULE> handles(512);
		DWORD                needed{ 0 };
		for (;;) {
			if (!EnumProcessModules(process, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed)) {
				REX::WARN("EnumProcessModules failed: {}", GetLastError());
				return;
			}
			if (needed <= handles.size() * sizeof(HMODULE)) {
				handles.resize(needed / sizeof(HMODULE));
				break;
			}
			handles.resize(needed / sizeof(HMODULE) + 64);
		}

		for (auto& module : _modules) {
			module.loaded = false;
		}

		const auto& game = Util::GameImage();
		const auto& plugin = Util::PluginImage();

		for (const auto handle : handles) {
			const auto image = Util::ModuleImage(handle);
			if (!image.base) {
				continue;
			}

			const auto known = std::ranges::find_if(_modules, [&](const ModuleInfo& a_module) {
				return a_module.base == image.base && a_module.end == image.end;
			});
			if (known != _modules.end()) {
				known->loaded = true;
				continue;
			}

			ModuleInfo info;
			info.base = image.base;
			info.end = image.end;
			info.kind = image.base == game.base ? ModuleKind::Game : image.base == plugin.base ? ModuleKind::Plugin : ModuleKind::Other;

			const auto  dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
			const auto  nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.base + dos->e_lfanew);
			const auto& exceptions = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
			if (exceptions.VirtualAddress && exceptions.Size) {
				info.functions = reinterpret_cast<const RUNTIME_FUNCTION*>(image.base + exceptions.VirtualAddress);
				info.functionCount = exceptions.Size / sizeof(RUNTIME_FUNCTION);
			}

			wchar_t name[MAX_PATH]{};
			const auto length = GetModuleBaseNameW(process, handle, name, MAX_PATH);
			info.name = Util::WideToUtf8(std::wstring_view{ name, length });

			_modules.push_back(std::move(info));
		}

		_sorted.clear();
		for (std::int32_t i = 0; i < static_cast<std::int32_t>(_modules.size()); ++i) {
			if (_modules[i].loaded) {
				_sorted.push_back(i);
			}
		}
		std::ranges::sort(_sorted, {}, [&](std::int32_t a_index) { return _modules[a_index].base; });
	}

	std::int32_t ModuleMap::IndexOf(std::uintptr_t a_address) const noexcept
	{
		std::size_t low = 0;
		std::size_t high = _sorted.size();
		while (low < high) {
			const auto  mid = (low + high) / 2;
			const auto& module = _modules[_sorted[mid]];
			if (a_address < module.base) {
				high = mid;
			} else if (a_address >= module.end) {
				low = mid + 1;
			} else {
				return _sorted[mid];
			}
		}
		return -1;
	}

	const RUNTIME_FUNCTION* ModuleMap::LookupFunctionEntry(std::uintptr_t a_pc, std::uintptr_t& a_imageBase) const noexcept
	{
		const auto index = IndexOf(a_pc);
		if (index < 0) {
			return nullptr;
		}

		const auto& module = _modules[index];
		const auto  rva = static_cast<std::uint32_t>(a_pc - module.base);

		std::size_t low = 0;
		std::size_t high = module.functionCount;
		while (low < high) {
			const auto  mid = (low + high) / 2;
			const auto& function = module.functions[mid];
			if (rva < function.BeginAddress) {
				high = mid;
			} else if (rva >= function.EndAddress) {
				low = mid + 1;
			} else {
				a_imageBase = module.base;
				return &function;
			}
		}
		return nullptr;
	}

	std::uint32_t ModuleMap::PrimaryFunctionRva(std::uintptr_t a_imageBase, const RUNTIME_FUNCTION* a_function) noexcept
	{
		for (int depth = 0; depth < 16; ++depth) {
			const auto unwindData = a_function->UnwindData;
			if (unwindData & 1) {
				// Some linkers point UnwindData at another RUNTIME_FUNCTION.
				a_function = reinterpret_cast<const RUNTIME_FUNCTION*>(a_imageBase + (unwindData & ~1u));
				continue;
			}

			// UNWIND_INFO: version:3/flags:5, prologue size, code count, frame
			// register/offset, then the (even-padded) unwind codes.
			const auto* info = reinterpret_cast<const std::uint8_t*>(a_imageBase + unwindData);
			std::uint8_t header[4]{};
			if (!Util::SafeRead(info, header, sizeof(header))) {
				break;
			}

			const auto flags = static_cast<std::uint8_t>(header[0] >> 3);
			if ((flags & UNW_FLAG_CHAININFO) == 0) {
				break;
			}

			const auto codeCount = static_cast<std::size_t>(header[2]);
			a_function = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + ((codeCount + 1) & ~std::size_t{ 1 }) * 2);
		}
		return a_function->BeginAddress;
	}
}
