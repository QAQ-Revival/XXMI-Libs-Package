// Offline probe for what a config reload (F10) keeps: [Resource] files that
// have not changed keep their GPU resource, a changed or written one does
// not, a Mod folder that is renamed away releases its resources, and a
// ShaderRegex patch is put back without being made again while the
// ShaderRegex sections are unchanged. Also checks that a deep "else if"
// chain, which the command list optimiser collapses, still picks the right
// branch. Runs the real DLL on a WARP device and presses F10 through a
// GetAsyncKeyState hook local to this process.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static void check(HRESULT hr, const char *what)
{
	if (FAILED(hr)) { std::ostringstream s; s << what << " HRESULT=0x" << std::hex << (unsigned)hr; throw std::runtime_error(s.str()); }
}
static void require(bool ok, const std::string &what) { if (!ok) throw std::runtime_error(what); }

static volatile bool f10_down = false;
static SHORT WINAPI FakeGetAsyncKeyState(int vk) { return (vk == VK_F10 && f10_down) ? (SHORT)0x8000 : 0; }

static void PatchImport(HMODULE module, const char *dll, const char *fn, void *replacement)
{
	BYTE *base = (BYTE*)module;
	auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
		if (_stricmp((char*)(base + imp->Name), dll))
			continue;
		auto thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
		auto orig = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
		for (; orig->u1.AddressOfData; orig++, thunk++) {
			if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal))
				continue;
			if (strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData))->Name, fn))
				continue;
			DWORD old;
			VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
			thunk->u1.Function = (ULONG_PTR)replacement;
			VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
			return;
		}
	}
	throw std::runtime_error("GetAsyncKeyState import not found");
}

// 4x4 R8G8B8A8 DDS, the format most Mod textures load through:
static void WriteDds(const fs::path &path, unsigned seed)
{
	DWORD header[32] = {};
	header[0] = 0x20534444;		// "DDS "
	header[1] = 124;		// dwSize
	header[2] = 0x1007;		// caps | height | width | pixelformat
	header[3] = 4;			// height
	header[4] = 4;			// width
	header[5] = 16;			// pitch
	header[7] = 1;			// mips
	header[19] = 32;		// ddspf.dwSize
	header[20] = 0x41;		// DDPF_RGB | DDPF_ALPHAPIXELS
	header[22] = 32;		// bit count
	header[23] = 0x000000ff;
	header[24] = 0x0000ff00;
	header[25] = 0x00ff0000;
	header[26] = 0xff000000;
	header[27] = 0x1000;		// DDSCAPS_TEXTURE
	unsigned char pixels[64];
	for (unsigned i = 0; i < sizeof(pixels); i++) pixels[i] = (unsigned char)(seed * 41 + i * 7);
	fs::create_directories(path.parent_path());
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f.write((char*)header, sizeof(header));
	f.write((char*)pixels, sizeof(pixels));
}

static void WriteBuffer(const fs::path &path, unsigned seed)
{
	std::vector<unsigned> data(64);
	for (unsigned i = 0; i < data.size(); i++) data[i] = seed * 0x1000 + i;
	fs::create_directories(path.parent_path());
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f.write((char*)data.data(), data.size() * 4);
}

static unsigned CountLines(const fs::path &log, const std::string &needle, size_t from_line = 0)
{
	std::ifstream f(log);
	std::string line;
	unsigned n = 0;
	for (size_t i = 0; std::getline(f, line); i++)
		if (i >= from_line && line.find(needle) != std::string::npos) n++;
	return n;
}

static size_t LogLines(const fs::path &log)
{
	std::ifstream f(log);
	std::string line;
	size_t n = 0;
	while (std::getline(f, line)) n++;
	return n;
}

enum Slot { A_TEX, A_BUF, B_TEX, WRITTEN, SLOTS };
static const char *slot_names[] = { "ModA texture", "ModA buffer", "ModB texture", "written texture" };

int wmain(int argc, wchar_t **argv)
{
	ComPtr<ID3D11Device> device;
	try {
		require(argc >= 3, "Usage: reload.exe DLL on|off");
		const std::wstring mode = argv[2];
		require(mode == L"on" || mode == L"off", "Unknown test mode");
		const bool on = mode == L"on";
		const auto work = fs::current_path();
		for (auto ancestor = work;; ancestor = ancestor.parent_path()) {
			for (const wchar_t *name : { L"Mods", L"Core", L"ShaderFixes", L"ShaderCache" })
				require(lstrcmpiW(ancestor.filename().c_str(), name) != 0, "Test outputs must be outside live importer and Mod directories");
			require(!fs::exists(ancestor / "d3dx.ini"), "Test outputs must be outside live importer and Mod directories");
			if (ancestor == ancestor.parent_path()) break;
		}
		fs::path dir = work / ((on ? std::string("reload-test-on-") : std::string("reload-test-off-")) + std::to_string(GetCurrentProcessId()));
		fs::create_directories(dir / "ShaderCache");
		fs::copy_file(fs::absolute(argv[1]), dir / "d3d11.dll", fs::copy_options::overwrite_existing);
		const fs::path log = dir / "d3d11_log.txt";

		WriteDds(dir / "Mods" / "ModA" / "tex.dds", 1);
		WriteBuffer(dir / "Mods" / "ModA" / "buf.buf", 2);
		WriteDds(dir / "Mods" / "ModB" / "tex.dds", 3);
		WriteDds(dir / "Mods" / "ModB" / "written.dds", 4);
		{ std::ofstream f(dir / "Mods" / "ModA" / "mod.ini"); f << "namespace = ModA\n[ResourceTex]\nfilename = tex.dds\n[ResourceBuf]\ntype = Buffer\nformat = R32_UINT\nfilename = buf.buf\n"; }
		{ std::ofstream f(dir / "Mods" / "ModB" / "mod.ini"); f << "namespace = ModB\n[ResourceTex]\nfilename = tex.dds\n[ResourceWritten]\nfilename = written.dds\n"; }

		// The trigger shader stores 1, which the ShaderRegex turns into
		// the value written below:
		const char *hlsl = "RWByteAddressBuffer o : register(u1);\n[numthreads(1,1,1)] void main() { o.Store(0, 1); }";
		ComPtr<ID3DBlob> bytecode, errors;
		check(D3DCompile(hlsl, strlen(hlsl), "ReloadTrigger", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors), "Compile trigger");
		auto bytes = (const unsigned char*)bytecode->GetBufferPointer();
		unsigned long long hash = 0;
		for (int i = 4; i < 12; i++) hash = (hash << 8) | bytes[i];

		auto write_ini = [&](unsigned regex_value) {
			std::ofstream ini(dir / "d3dx.ini", std::ios::trunc);
			ini << "[System]\ncheck_foreground_window = 0\nallow_check_interface = 1\nallow_create_device = 1\nallow_platform_update = 1\n"
			       "skip_early_includes_load = 0\nsettings_auto_save_interval = -1\n"
			       "[Hunting]\nhunting = 0\nreload_config = no_modifiers VK_F10\n"
			       "[Logging]\nlog_level = info\nunbuffered = 1\nshow_warnings = 0\n"
			       "[Rendering]\nshader_hash = embedded\ncache_shaders = 0\ncache_directory = ShaderCache\n"
			       "share_duplicate_resources = " << (on ? 1 : 0) << "\n"
			       "[Include]\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\ninclude = activity.ini\n"
			       "[Constants]\nglobal $never = 0\nglobal $pick = 57\nglobal $picked = 0\n"
			       "[Present]\n$\\QAQM\\Activity\\m0 = $picked\n"
			       "[ShaderRegexReloadTest]\nshader_model = cs_5_0\n"
			       "[ShaderRegexReloadTest.Pattern]\n(?<head>store_raw u1\\.x, l\\(0\\), )l\\(1\\)\n"
			       "[ShaderRegexReloadTest.Pattern.Replace]\n${head}l(" << regex_value << ")\n"
			       "[ShaderOverrideTrigger]\nhash = " << std::hex << std::setfill('0') << std::setw(16) << hash << std::dec << "\n"
			       "run = CommandListTest\n[CommandListTest]\n"
			       "cs-t0 = ref Resource\\ModA\\Tex\n"
			       "cs-t1 = ref Resource\\ModA\\Buf\n"
			       "cs-t2 = ref Resource\\ModB\\Tex\n"
			       "cs-t3 = ref Resource\\ModB\\Written\n"
			       "if $never == 1\nResource\\ModB\\Written = copy Resource\\ModA\\Tex\nendif\n";
			// One level of nesting per branch; all but one are empty
			// and optimised out, the last one assigns:
			ini << "if $pick == 0\n";
			for (int i = 1; i < 70; i++) {
				ini << "else if $pick == " << i << "\n";
				if (i == 57) ini << "$picked = 57\n";
			}
			ini << "endif\n";
		};
		write_ini(7);
		{ std::ofstream f(dir / "activity.ini"); f << "namespace = QAQM\\Activity\n[Constants]\nglobal $token = 515151\nglobal $m0 = 0\n"; }

		HMODULE dll = LoadLibraryW((dir / L"d3d11.dll").c_str());
		require(dll != nullptr, "Load XXMI DLL");
		PatchImport(dll, "user32.dll", "GetAsyncKeyState", (void*)FakeGetAsyncKeyState);
		auto create = (decltype(&D3D11CreateDeviceAndSwapChain))GetProcAddress(dll, "D3D11CreateDeviceAndSwapChain");
		require(create != nullptr, "Find create export");
		HWND window = CreateWindowExW(0, L"STATIC", L"QAQR isolated reload verification", WS_OVERLAPPEDWINDOW, 0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		require(window != nullptr, "Create hidden test window");
		DXGI_SWAP_CHAIN_DESC desc = {};
		desc.BufferDesc.Width = desc.BufferDesc.Height = 32;
		desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = 1;
		desc.OutputWindow = window;
		desc.Windowed = TRUE;
		ComPtr<ID3D11DeviceContext> context;
		ComPtr<IDXGISwapChain> swap;
		D3D_FEATURE_LEVEL level;
		check(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swap, &device, &level, &context), "Create WARP swapchain through XXMI");

		ComPtr<ID3D11ComputeShader> shader;
		check(device->CreateComputeShader(bytes, bytecode->GetBufferSize(), nullptr, &shader), "Create trigger shader");
		D3D11_BUFFER_DESC out_desc = {};
		out_desc.ByteWidth = 16;
		out_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		out_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		ComPtr<ID3D11Buffer> out, staging;
		check(device->CreateBuffer(&out_desc, nullptr, &out), "Create output buffer");
		out_desc.BindFlags = 0;
		out_desc.MiscFlags = 0;
		out_desc.Usage = D3D11_USAGE_STAGING;
		out_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		check(device->CreateBuffer(&out_desc, nullptr, &staging), "Create staging buffer");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
		uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
		uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav_desc.Buffer.NumElements = 4;
		uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		ComPtr<ID3D11UnorderedAccessView> uav;
		check(device->CreateUnorderedAccessView(out.Get(), &uav_desc, &uav), "Create output view");

		auto present = [&]() { Sleep(20); check(swap->Present(0, 0), "Present"); };
		auto reload = [&]() {
			f10_down = true;
			present();
			f10_down = false;
			present();
		};
		// Holding the resources of every step keeps their addresses from
		// being reused, so equal pointers do mean the same resource:
		std::vector<std::vector<ComPtr<ID3D11Resource>>> held;
		unsigned stored = 0;
		auto dispatch = [&]() {
			ID3D11ShaderResourceView *none[SLOTS] = {};
			const UINT zero = 0;
			ID3D11UnorderedAccessView *views[] = { uav.Get() };
			context->CSSetShader(shader.Get(), nullptr, 0);
			context->CSSetShaderResources(0, SLOTS, none);
			context->CSSetUnorderedAccessViews(1, 1, views, &zero);
			context->Dispatch(1, 1, 1);
			std::vector<ComPtr<ID3D11Resource>> res(SLOTS);
			for (unsigned i = 0; i < SLOTS; i++) {
				ComPtr<ID3D11ShaderResourceView> view;
				context->CSGetShaderResources(i, 1, &view);
				if (view)
					view->GetResource(&res[i]);
			}
			context->CopyResource(staging.Get(), out.Get());
			D3D11_MAPPED_SUBRESOURCE mapped;
			check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map output");
			stored = *(unsigned*)mapped.pData;
			context->Unmap(staging.Get(), 0);
			held.push_back(res);
			return res;
		};
		auto expect_bound = [&](const std::vector<ComPtr<ID3D11Resource>> &res, const char *step) {
			for (unsigned i = 0; i < SLOTS; i++)
				require(res[i] != nullptr, std::string(step) + ": SRV missing for " + slot_names[i]);
		};
		auto same = [&](const std::vector<ComPtr<ID3D11Resource>> &a, const std::vector<ComPtr<ID3D11Resource>> &b, Slot slot) {
			return a[slot].Get() == b[slot].Get();
		};

		for (int i = 0; i < 5; i++) present();

		auto r0 = dispatch();
		expect_bound(r0, "Initial load");
		require(stored == 7, "ShaderRegex patch not applied on the first load, stored " + std::to_string(stored));
		size_t mark = LogLines(log);

		// 1. Nothing changed: every file resource is kept, the written one is not:
		reload();
		auto r1 = dispatch();
		expect_bound(r1, "Unchanged reload");
		require(same(r0, r1, A_TEX) == on, on ? "Unchanged texture was loaded again" : "Texture kept with sharing disabled");
		require(same(r0, r1, A_BUF) == on, on ? "Unchanged buffer was loaded again" : "Buffer kept with sharing disabled");
		require(same(r0, r1, B_TEX) == on, on ? "Unchanged second Mod texture was loaded again" : "Texture kept with sharing disabled");
		require(!same(r0, r1, WRITTEN), "A resource a command list writes into was kept");
		require(stored == 7, "ShaderRegex patch lost after an unchanged reload, stored " + std::to_string(stored));
		require(CountLines(log, "Performing deferred shader analysis", mark) == 0, "Shader analysed again after an unchanged reload");
		mark = LogLines(log);

		// 2. One file rewritten: only that one loads again:
		WriteDds(dir / "Mods" / "ModB" / "tex.dds", 5);
		reload();
		auto r2 = dispatch();
		expect_bound(r2, "Changed file reload");
		require(!same(r1, r2, B_TEX), "A rewritten texture kept its old contents");
		require(same(r1, r2, A_TEX) == on, "Unchanged texture next to a changed one was not kept");
		D3D11_TEXTURE2D_DESC tex_desc;
		ComPtr<ID3D11Texture2D> tex;
		check(r2[B_TEX].As(&tex), "Reloaded texture type");
		tex->GetDesc(&tex_desc);
		require(tex_desc.Width == 4 && tex_desc.Height == 4, "Reloaded texture description");
		mark = LogLines(log);

		// 3. A Mod folder renamed away, as disabling it does: its sections
		// are gone, and so are its resources once nothing holds them:
		fs::rename(dir / "Mods" / "ModA", dir / "Mods" / "DISABLED_ModA");
		reload();
		auto r3 = dispatch();
		require(!r3[A_TEX] && !r3[A_BUF], "Disabled Mod resources still bound");
		if (on)
			require(CountLines(log, "released 2", mark) == 1, "Disabled Mod resources were not released");
		fs::rename(dir / "Mods" / "DISABLED_ModA", dir / "Mods" / "ModA");
		reload();
		auto r4 = dispatch();
		expect_bound(r4, "Enabled again");
		require(!same(r1, r4, A_TEX), "Texture of a Mod disabled in between came back without loading");
		mark = LogLines(log);

		// 4. A ShaderRegex changed: the shader is analysed and patched again:
		write_ini(9);
		reload();
		dispatch();
		require(stored == 9, "Changed ShaderRegex not applied after reload, stored " + std::to_string(stored));
		require(CountLines(log, "Performing deferred shader analysis", mark) >= 1, "Shader not analysed again after its ShaderRegex changed");

		// The else if chain collapsed by the optimiser still runs its branch:
		HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\QAQR.Activity.v1.515151");
		require(mapping != nullptr, "Activity channel mapping");
		auto view = (DWORD*)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 32);
		require(view != nullptr, "Read activity channel");
		Sleep(300);
		present();
		float picked;
		memcpy(&picked, &view[6], 4);
		require(picked == 57, "Deep else if chain picked the wrong branch: " + std::to_string(picked));
		UnmapViewOfFile(view);
		CloseHandle(mapping);

		check(device->GetDeviceRemovedReason(), "Device status");
		std::cout << "PASS reload_" << (on ? "on" : "off") << " kept=" << CountLines(log, "Kept the texture loaded from unchanged") + CountLines(log, "Kept the buffer loaded from unchanged")
			<< " analyses=" << CountLines(log, "Performing deferred shader analysis") << " software_backend=WARP\n";
		DestroyWindow(window);
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "FAIL " << e.what() << "\n";
		if (device) std::cerr << "Device reason: 0x" << std::hex << (unsigned)device->GetDeviceRemovedReason() << "\n";
		return 1;
	}
}
