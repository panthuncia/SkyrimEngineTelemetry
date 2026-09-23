#include "PCH.h"

#include "Report/ReportWriter.h"

#include "Threads/ThreadRegistry.h"

namespace EngineTelemetry::ReportWriter
{
	namespace
	{
		constexpr std::uint32_t kSchemaVersion = 1;

		void AppendEscaped(std::string& a_out, std::string_view a_text)
		{
			a_out += '"';
			for (const char c : a_text) {
				switch (c) {
				case '"':
					a_out += "\\\"";
					break;
				case '\\':
					a_out += "\\\\";
					break;
				case '\n':
					a_out += "\\n";
					break;
				case '\r':
					a_out += "\\r";
					break;
				case '\t':
					a_out += "\\t";
					break;
				default:
					if (static_cast<unsigned char>(c) < 0x20) {
						std::format_to(std::back_inserter(a_out), "\\u{:04x}", static_cast<unsigned>(c));
					} else {
						a_out += c;
					}
				}
			}
			a_out += '"';
		}

		std::string Quoted(std::string_view a_text)
		{
			std::string result;
			AppendEscaped(result, a_text);
			return result;
		}

		template <class T>
		std::string OptionalNumber(const std::optional<T>& a_value)
		{
			return a_value ? std::to_string(*a_value) : "null"s;
		}

		std::string_view ToString(ModuleKind a_kind)
		{
			switch (a_kind) {
			case ModuleKind::Game:
				return "game"sv;
			case ModuleKind::Plugin:
				return "plugin"sv;
			case ModuleKind::Other:
				break;
			}
			return "other"sv;
		}

		// Assigns dense indices to frames as they are first referenced.
		class FrameTable
		{
		public:
			explicit FrameTable(Symbolizer& a_symbolizer) :
				_symbolizer(a_symbolizer)
			{}

			std::size_t IndexOf(std::uintptr_t a_address, bool a_isReturnAddress)
			{
				const auto key = std::pair{ a_address, a_isReturnAddress };
				if (const auto it = _indices.find(key); it != _indices.end()) {
					return it->second;
				}
				const auto index = _frames.size();
				_frames.push_back(&_symbolizer.Resolve(a_address, a_isReturnAddress));
				_indices.emplace(key, index);
				return index;
			}

			void AppendJson(std::string& a_out) const
			{
				a_out += "\"frames\":[\n";
				for (std::size_t i = 0; i < _frames.size(); ++i) {
					const auto& frame = *_frames[i];
					std::format_to(
						std::back_inserter(a_out),
						"{{\"module\":{},\"rva\":{},\"function\":{},\"id\":{}",
						frame.module, frame.rva, OptionalNumber(frame.functionRva), OptionalNumber(frame.id));
					if (frame.callKind != CallKind::None) {
						std::format_to(
							std::back_inserter(a_out),
							",\"call\":{{\"kind\":\"{}\",\"rva\":{},\"target\":{},\"disp\":{}}}",
							EngineTelemetry::ToString(frame.callKind), frame.callRva, OptionalNumber(frame.callTargetRva), frame.callDisplacement);
					}
					a_out += i + 1 < _frames.size() ? "},\n" : "}\n";
				}
				a_out += "]";
			}

		private:
			struct PairHash
			{
				std::size_t operator()(const std::pair<std::uintptr_t, bool>& a_key) const noexcept
				{
					return std::hash<std::uintptr_t>{}(a_key.first) ^ (a_key.second ? 0x9E3779B97F4A7C15ull : 0);
				}
			};

			Symbolizer&                                                           _symbolizer;
			std::vector<const FrameInfo*>                                         _frames;
			std::unordered_map<std::pair<std::uintptr_t, bool>, std::size_t, PairHash> _indices;
		};
	}

	bool Write(
		const std::filesystem::path& a_path,
		const SampleSession&         a_session,
		const ModuleMap&             a_modules,
		Symbolizer&                  a_symbolizer,
		std::string&                 a_error)
	{
		auto& registry = ThreadRegistry::Get();
		const auto records = registry.Records();

		std::unordered_map<const ThreadRecord*, std::size_t> threadIndex;
		for (std::size_t i = 0; i < records.size(); ++i) {
			threadIndex.emplace(records[i], i);
		}

		std::unordered_map<const ZoneSite*, std::size_t> zoneIndex;
		std::vector<const ZoneSite*>                     zones;
		for (const auto& [key, count] : a_session.stacks) {
			for (const auto* zone : key.zones) {
				if (zoneIndex.try_emplace(zone, zones.size()).second) {
					zones.push_back(zone);
				}
			}
		}

		std::string out;
		out.reserve(4 * 1024 * 1024);

		const auto  game = REX::FModule::GetExecutingModule();
		const auto  durationSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - a_session.startedSteady).count();
		const auto  startedAt = std::chrono::floor<std::chrono::seconds>(a_session.startedAt);

		out += "{\n";
		std::format_to(std::back_inserter(out), "\"schema\":{},\n", kSchemaVersion);
		std::format_to(std::back_inserter(out), "\"plugin\":{},\n", Quoted(std::format("SkyrimEngineTelemetry {}.{}.{}", SET_PLUGIN_VERSION_MAJOR, SET_PLUGIN_VERSION_MINOR, SET_PLUGIN_VERSION_PATCH)));
		std::format_to(std::back_inserter(out), "\"runtime\":{},\n", Quoted(game.GetFileVersion().string()));
#ifdef SKYRIM_SUPPORT_AE
		out += "\"edition\":\"AE\",\n";
#else
		out += "\"edition\":\"SE\",\n";
#endif
		std::format_to(std::back_inserter(out), "\"imageBase\":{},\n", game.GetBaseAddress());
		std::format_to(std::back_inserter(out), "\"startedAtUtc\":\"{:%Y-%m-%dT%H:%M:%SZ}\",\n", startedAt);
		std::format_to(std::back_inserter(out), "\"durationSec\":{:.3f},\n", durationSec);
		std::format_to(std::back_inserter(out), "\"intervalUs\":{},\n", a_session.intervalUs);
		std::format_to(std::back_inserter(out), "\"rounds\":{},\n", a_session.rounds);
		std::format_to(std::back_inserter(out), "\"samples\":{},\n", a_session.samples);
		std::format_to(std::back_inserter(out), "\"droppedStacks\":{},\n", a_session.droppedStacks);
		std::format_to(
			std::back_inserter(out),
			"\"suspendUs\":{{\"max\":{:.1f},\"mean\":{:.2f}}},\n",
			a_session.maxSuspendUs,
			a_session.samples ? a_session.totalSuspendUs / static_cast<double>(a_session.samples) : 0.0);

		out += "\"modules\":[\n";
		const auto& modules = a_modules.Modules();
		for (std::size_t i = 0; i < modules.size(); ++i) {
			const auto& module = modules[i];
			std::format_to(
				std::back_inserter(out),
				"{{\"name\":{},\"base\":{},\"size\":{},\"kind\":\"{}\",\"loaded\":{}}}{}\n",
				Quoted(module.name), module.base, module.end - module.base, ToString(module.kind), module.loaded, i + 1 < modules.size() ? "," : "");
		}
		out += "],\n";

		FrameTable frames{ a_symbolizer };

		out += "\"threads\":[\n";
		{
			const auto lock = registry.Lock();
			for (std::size_t i = 0; i < records.size(); ++i) {
				const auto& record = *records[i];
				const auto  cycles = record.cyclesInitialized ? record.latestCycles - record.firstCycles : 0;
				const auto  start = record.startAddress ? std::optional{ frames.IndexOf(record.startAddress, false) } : std::nullopt;
				std::format_to(
					std::back_inserter(out),
					"{{\"tid\":{},\"name\":{},\"description\":{},\"engineName\":{},\"rtti\":{},\"origin\":\"{}\",\"createdBy\":{},"
					"\"start\":{},\"alive\":{},\"excluded\":{},\"cycles\":{},"
					"\"samples\":{{\"idle\":{},\"blocked\":{},\"zoned\":{},\"unscoped\":{},\"failed\":{}}}}}{}\n",
					record.tid,
					Quoted(record.DisplayName()),
					Quoted(record.description),
					Quoted(record.engineName),
					Quoted(record.rttiClass),
					ToString(record.origin),
					Quoted(record.createdBy),
					OptionalNumber(start),
					record.alive.load(std::memory_order_relaxed),
					record.excluded,
					cycles,
					record.counts.idle,
					record.counts.blocked,
					record.counts.zoned,
					record.counts.unscoped,
					record.counts.failed,
					i + 1 < records.size() ? "," : "");
			}
		}
		out += "],\n";

		out += "\"zones\":[\n";
		for (std::size_t i = 0; i < zones.size(); ++i) {
			std::format_to(
				std::back_inserter(out),
				"{{\"name\":{},\"category\":{}}}{}\n",
				Quoted(zones[i]->name),
				Quoted(zones[i]->category ? zones[i]->category : ""),
				i + 1 < zones.size() ? "," : "");
		}
		out += "],\n";

		out += "\"stacks\":[\n";
		std::size_t written = 0;
		for (const auto& [key, count] : a_session.stacks) {
			std::format_to(
				std::back_inserter(out),
				"{{\"thread\":{},\"count\":{},\"blocked\":{},\"truncated\":{},\"zoneDepth\":{},\"zones\":[",
				threadIndex.at(key.thread), count, key.blocked, key.truncated, key.zoneDepth);
			for (std::size_t z = 0; z < key.zones.size(); ++z) {
				std::format_to(std::back_inserter(out), "{}{}", z ? "," : "", zoneIndex.at(key.zones[z]));
			}
			out += "],\"frames\":[";
			for (std::size_t f = 0; f < key.frames.size(); ++f) {
				std::format_to(std::back_inserter(out), "{}{}", f ? "," : "", frames.IndexOf(key.frames[f], f != 0));
			}
			out += ++written < a_session.stacks.size() ? "]},\n" : "]}\n";
		}
		out += "],\n";

		frames.AppendJson(out);
		out += "\n}\n";

		std::error_code ec;
		std::filesystem::create_directories(a_path.parent_path(), ec);

		auto temporary = a_path;
		temporary += L".tmp";
		{
			std::ofstream file{ temporary, std::ios::binary | std::ios::trunc };
			if (!file) {
				a_error = std::format("cannot open {}", temporary.string());
				return false;
			}
			file.write(out.data(), static_cast<std::streamsize>(out.size()));
			if (!file) {
				a_error = std::format("write failed for {}", temporary.string());
				return false;
			}
		}

		if (!MoveFileExW(temporary.c_str(), a_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			a_error = std::format("rename to {} failed: {}", a_path.string(), GetLastError());
			return false;
		}
		return true;
	}
}
