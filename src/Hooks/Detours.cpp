#include "PCH.h"

#include "Hooks/Detours.h"

namespace EngineTelemetry
{
	DetourTransaction::DetourTransaction()
	{
		if (const auto error = DetourTransactionBegin(); error != NO_ERROR) {
			REX::ERROR("DetourTransactionBegin failed: {}", error);
			_failed = true;
			return;
		}
		_open = true;

		if (const auto error = DetourUpdateThread(GetCurrentThread()); error != NO_ERROR) {
			REX::ERROR("DetourUpdateThread failed: {}", error);
			_failed = true;
		}
	}

	DetourTransaction::~DetourTransaction()
	{
		if (_open) {
			DetourTransactionAbort();
		}
	}

	void DetourTransaction::Attach(void** a_target, void* a_detour, std::string_view a_name)
	{
		if (!_open || _failed) {
			return;
		}

		if (!a_target || !*a_target || !a_detour) {
			REX::ERROR("Cannot install detour '{}': invalid target or detour", a_name);
			_failed = true;
			return;
		}

		if (const auto error = DetourAttach(reinterpret_cast<PVOID*>(a_target), a_detour); error != NO_ERROR) {
			REX::ERROR("DetourAttach failed for '{}': {}", a_name, error);
			_failed = true;
			return;
		}

		_names.push_back(a_name);
	}

	bool DetourTransaction::Commit()
	{
		if (!_open) {
			return false;
		}
		_open = false;

		if (_failed) {
			DetourTransactionAbort();
			REX::ERROR("Aborted detour transaction ({} hooks queued)", _names.size());
			return false;
		}

		if (const auto error = DetourTransactionCommit(); error != NO_ERROR) {
			REX::ERROR("DetourTransactionCommit failed: {}", error);
			return false;
		}

		for (const auto name : _names) {
			REX::INFO("Installed detour '{}'", name);
		}
		return true;
	}
}
