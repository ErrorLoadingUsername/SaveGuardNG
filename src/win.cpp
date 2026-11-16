// Windows-only helpers, kept out of the CommonLib translation unit so that
// <Windows.h> cannot clash with CommonLibSSE's own Win32 declarations.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <Psapi.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "psapi.lib")

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <string>

#include "win.h"

namespace sg::win
{
	std::uintptr_t ModuleBase(const wchar_t* a_name)
	{
		return reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(a_name));
	}

	void Lock(void* a_criticalSection)
	{
		::EnterCriticalSection(static_cast<CRITICAL_SECTION*>(a_criticalSection));
	}

	void Unlock(void* a_criticalSection)
	{
		::LeaveCriticalSection(static_cast<CRITICAL_SECTION*>(a_criticalSection));
	}

	std::wstring DocumentsDir()
	{
		PWSTR       raw = nullptr;
		std::wstring result;
		if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
			result = raw;
		}
		if (raw) {
			::CoTaskMemFree(raw);
		}
		return result;
	}

	std::size_t CriticalSectionSize()
	{
		return sizeof(CRITICAL_SECTION);
	}

	// Calls ISKSEObject::ClassName() (vtable slot 1, after the destructor).
	// Guarded with SEH: a broken entry is exactly what we are looking for,
	// so reading it must never take the game down.
	static const char* ClassNameRaw(void* a_obj)
	{
		using Fn = const char* (*)(void*);
		auto vtbl = *static_cast<void***>(a_obj);
		return reinterpret_cast<Fn>(vtbl[1])(a_obj);
	}

	bool SafeClassName(void* a_obj, char* a_buf, std::size_t a_len)
	{
		if (!a_obj || !a_buf || a_len == 0) {
			return false;
		}
		__try {
			const char* name = ClassNameRaw(a_obj);
			if (!name) {
				return false;
			}
			std::size_t i = 0;
			for (; i + 1 < a_len && name[i] != '\0'; ++i) {
				a_buf[i] = name[i];
			}
			a_buf[i] = '\0';
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	std::uintptr_t FindSkseModule(std::wstring& a_nameOut)
	{
		HMODULE modules[1024];
		DWORD   needed = 0;
		if (!::EnumProcessModules(::GetCurrentProcess(), modules, sizeof(modules), &needed)) {
			return 0;
		}
		const auto count = (std::min)(static_cast<std::size_t>(needed / sizeof(HMODULE)), std::size(modules));
		for (std::size_t i = 0; i < count; ++i) {
			wchar_t name[MAX_PATH]{};
			if (!::GetModuleBaseNameW(::GetCurrentProcess(), modules[i], name, MAX_PATH)) {
				continue;
			}
			std::wstring n = name;
			for (auto& c : n) {
				c = static_cast<wchar_t>(::towlower(c));
			}
			const bool skse = n.rfind(L"skse64_", 0) == 0 || n.rfind(L"sksevr_", 0) == 0;
			const bool dll  = n.size() > 4 && n.compare(n.size() - 4, 4, L".dll") == 0;
			if (skse && dll && n.find(L"loader") == std::wstring::npos && n.find(L"steam") == std::wstring::npos) {
				a_nameOut = name;
				return reinterpret_cast<std::uintptr_t>(modules[i]);
			}
		}
		return 0;
	}

	bool Section(std::uintptr_t a_base, const char* a_name, std::uintptr_t& a_begin, std::size_t& a_size)
	{
		auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_base);
		if (!a_base || dos->e_magic != IMAGE_DOS_SIGNATURE) {
			return false;
		}
		auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(a_base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) {
			return false;
		}
		auto sec = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
			char name[9]{};
			std::memcpy(name, sec->Name, 8);
			if (std::strcmp(name, a_name) == 0) {
				a_begin = a_base + sec->VirtualAddress;
				a_size  = sec->Misc.VirtualSize;
				return true;
			}
		}
		return false;
	}

	bool SafeStringAt(const void* a_obj, std::size_t a_offset, char* a_buf, std::size_t a_len)
	{
		if (!a_obj || !a_buf || a_len == 0) {
			return false;
		}
		a_buf[0] = '\0';
		__try {
			const char* text = *reinterpret_cast<const char* const*>(static_cast<const char*>(a_obj) + a_offset);
			if (!text) {
				return false;
			}
			std::size_t i = 0;
			for (; i + 1 < a_len && text[i] != '\0'; ++i) {
				const char c = text[i];
				a_buf[i]     = (c >= 32 && c < 127) ? c : '?';
			}
			a_buf[i] = '\0';
			return i > 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_buf[0] = '\0';
			return false;
		}
	}

	bool ReadableRange(const void* a_ptr, std::size_t a_len)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		if (!::VirtualQuery(a_ptr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
			return false;
		}
		constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
		                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if ((mbi.Protect & readable) == 0 || (mbi.Protect & PAGE_GUARD)) {
			return false;
		}
		auto end = static_cast<const std::byte*>(mbi.BaseAddress) + mbi.RegionSize;
		return static_cast<const std::byte*>(a_ptr) + a_len <= end;
	}
}
