#include "ResourceLoading.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "util.h"
#include "globals.h"

#pragma region MappedFile

struct MemoryRangeEntry {
	PVOID VirtualAddress;
	SIZE_T NumberOfBytes;
};
typedef BOOL (WINAPI *PrefetchVirtualMemory_t)(HANDLE, ULONG_PTR, MemoryRangeEntry*, ULONG);

// Windows 8+; resolved at runtime so the DLL still loads on Windows 7,
// where the pages are simply faulted in on first touch instead:
static PrefetchVirtualMemory_t GetPrefetchVirtualMemory()
{
	static PrefetchVirtualMemory_t fn = (PrefetchVirtualMemory_t)
		GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory");
	return fn;
}

MappedFile::MappedFile(const wchar_t *path)
{
	LARGE_INTEGER file_size;

	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		error = GetLastError();
		return;
	}

	if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0
			|| (uint64_t)file_size.QuadPart > (uint64_t)SIZE_MAX) {
		error = GetLastError();
		if (!error)
			error = ERROR_FILE_INVALID;
		return;
	}

	mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
	if (!mapping) {
		error = GetLastError();
		return;
	}

	data = (const uint8_t*)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
	if (!data) {
		error = GetLastError();
		return;
	}

	size = (size_t)file_size.QuadPart;
}

MappedFile::~MappedFile()
{
	if (data)
		UnmapViewOfFile(data);
	if (mapping)
		CloseHandle(mapping);
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
}

void MappedFile::prefetch() const
{
	PrefetchVirtualMemory_t fn = GetPrefetchVirtualMemory();
	MemoryRangeEntry range;

	if (!fn || !data)
		return;

	range.VirtualAddress = (PVOID)data;
	range.NumberOfBytes = size;
	fn(GetCurrentProcess(), 1, &range, 0);
}

#pragma endregion MappedFile

#pragma region SharedFileResources

bool SharedResourceKey::operator==(const SharedResourceKey &other) const
{
	return device == other.device
		&& loader == other.loader
		&& bind_flags == other.bind_flags
		&& misc_flags == other.misc_flags
		&& load_flags == other.load_flags
		&& byte_width == other.byte_width
		&& structure_stride == other.structure_stride
		&& size == other.size
		&& content == other.content;
}

struct SharedEntry
{
	SharedResourceKey key;
	std::wstring path;
	ID3D11Resource *resource;
};

static std::mutex shared_resources_lock;
// Bucketed by file size:
static std::unordered_map<uint64_t, std::vector<SharedEntry>> shared_resources;

static bool FileContentsEqual(const std::wstring &path, const void *data, size_t size)
{
	MappedFile file(path.c_str());

	return file.valid() && file.size == size && !memcmp(file.data, data, size);
}

ID3D11Resource* FindSharedFileResource(SharedResourceKey &key, const void *data, size_t size)
{
	std::vector<SharedEntry> candidates;
	ID3D11Resource *match = nullptr;

	// Hashed from the data the caller is about to upload, never from the
	// file at some later point, so an entry always describes what its
	// resource was created from even if the file changes afterwards:
	key.size = size;
	key.content = crc32c_hw(0, data, size);

	{
		std::lock_guard<std::mutex> lock(shared_resources_lock);

		auto bucket = shared_resources.find(key.size);
		if (bucket == shared_resources.end())
			return nullptr;

		for (const SharedEntry &entry : bucket->second) {
			if (entry.key == key) {
				entry.resource->AddRef();
				candidates.push_back(entry);
			}
		}
	}

	// The hash only selects candidates. A candidate is shared once its
	// file still compares equal byte for byte; one whose file has since
	// changed or disappeared is simply not shared with:
	for (const SharedEntry &entry : candidates) {
		if (!match && FileContentsEqual(entry.path, data, size))
			match = entry.resource;
		else
			entry.resource->Release();
	}

	return match;
}

void ShareFileResource(const SharedResourceKey &key, const std::wstring &path, ID3D11Resource *resource)
{
	std::lock_guard<std::mutex> lock(shared_resources_lock);

	resource->AddRef();
	shared_resources[key.size].push_back(SharedEntry{key, path, resource});
}

void ReleaseSharedFileResources(ID3D11Device *device)
{
	std::vector<ID3D11Resource*> released;

	{
		std::lock_guard<std::mutex> lock(shared_resources_lock);

		for (auto bucket = shared_resources.begin(); bucket != shared_resources.end(); ) {
			std::vector<SharedEntry> &entries = bucket->second;
			for (auto entry = entries.begin(); entry != entries.end(); ) {
				if (device && entry->key.device != device) {
					entry++;
					continue;
				}
				released.push_back(entry->resource);
				entry = entries.erase(entry);
			}
			if (entries.empty())
				bucket = shared_resources.erase(bucket);
			else
				bucket++;
		}
	}

	// Released outside the lock: the final Release() calls into the driver.
	for (ID3D11Resource *resource : released)
		resource->Release();
}

#pragma endregion SharedFileResources
