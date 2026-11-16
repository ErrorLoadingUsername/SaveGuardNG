#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sg::win
{
	std::uintptr_t ModuleBase(const wchar_t* a_name);
	void           Lock(void* a_criticalSection);
	void           Unlock(void* a_criticalSection);
	std::wstring   DocumentsDir();  // FOLDERID_Documents, empty on failure
	std::size_t    CriticalSectionSize();
	bool           SafeClassName(void* a_obj, char* a_buf, std::size_t a_len);
	// Reads a C string through a pointer stored at a_obj + a_offset (SEH guarded).
	bool           SafeStringAt(const void* a_obj, std::size_t a_offset, char* a_buf, std::size_t a_len);
	bool           ReadableRange(const void* a_ptr, std::size_t a_len);

	// The loaded SKSE runtime DLL (skse64_1_5_97.dll, skse64_1_6_1170.dll, sksevr_1_4_15.dll, ...).
	std::uintptr_t FindSkseModule(std::wstring& a_nameOut);

	// A section of a loaded module (virtual size), false if missing.
	bool Section(std::uintptr_t a_base, const char* a_name, std::uintptr_t& a_begin, std::size_t& a_size);
}
