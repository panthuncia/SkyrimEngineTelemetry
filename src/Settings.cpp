#include "PCH.h"

#include "Settings.h"

namespace EngineTelemetry::Settings
{
	namespace
	{
		Values g_values;

		bool ReadBool(const std::wstring& a_path, const wchar_t* a_section, const wchar_t* a_key, bool a_default)
		{
			return GetPrivateProfileIntW(a_section, a_key, a_default ? 1 : 0, a_path.c_str()) != 0;
		}

		std::uint32_t ReadUInt(const std::wstring& a_path, const wchar_t* a_section, const wchar_t* a_key, std::uint32_t a_default, std::uint32_t a_min, std::uint32_t a_max)
		{
			const auto value = GetPrivateProfileIntW(a_section, a_key, static_cast<INT>(a_default), a_path.c_str());
			return std::clamp(static_cast<std::uint32_t>(value), a_min, a_max);
		}
	}

	void Load()
	{
		const auto path = PluginSidecarPath(L".ini");
		if (path.empty()) {
			REX::WARN("Could not resolve plugin path; using default settings");
			return;
		}

		const auto file = path.wstring();
		g_values.nameThreads = ReadBool(file, L"Threads", L"bNameThreads", g_values.nameThreads);
		g_values.logThreads = ReadBool(file, L"Threads", L"bLogThreads", g_values.logThreads);
		g_values.samplerEnabled = ReadBool(file, L"Sampler", L"bEnable", g_values.samplerEnabled);
		g_values.samplerIntervalUs = ReadUInt(file, L"Sampler", L"iIntervalUs", g_values.samplerIntervalUs, 100, 1'000'000);
		g_values.reportIntervalSec = ReadUInt(file, L"Sampler", L"iReportIntervalSec", g_values.reportIntervalSec, 1, 3'600);

		REX::INFO(
			"Settings: nameThreads={} logThreads={} sampler={} intervalUs={} reportIntervalSec={}",
			g_values.nameThreads, g_values.logThreads, g_values.samplerEnabled, g_values.samplerIntervalUs, g_values.reportIntervalSec);
	}

	const Values& Get() noexcept
	{
		return g_values;
	}

	HMODULE PluginModule() noexcept
	{
		static const HMODULE module = [] {
			HMODULE handle{ nullptr };
			GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&PluginModule),
				&handle);
			return handle;
		}();
		return module;
	}

	std::filesystem::path PluginSidecarPath(std::wstring_view a_extension)
	{
		std::wstring buffer(MAX_PATH, L'\0');
		for (;;) {
			const auto length = GetModuleFileNameW(PluginModule(), buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length == 0) {
				return {};
			}
			if (length < buffer.size()) {
				buffer.resize(length);
				break;
			}
			buffer.resize(buffer.size() * 2);
		}

		std::filesystem::path path{ buffer };
		path.replace_extension(a_extension);
		return path;
	}
}
