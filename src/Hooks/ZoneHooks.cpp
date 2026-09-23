#include "PCH.h"

#include "Hooks/ZoneHooks.h"

#include "Hooks/Detours.h"
#include "Sampler/ModuleMap.h"
#include "Sampler/Symbolizer.h"
#include "Settings.h"
#include "Telemetry/Zone.h"
#include "Util/Memory.h"

namespace EngineTelemetry
{
	struct ZoneHookData;

	// Handed to the assembly stub in r11. `original` must stay at offset 0.
	struct ZoneHookContext
	{
		void*         original;
		ZoneHookData* data;
	};
	static_assert(offsetof(ZoneHookContext, original) == 0);

	struct ZoneHookData
	{
		ZoneHookData(std::string_view a_name, std::string_view a_category, std::uintptr_t a_target) :
			name(a_name),
			category(a_category),
			site{ name.c_str(), category.c_str() },
			tracyLocation{ name.c_str(), name.c_str(), "SkyrimSE.exe", 0, 0 },
			callsite{ name, category },
			target(a_target)
		{}

		// Pointed into by the members below; the object is never moved or freed.
		std::string               name;
		std::string               category;
		ZoneSite                  site;
		tracy::SourceLocationData tracyLocation;
		basic_telemetry::Callsite callsite;
		std::uintptr_t            target;
		ZoneHookContext           context{};
	};
}

extern "C"
{
	void SET_ZoneHookEntry();
	void SET_ZoneHookEnter(EngineTelemetry::ZoneHookContext* a_context) noexcept;
	void SET_ZoneHookExit(EngineTelemetry::ZoneHookContext* a_context) noexcept;
	EXCEPTION_DISPOSITION SET_ZoneHookUnwind(EXCEPTION_RECORD* a_record, void* a_establisherFrame, CONTEXT* a_context, void* a_dispatcherContext);
}

namespace EngineTelemetry::ZoneHooks
{
	namespace
	{
		std::mutex                 g_installedMutex;
		std::vector<ZoneHookData*> g_installed;

		// Must match the frame layout in ZoneHookStub.asm.
		constexpr std::size_t kContextOffset = 0xC0;
		constexpr std::size_t kStateOffset = 0xC8;

		// EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND
		constexpr DWORD kUnwindFlags = 0x2 | 0x4;

		constexpr std::uint32_t kMaxActive = 128;

		struct ActiveZone
		{
			ZoneStack* zoneStack;
			alignas(basic_telemetry::Scope) std::byte scope[sizeof(basic_telemetry::Scope)];
			alignas(tracy::ScopedZone) std::byte tracyZone[sizeof(tracy::ScopedZone)];
		};

		// Per-thread LIFO of open hook zones. Zones past kMaxActive are counted
		// in `overflow` and not recorded; they are always the innermost ones, so
		// exits consume the overflow first.
		struct ActiveZones
		{
			std::uint32_t depth;
			std::uint32_t overflow;
			ActiveZone    entries[kMaxActive];
		};

		thread_local ActiveZones* t_active{ nullptr };
		thread_local bool         t_activeFailed{ false };

		ActiveZones* CurrentActiveZones() noexcept
		{
			if (t_active || t_activeFailed) {
				return t_active;
			}
			// Straight from the process heap: never engine code, which may itself
			// be hooked.
			t_active = static_cast<ActiveZones*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ActiveZones)));
			t_activeFailed = t_active == nullptr;
			return t_active;
		}

		// Runtime thunks: `mov r11, context; jmp [rip]; dq SET_ZoneHookEntry`.
		// Pages stay executable throughout, since earlier thunks on the same page
		// may be running while a new one is written.
		void* AllocateThunk(ZoneHookContext* a_context)
		{
			constexpr std::size_t kThunkSize = 24;
			constexpr std::size_t kPageSize = 4096;

			static std::mutex lock;
			static std::byte* page{ nullptr };
			static std::size_t used{ kPageSize };

			const std::scoped_lock guard{ lock };
			if (used + kThunkSize > kPageSize) {
				page = static_cast<std::byte*>(VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ));
				if (!page) {
					return nullptr;
				}
				used = 0;
			}

			auto* thunk = page + used;
			const auto contextValue = reinterpret_cast<std::uint64_t>(a_context);
			const auto entryValue = reinterpret_cast<std::uint64_t>(&SET_ZoneHookEntry);

			std::uint8_t code[kThunkSize]{ 0x49, 0xBB };  // mov r11, imm64
			std::memcpy(code + 2, &contextValue, 8);
			code[10] = 0xFF;  // jmp qword ptr [rip+0]
			code[11] = 0x25;
			std::memcpy(code + 16, &entryValue, 8);

			DWORD oldProtect{ 0 };
			if (!VirtualProtect(page, kPageSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
				return nullptr;
			}
			std::memcpy(thunk, code, kThunkSize);
			VirtualProtect(page, kPageSize, PAGE_EXECUTE_READ, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), thunk, kThunkSize);

			used += kThunkSize;
			return thunk;
		}

		// True if a_address is the primary entry of a function with unwind data
		// in the game executable. Rejects data, mid-function addresses, and
		// split-off function chunks.
		bool IsGameFunctionEntry(std::uintptr_t a_address)
		{
			const auto& game = Util::GameImage();
			if (!game.Contains(a_address)) {
				return false;
			}

			DWORD64    imageBase{ 0 };
			const auto function = RtlLookupFunctionEntry(a_address, &imageBase, nullptr);
			if (!function || imageBase != game.base) {
				return false;
			}
			const auto rva = static_cast<std::uint32_t>(a_address - game.base);
			return function->BeginAddress == rva && ModuleMap::PrimaryFunctionRva(game.base, function) == rva;
		}

		struct ZoneLine
		{
			std::size_t   line;
			std::uint64_t id;
			std::string   category;
			std::string   name;
		};

		std::optional<std::uint64_t> ParseId(std::string_view a_text)
		{
			// "<seId>,<aeId>" selects the id for the runtime this build targets.
			if (const auto comma = a_text.find(','); comma != std::string_view::npos) {
#ifdef SKYRIM_SUPPORT_AE
				a_text = a_text.substr(comma + 1);
#else
				a_text = a_text.substr(0, comma);
#endif
			}

			std::uint64_t value{ 0 };
			const auto [end, error] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value);
			if (error != std::errc{} || end != a_text.data() + a_text.size()) {
				return std::nullopt;
			}
			return value;
		}

		std::vector<ZoneLine> ReadZoneFile(const std::filesystem::path& a_path)
		{
			std::vector<ZoneLine> zones;

			std::ifstream file{ a_path };
			std::string   text;
			for (std::size_t lineNumber = 1; std::getline(file, text); ++lineNumber) {
				std::string_view line{ text };
				if (const auto comment = line.find_first_of("#;"); comment != std::string_view::npos) {
					line = line.substr(0, comment);
				}

				const auto next = [&]() {
					const auto start = line.find_first_not_of(" \t\r");
					if (start == std::string_view::npos) {
						line = {};
						return std::string_view{};
					}
					line.remove_prefix(start);
					const auto end = std::min(line.find_first_of(" \t\r"), line.size());
					const auto token = line.substr(0, end);
					line.remove_prefix(end);
					return token;
				};

				const auto idText = next();
				if (idText.empty()) {
					continue;
				}
				const auto category = next();
				const auto nameStart = line.find_first_not_of(" \t");
				const auto nameEnd = line.find_last_not_of(" \t\r");
				const auto id = ParseId(idText);
				if (!id || category.empty() || nameStart == std::string_view::npos) {
					REX::WARN("{}:{}: expected '<id> <category> <name>'", a_path.filename().string(), lineNumber);
					continue;
				}
				zones.push_back({ lineNumber, *id, std::string{ category }, std::string{ line.substr(nameStart, nameEnd - nameStart + 1) } });
			}
			return zones;
		}
	}

	bool Install(std::uintptr_t a_target, std::string_view a_name, std::string_view a_category)
	{
		if (!IsGameFunctionEntry(a_target)) {
			REX::WARN("Zone hook '{}': {} is not a function entry in the game executable", a_name, Util::DescribeAddress(a_target));
			return false;
		}

		// Leaked on purpose: the stub and any in-flight calls reference it.
		auto* data = new ZoneHookData{ a_name, a_category, a_target };
		data->context.original = reinterpret_cast<void*>(a_target);
		data->context.data = data;

		auto* thunk = AllocateThunk(&data->context);
		if (!thunk) {
			REX::ERROR("Zone hook '{}': failed to allocate thunk", a_name);
			return false;
		}

		DetourTransaction transaction;
		transaction.Attach(&data->context.original, thunk, data->name);
		if (!transaction.Commit()) {
			return false;
		}

		{
			const std::scoped_lock lock{ g_installedMutex };
			g_installed.push_back(data);
		}
		return true;
	}

	const ZoneSite* FindInstalledSite(std::uintptr_t a_target) noexcept
	{
		const std::scoped_lock lock{ g_installedMutex };
		const auto it = std::ranges::find(g_installed, a_target, &ZoneHookData::target);
		return it == g_installed.end() ? nullptr : &(*it)->site;
	}

	void InstallFromFile()
	{
		const auto path = Settings::PluginSidecarPath(L".zones");
		std::error_code ec;
		if (path.empty() || !std::filesystem::exists(path, ec)) {
			return;
		}

		const auto zones = ReadZoneFile(path);
		if (zones.empty()) {
			return;
		}

		// Resolve IDs exactly: IDDB::offset() returns a neighbouring entry for an
		// unknown ID. One pass over the reverse index covers every line.
		std::unordered_map<std::uint64_t, std::uint64_t> offsets;
		for (const auto& zone : zones) {
			offsets.emplace(zone.id, 0);
		}
		for (const auto& mapping : AddressLibraryReverseIndex()) {
			if (const auto it = offsets.find(mapping.id); it != offsets.end()) {
				it->second = mapping.offset;
			}
		}

		const auto          base = Util::GameImage().base;
		std::size_t         installed = 0;
		std::vector<std::uintptr_t> targets;
		for (const auto& zone : zones) {
			const auto offset = offsets.at(zone.id);
			if (offset == 0) {
				REX::WARN("{}:{}: unknown Address Library ID {}", path.filename().string(), zone.line, zone.id);
				continue;
			}

			const auto target = base + offset;
			if (std::ranges::find(targets, target) != targets.end()) {
				REX::WARN("{}:{}: ID {} is already zoned; skipping", path.filename().string(), zone.line, zone.id);
				continue;
			}
			targets.push_back(target);

			if (Install(target, zone.name, zone.category)) {
				++installed;
			}
		}

		REX::INFO("Installed {} of {} zone hooks from {}", installed, zones.size(), path.filename().string());
	}
}

// Called from the stub with the target's arguments saved; may clobber volatiles.
extern "C" void SET_ZoneHookEnter(EngineTelemetry::ZoneHookContext* a_context) noexcept
{
	using namespace EngineTelemetry::ZoneHooks;

	auto* active = CurrentActiveZones();
	if (!active) {
		return;
	}
	if (active->depth == kMaxActive) {
		++active->overflow;
		return;
	}

	auto& entry = active->entries[active->depth++];
	auto& data = *a_context->data;
	entry.zoneStack = EngineTelemetry::PushZone(data.site);
	new (entry.scope) basic_telemetry::Scope{ data.callsite };
	new (entry.tracyZone) tracy::ScopedZone{ &data.tracyLocation };
}

extern "C" void SET_ZoneHookExit(EngineTelemetry::ZoneHookContext*) noexcept
{
	using namespace EngineTelemetry::ZoneHooks;

	auto* active = t_active;
	if (!active) {
		return;
	}
	if (active->overflow) {
		--active->overflow;
		return;
	}
	if (active->depth == 0) {
		return;
	}

	auto& entry = active->entries[--active->depth];
	std::launder(reinterpret_cast<tracy::ScopedZone*>(entry.tracyZone))->~ScopedZone();
	std::launder(reinterpret_cast<basic_telemetry::Scope*>(entry.scope))->~Scope();
	EngineTelemetry::PopZone(entry.zoneStack);
}

// Language handler for the stub's frame. When an exception unwinds through a
// hooked call, close its zone so the per-thread stacks stay balanced.
extern "C" EXCEPTION_DISPOSITION SET_ZoneHookUnwind(EXCEPTION_RECORD* a_record, void* a_establisherFrame, CONTEXT*, void*)
{
	using namespace EngineTelemetry::ZoneHooks;

	if (a_record->ExceptionFlags & kUnwindFlags) {
		auto* frame = static_cast<std::byte*>(a_establisherFrame);
		auto& state = *reinterpret_cast<std::uint64_t*>(frame + kStateOffset);
		if (state == 1) {
			state = 0;
			SET_ZoneHookExit(*reinterpret_cast<EngineTelemetry::ZoneHookContext**>(frame + kContextOffset));
		}
	}
	return ExceptionContinueSearch;
}
