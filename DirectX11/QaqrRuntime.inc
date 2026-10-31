// QAQR runtime additions. The activity channel is independent of persistence.
#include <io.h>
#include <mutex>

static std::vector<CommandListVariable*> dirty_persistent_variables;
static bool persistent_journal_dirty = false;
static std::recursive_mutex persistent_mutex;

static bool SamePersistentValue(float a, float b)
{
	return a == b || (std::isnan(a) && std::isnan(b));
}

void MarkPersistentVariableDirty(CommandListVariable* var)
{
	std::lock_guard<std::recursive_mutex> lock(persistent_mutex);
	if (!var->persist_queued) {
		var->persist_queued = true;
		dirty_persistent_variables.push_back(var);
	}
	G->user_config_dirty = true;
}

void ResetPersistentTracking()
{
	std::lock_guard<std::recursive_mutex> lock(persistent_mutex);
	dirty_persistent_variables.clear();
	for (auto var : persistent_variables) {
		var->persist_saved_value = var->fval;
		var->persist_queued = false;
	}
	G->user_config_dirty = false;
}

static bool HasPersistentChanges()
{
	for (auto var : dirty_persistent_variables)
		if (!SamePersistentValue(var->fval, var->persist_saved_value))
			return true;
	for (auto var : dirty_persistent_variables)
		var->persist_queued = false;
	dirty_persistent_variables.clear();
	G->user_config_dirty = false;
	return false;
}

// [Constants] is an ordered command list. Appending only changed assignments
// retains compatibility with stock XXMI and with the manager's last-value reader.
// The ordinary reload path compacts the journal; no full scan or rewrite happens
// on the periodic save path. Reverted changes never reach the filesystem.
static bool SaveIncrementalSettings()
{
	std::string changes;
	std::vector<std::pair<CommandListVariable*, float>> values;
	for (auto var : dirty_persistent_variables) {
		const float value = var->fval;
		if (SamePersistentValue(value, var->persist_saved_value)) continue;
		const wstring& key = persistent_variable_save_name(var);
		int size = WideCharToMultiByte(CP_UTF8, 0, key.data(), (int)key.size(), NULL, 0, NULL, NULL);
		if (!size) return false;
		std::string name(size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, key.data(), (int)key.size(), &name[0], size, NULL, NULL);
		char number[64];
		sprintf_s(number, " = %.9g\n", value);
		changes += name + number;
		values.emplace_back(var, value);
	}
	if (changes.empty()) return false;
	HANDLE file = CreateFileW(G->user_config.c_str(), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER start = {}, zero = {};
	bool ok = SetFilePointerEx(file, zero, &start, FILE_END) != 0;
	if (ok) {
		changes.insert(0, start.QuadPart ? "\n; QAQR incremental update\n" : "; QAQR incremental update\n[Constants]\n");
		DWORD written = 0;
		ok = changes.size() <= MAXDWORD && WriteFile(file, changes.data(), (DWORD)changes.size(), &written, NULL)
			&& written == changes.size();
		if (!ok && SetFilePointerEx(file, start, NULL, FILE_BEGIN)) SetEndOfFile(file);
	}
	CloseHandle(file);
	if (!ok) return false; // Keep the dirty queue for the next save attempt.
	for (auto var : dirty_persistent_variables) {
		var->persist_queued = false;
	}
	for (const auto& value : values) value.first->persist_saved_value = value.second;
	dirty_persistent_variables.clear();
	G->user_config_dirty = false;
	persistent_journal_dirty = true;
	return true;
}

// Header: magic, version, sequence, token, tick(float), count, flags(float[]).
// Only CPU memory is copied, at most four times per second. There is no file I/O,
// GPU readback, or persistent heartbeat. Sequence protects readers from torn data.
static HANDLE activity_mapping = NULL;
static DWORD* activity_view = NULL;
static std::vector<CommandListVariable*> activity_variables;
static float activity_last_publish = -1;

void ResetActivityChannel()
{
	if (activity_view) UnmapViewOfFile(activity_view);
	if (activity_mapping) CloseHandle(activity_mapping);
	activity_view = NULL;
	activity_mapping = NULL;
	activity_variables.clear();
	activity_last_publish = -1;
}

void InitializeActivityChannel()
{
	ResetActivityChannel();
	auto token_it = command_list_globals.find(L"$\\qaqm\\activity\\token");
	if (token_it == command_list_globals.end()) return;
	float token = token_it->second.fval;
	if (!(token >= 1 && token <= 0xffffff) || token != (DWORD)token) return;
	for (unsigned i = 0; i < 4096; ++i) {
		auto it = command_list_globals.find(L"$\\qaqm\\activity\\m" + std::to_wstring(i));
		if (it == command_list_globals.end()) break;
		activity_variables.push_back(&it->second);
	}
	if (activity_variables.empty()) return;
	wchar_t name[96];
	swprintf_s(name, L"Local\\QAQR.Activity.v1.%u", (DWORD)token);
	DWORD bytes = (DWORD)(6 + activity_variables.size()) * sizeof(DWORD);
	activity_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, bytes, name);
	if (!activity_mapping) return;
	activity_view = (DWORD*)MapViewOfFile(activity_mapping, FILE_MAP_WRITE, 0, 0, bytes);
	if (!activity_view) { ResetActivityChannel(); return; }
	InterlockedExchange((volatile LONG*)&activity_view[2], 1);
	activity_view[0] = 0x31524151;
	activity_view[1] = 1;
	activity_view[3] = (DWORD)token;
	activity_view[4] = 0;
	activity_view[5] = (DWORD)activity_variables.size();
	for (size_t i = 0; i < activity_variables.size(); ++i) activity_view[6 + i] = 0;
	MemoryBarrier();
	InterlockedExchange((volatile LONG*)&activity_view[2], 2);
}

void PublishActivityChannel()
{
	if (!activity_view || G->gTime - activity_last_publish < .25f) return;
	activity_last_publish = G->gTime;
	InterlockedIncrement((volatile LONG*)&activity_view[2]);
	memcpy(&activity_view[4], &G->gTime, sizeof(float));
	for (size_t i = 0; i < activity_variables.size(); ++i)
		memcpy(&activity_view[6 + i], &activity_variables[i]->fval, sizeof(float));
	MemoryBarrier();
	InterlockedIncrement((volatile LONG*)&activity_view[2]);
}
