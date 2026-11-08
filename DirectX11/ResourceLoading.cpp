#include "ResourceLoading.h"

#include <algorithm>
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

bool MappedFile::identity(FileIdentity *identity) const
{
	return file != INVALID_HANDLE_VALUE && GetFileIdentity(file, identity);
}

#pragma endregion MappedFile

#pragma region FileIdentity

bool FileIdentity::operator==(const FileIdentity &other) const
{
	return volume == other.volume
		&& index == other.index
		&& size == other.size
		&& last_write == other.last_write
		&& change == other.change;
}

bool GetFileIdentity(HANDLE file, FileIdentity *identity)
{
	BY_HANDLE_FILE_INFORMATION info;
	FILE_BASIC_INFO basic;

	if (!GetFileInformationByHandle(file, &info)
			|| !GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)))
		return false;

	// A file system without file IDs cannot tell two files apart:
	if (!info.nFileIndexHigh && !info.nFileIndexLow)
		return false;

	identity->volume = info.dwVolumeSerialNumber;
	identity->index = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
	identity->size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
	identity->last_write = basic.LastWriteTime.QuadPart;
	identity->change = basic.ChangeTime.QuadPart;
	return true;
}

bool GetFileIdentity(const wchar_t *path, FileIdentity *identity)
{
	HANDLE file;
	bool ok;

	file = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	ok = GetFileIdentity(file, identity);
	CloseHandle(file);
	return ok;
}

#pragma endregion FileIdentity

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

// Everything but the contents, which a matching file identity implies:
static bool SameCreation(const SharedResourceKey &a, const SharedResourceKey &b)
{
	return a.device == b.device
		&& a.loader == b.loader
		&& a.bind_flags == b.bind_flags
		&& a.misc_flags == b.misc_flags
		&& a.load_flags == b.load_flags
		&& a.byte_width == b.byte_width
		&& a.structure_stride == b.structure_stride;
}

static std::wstring LowerPath(const std::wstring &path)
{
	std::wstring lower(path);

	for (wchar_t &c : lower)
		c = fast_towlower(c);
	return lower;
}

struct SharedFile
{
	std::wstring path;
	std::wstring lower_path;
	FileIdentity identity;
	bool has_identity;
};

struct SharedEntry
{
	SharedResourceKey key;
	// Every file the resource was created from or found identical to:
	std::vector<SharedFile> files;
	ID3D11Resource *resource;
	// The config generation that last used it:
	unsigned generation;
};

static std::mutex shared_resources_lock;
// Bucketed by file size:
static std::unordered_map<uint64_t, std::vector<SharedEntry>> shared_resources;
// Counts config reloads:
static unsigned shared_generation = 0;

static bool FileContentsEqual(const std::wstring &path, const void *data, size_t size)
{
	MappedFile file(path.c_str());

	return file.valid() && file.size == size && !memcmp(file.data, data, size);
}

// Adds the file to those the entry is known by, or updates its identity:
static void RememberSharedFile(SharedEntry &entry, const std::wstring &path, const FileIdentity *identity)
{
	std::wstring lower = LowerPath(path);

	for (SharedFile &file : entry.files) {
		if (file.lower_path != lower)
			continue;
		file.has_identity = !!identity;
		if (identity)
			file.identity = *identity;
		return;
	}

	entry.files.push_back(SharedFile{path, lower, identity ? *identity : FileIdentity(), !!identity});
}

ID3D11Resource* FindSharedFileResource(SharedResourceKey &key, const void *data, size_t size,
		const std::wstring *path, const FileIdentity *path_identity)
{
	struct Candidate {
		ID3D11Resource *resource;
		std::vector<std::wstring> paths;
	};
	std::vector<Candidate> candidates;
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
				Candidate candidate{entry.resource};
				for (const SharedFile &file : entry.files)
					candidate.paths.push_back(file.path);
				entry.resource->AddRef();
				candidates.push_back(std::move(candidate));
			}
		}
	}

	// The hash only selects candidates. A candidate is shared once one of
	// its files still compares equal byte for byte; one whose files have
	// all since changed or disappeared is simply not shared with:
	for (const Candidate &candidate : candidates) {
		bool equal = false;
		if (!match) {
			for (const std::wstring &candidate_path : candidate.paths) {
				if (FileContentsEqual(candidate_path, data, size)) {
					equal = true;
					break;
				}
			}
		}
		if (equal)
			match = candidate.resource;
		else
			candidate.resource->Release();
	}

	// Known by this file too from now on, so the next config finds it by
	// identity, and keeps it while any of its files is still in use:
	if (match && path) {
		std::lock_guard<std::mutex> lock(shared_resources_lock);

		auto bucket = shared_resources.find(key.size);
		if (bucket != shared_resources.end()) {
			for (SharedEntry &entry : bucket->second) {
				if (entry.resource == match && entry.key == key) {
					RememberSharedFile(entry, *path, path_identity);
					entry.generation = shared_generation;
					break;
				}
			}
		}
	}

	return match;
}

void ShareFileResource(const SharedResourceKey &key, const std::wstring &path, ID3D11Resource *resource,
		const FileIdentity *path_identity)
{
	std::lock_guard<std::mutex> lock(shared_resources_lock);
	SharedEntry entry{key, {}, resource, shared_generation};

	RememberSharedFile(entry, path, path_identity);
	resource->AddRef();
	shared_resources[key.size].push_back(std::move(entry));
}

ID3D11Resource* FindRetainedFileResource(const SharedResourceKey &key, const std::wstring &path,
		const FileIdentity &identity)
{
	std::vector<ID3D11Resource*> released;
	std::wstring lower = LowerPath(path);
	ID3D11Resource *match = nullptr;

	{
		std::lock_guard<std::mutex> lock(shared_resources_lock);

		auto bucket = shared_resources.find(identity.size);
		if (bucket == shared_resources.end())
			return nullptr;

		for (auto entry = bucket->second.begin(); entry != bucket->second.end(); ) {
			bool stale = false;

			for (auto file = entry->files.begin(); file != entry->files.end(); ) {
				if (file->has_identity && file->identity == identity) {
					if (!match && SameCreation(entry->key, key)) {
						match = entry->resource;
						match->AddRef();
						entry->generation = shared_generation;
					}
					file++;
					continue;
				}
				// The file at this path has been written to since
				// the resource was created from it:
				if (file->lower_path == lower) {
					file = entry->files.erase(file);
					stale = true;
					continue;
				}
				file++;
			}

			if (stale && entry->files.empty()) {
				released.push_back(entry->resource);
				entry = bucket->second.erase(entry);
				continue;
			}
			entry++;
		}

		// Found through a renamed folder, for instance:
		if (match) {
			for (SharedEntry &entry : bucket->second) {
				if (entry.resource == match) {
					RememberSharedFile(entry, path, &identity);
					break;
				}
			}
		}

		if (bucket->second.empty())
			shared_resources.erase(bucket);
	}

	// Released outside the lock: the final Release() calls into the driver.
	for (ID3D11Resource *resource : released)
		resource->Release();

	return match;
}

void RetainSharedFileResources(const std::unordered_set<std::wstring> &lower_paths)
{
	std::vector<ID3D11Resource*> released;
	size_t kept = 0;

	{
		std::lock_guard<std::mutex> lock(shared_resources_lock);

		for (auto bucket = shared_resources.begin(); bucket != shared_resources.end(); ) {
			std::vector<SharedEntry> &entries = bucket->second;
			for (auto entry = entries.begin(); entry != entries.end(); ) {
				// One that went unused since the previous reload goes,
				// so a resource whose section was changed to load the
				// file differently does not linger:
				if (entry->generation == shared_generation) {
					auto &files = entry->files;
					files.erase(std::remove_if(files.begin(), files.end(), [&](const SharedFile &file) {
						return !lower_paths.count(file.lower_path);
					}), files.end());
					if (!files.empty()) {
						kept++;
						entry++;
						continue;
					}
				}
				released.push_back(entry->resource);
				entry = entries.erase(entry);
			}
			if (entries.empty())
				bucket = shared_resources.erase(bucket);
			else
				bucket++;
		}

		shared_generation++;
	}

	LogInfo("Kept %Iu file resources across the config reload, released %Iu\n", kept, released.size());

	// Released outside the lock: the final Release() calls into the driver.
	for (ID3D11Resource *resource : released)
		resource->Release();
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
