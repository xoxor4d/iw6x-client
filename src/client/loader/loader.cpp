#include <std_include.hpp>
#include "loader.hpp"
#include "seh.hpp"
#include "tls.hpp"
#include "utils/string.hpp"

FARPROC loader::load(const utils::nt::module& module, const std::string& buffer) const
{
	if (buffer.empty()) return nullptr;

	const utils::nt::module source(HMODULE(buffer.data()));
	if (!source) return nullptr;

	this->load_sections(module, source);
	this->load_imports(module, source);
	this->load_exception_table(module, source);

	if (source.get_optional_header()->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)
	{
		auto* target_tls = tls::allocate_tls_index();
		/* target_tls = reinterpret_cast<PIMAGE_TLS_DIRECTORY>(module.get_ptr() + module.get_optional_header()
				   ->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress); */
		auto* const source_tls = reinterpret_cast<PIMAGE_TLS_DIRECTORY>(module.get_ptr() + source.get_optional_header()
			->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);

		const auto tls_size = source_tls->EndAddressOfRawData - source_tls->StartAddressOfRawData;
		const auto tls_index = *reinterpret_cast<DWORD*>(target_tls->AddressOfIndex);
		*reinterpret_cast<DWORD*>(source_tls->AddressOfIndex) = tls_index;

		// I made sure it's large enough for IW6.
		/*if (tls_size > TLS_PAYLOAD_SIZE)
		{
			throw std::runtime_error(utils::string::va(
				"TLS data is of size 0x%X, but we have only reserved 0x%X bytes!", tls_size, TLS_PAYLOAD_SIZE));
		}*/

		DWORD old_protect;
		VirtualProtect(PVOID(target_tls->StartAddressOfRawData),
		               source_tls->EndAddressOfRawData - source_tls->StartAddressOfRawData, PAGE_READWRITE,
		               &old_protect);

		auto* const tls_base = *reinterpret_cast<LPVOID*>(__readgsqword(0x58) + 8ull * tls_index);
		std::memmove(tls_base, PVOID(source_tls->StartAddressOfRawData), tls_size);
		std::memmove(PVOID(target_tls->StartAddressOfRawData), PVOID(source_tls->StartAddressOfRawData), tls_size);

		VirtualProtect(target_tls, sizeof(*target_tls), PAGE_READWRITE, &old_protect);
		*target_tls = *source_tls;
	}

	DWORD oldProtect;
	VirtualProtect(module.get_nt_headers(), 0x1000, PAGE_EXECUTE_READWRITE, &oldProtect);

	module.get_optional_header()->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = source
		.get_optional_header()->DataDirectory[
			IMAGE_DIRECTORY_ENTRY_IMPORT];
	std::memmove(module.get_nt_headers(), source.get_nt_headers(),
	             sizeof(IMAGE_NT_HEADERS) + source.get_nt_headers()->FileHeader.NumberOfSections * sizeof(
		             IMAGE_SECTION_HEADER));

	return FARPROC(module.get_ptr() + source.get_relative_entry_point());
}

void loader::set_import_resolver(const std::function<FARPROC(const std::string&, const std::string&)>& resolver)
{
	this->import_resolver_ = resolver;
}

void loader::load_section(const utils::nt::module& target, const utils::nt::module& source,
                          IMAGE_SECTION_HEADER* section)
{
	void* target_ptr = target.get_ptr() + section->VirtualAddress;
	const void* source_ptr = source.get_ptr() + section->PointerToRawData;

	if (PBYTE(target_ptr) >= (target.get_ptr() + BINARY_PAYLOAD_SIZE))
	{
		throw std::runtime_error("Section exceeds the binary payload size, please increase it!");
	}

	if (section->SizeOfRawData > 0)
	{
		std::memmove(target_ptr, source_ptr, section->SizeOfRawData);

		DWORD old_protect;
		VirtualProtect(target_ptr, section->Misc.VirtualSize, PAGE_EXECUTE_READWRITE, &old_protect);
	}
}

void loader::load_sections(const utils::nt::module& target, const utils::nt::module& source) const
{
	for (auto& section : source.get_section_headers())
	{
		this->load_section(target, source, section);
	}
}

void loader::load_imports(const utils::nt::module& target, const utils::nt::module& source) const
{
	auto* const import_directory = &source.get_optional_header()->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

	auto* descriptor = PIMAGE_IMPORT_DESCRIPTOR(target.get_ptr() + import_directory->VirtualAddress);

	while (descriptor->Name)
	{
		std::string name = LPSTR(target.get_ptr() + descriptor->Name);

		auto* name_table_entry = reinterpret_cast<uintptr_t*>(target.get_ptr() + descriptor->OriginalFirstThunk);
		auto* address_table_entry = reinterpret_cast<uintptr_t*>(target.get_ptr() + descriptor->FirstThunk);

		if (!descriptor->OriginalFirstThunk)
		{
			name_table_entry = reinterpret_cast<uintptr_t*>(target.get_ptr() + descriptor->FirstThunk);
		}

		while (*name_table_entry)
		{
			FARPROC function = nullptr;
			std::string function_name;

			// is this an ordinal-only import?
			if (IMAGE_SNAP_BY_ORDINAL(*name_table_entry))
			{
				auto module = utils::nt::module::load(name);
				if (module)
				{
					function = GetProcAddress(module, MAKEINTRESOURCEA(IMAGE_ORDINAL(*name_table_entry)));
				}

				function_name = "#" + std::to_string(IMAGE_ORDINAL(*name_table_entry));
			}
			else
			{
				auto* import = PIMAGE_IMPORT_BY_NAME(target.get_ptr() + *name_table_entry);
				function_name = import->Name;

				if (this->import_resolver_) function = this->import_resolver_(name, function_name);
				if (!function)
				{
					auto module = utils::nt::module::load(name);
					if (module)
					{
						function = GetProcAddress(module, function_name.data());
					}
				}
			}

			if (!function)
			{
				throw std::runtime_error(utils::string::va("Unable to load import '%s' from module '%s'",
				                                           function_name.data(), name.data()));
			}

			*address_table_entry = reinterpret_cast<uintptr_t>(function);

			name_table_entry++;
			address_table_entry++;
		}

		descriptor++;
	}
}

void loader::load_exception_table(const utils::nt::module& target, const utils::nt::module& source) const
{
	auto* exception_directory = &source.get_optional_header()->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];

	auto* function_list = PRUNTIME_FUNCTION(target.get_ptr() + exception_directory->VirtualAddress);
	const auto entry_count = ULONG(exception_directory->Size / sizeof(RUNTIME_FUNCTION));

	if (!RtlAddFunctionTable(function_list, entry_count, DWORD64(target.get_ptr())))
	{
		MessageBoxA(nullptr, "Setting exception handlers failed.", "Error", MB_OK | MB_ICONERROR);
	}

	{
		const utils::nt::module ntdll("ntdll.dll");

		auto* const table_list_head = ntdll.invoke_pascal<PLIST_ENTRY>("RtlGetFunctionTableListHead");
		auto* table_list_entry = table_list_head->Flink;

		while (table_list_entry != table_list_head)
		{
			auto* const function_table = CONTAINING_RECORD(table_list_entry, DYNAMIC_FUNCTION_TABLE, Links);

			if (function_table->BaseAddress == ULONG_PTR(target.get_handle()))
			{
				function_table->EntryCount = entry_count;
				function_table->FunctionTable = function_list;
			}

			table_list_entry = function_table->Links.Flink;
		}
	}

	seh::setup_handler(target.get_ptr(), target.get_ptr() + source.get_optional_header()->SizeOfImage, function_list,
	                   entry_count);
}
