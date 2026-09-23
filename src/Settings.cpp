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

		std::wstring ReadString(const std::wstring& a_path, const wchar_t* a_section, const wchar_t* a_key)
		{
			std::wstring value(32'768, L'\0');
			const auto length = GetPrivateProfileStringW(a_section, a_key, L"", value.data(), static_cast<DWORD>(value.size()), a_path.c_str());
			value.resize(length);
			return value;
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
		g_values.basicTelemetryEnabled = ReadBool(file, L"BasicTelemetry", L"bCapture", g_values.basicTelemetryEnabled);
		const auto mode = ReadString(file, L"BasicTelemetry", L"sMode");
		g_values.basicTelemetryTrace = _wcsicmp(mode.c_str(), L"Trace") == 0;
		g_values.basicTelemetryOutputDirectory = ReadString(file, L"BasicTelemetry", L"sOutputDirectory");
		g_values.basicTelemetrySnapshotIntervalSec = ReadUInt(file, L"BasicTelemetry", L"iSnapshotIntervalSec", g_values.basicTelemetrySnapshotIntervalSec, 1, 3'600);
		g_values.basicTelemetryMaximumTraceEvents = ReadUInt(file, L"BasicTelemetry", L"iMaximumTraceEvents", g_values.basicTelemetryMaximumTraceEvents, 1'000, 10'000'000);
		g_values.basicTelemetryWriteSqlite = ReadBool(file, L"BasicTelemetry", L"bWriteSqlite", g_values.basicTelemetryWriteSqlite);
		g_values.basicTelemetryWriteMarkdown = ReadBool(file, L"BasicTelemetry", L"bWriteMarkdown", g_values.basicTelemetryWriteMarkdown);
		g_values.basicTelemetryMeasureThreadCpuTime = ReadBool(file, L"BasicTelemetry", L"bMeasureThreadCpuTime", g_values.basicTelemetryMeasureThreadCpuTime);

		REX::INFO(
			"Settings: nameThreads={} logThreads={} sampler={} intervalUs={} reportIntervalSec={} basicTelemetry={} mode={} snapshotIntervalSec={}",
			g_values.nameThreads, g_values.logThreads, g_values.samplerEnabled, g_values.samplerIntervalUs, g_values.reportIntervalSec,
			g_values.basicTelemetryEnabled, g_values.basicTelemetryTrace ? "Trace" : "Summary", g_values.basicTelemetrySnapshotIntervalSec);
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
