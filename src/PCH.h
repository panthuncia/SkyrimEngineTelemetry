#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include "detours/detours.h"

// wingdi.h defines ERROR, which collides with REX::ERROR.
#undef ERROR

#include <BasicTelemetry/Tracy.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::literals;
