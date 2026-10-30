#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <chrono>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void check(HRESULT hr, const char* message) { require(SUCCEEDED(hr), message); }
std::string read(const fs::path& file) { std::ifstream f(file, std::ios::binary); return { std::istreambuf_iterator<char>(f), {} }; }
int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 4, "Usage: verify DLL fixture-directory initial|restore");
    fs::path dll = fs::absolute(argv[1]), dir = fs::absolute(argv[2]);
    bool restore = std::wstring(argv[3]) == L"restore";
    const char* source = "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID){}";
    ComPtr<ID3DBlob> code, errors;
    check(D3DCompile(source, strlen(source), "AutosaveTest", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors), "Compile trigger");
    auto data = (unsigned char*)code->GetBufferPointer(); unsigned long long hash = 0;
    for (int i = 4; i < 12; i++) hash = (hash << 8) | data[i];
    if (!restore) {
      require(!fs::exists(dir), "Use a fresh fixture directory outside the game");
      fs::create_directories(dir); fs::copy_file(dll, dir / "d3d11.dll");
      std::ofstream ini(dir / "d3dx.ini"), user(dir / "d3dx_user.ini");
      ini << "namespace = QAQM\\Persist\\Benchmark\n[System]\ncheck_foreground_window = 0\nallow_check_interface = 1\nallow_create_device = 1\nallow_platform_update = 1\nsettings_auto_save_interval = 0\nclear_unknown_settings = 0\n[Hunting]\nhunting = 0\n[Logging]\nlog_level = warning\nshow_warnings = 0\n[Rendering]\nshader_hash = embedded\ncache_shaders = 0\n[Include]\ninclude = activity.ini\n[Constants]\nglobal persist $value = 0\nglobal $phase = 0\n";
      user << "; Baseline\n[Constants]\n$\\qaqm\\persist\\benchmark\\value = 0\n$\\removed\\setting = 55\n";
      for (int i = 0; i < 150000; i++) {
        ini << "global persist $v" << i << " = 0\n";
        user << "$\\qaqm\\persist\\benchmark\\v" << i << " = 0\n";
      }
      ini << "[Present]\n$\\QAQM\\Activity\\m0 = 1\n$\\QAQM\\Activity\\m1 = $value\n[ShaderOverrideTrigger]\nhash = " << std::hex << std::setfill('0') << std::setw(16) << hash << std::dec;
      ini << "\nrun = CommandListChange\n[CommandListChange]\nif $phase == 0\n$value = 7\nelse if $phase == 1\n$value = 8\n$value = 7\nelse if $phase == 2\n$value = 9\nendif\n$phase = $phase + 1\n";
      fs::create_directories(dir / "QAQM/Persist");
      std::ofstream activity(dir / "QAQM/Persist/activity.ini");
      activity << "namespace = QAQM\\Activity\n[Constants]\nglobal $token = 424241\nglobal $m0 = 0\nglobal $m1 = 0\n";
    }
    HMODULE module = LoadLibraryW((dir / "d3d11.dll").c_str()); require(module, "Load actual candidate DLL");
    auto create = (decltype(&D3D11CreateDeviceAndSwapChain))GetProcAddress(module, "D3D11CreateDeviceAndSwapChain"); require(create, "Create export");
    HWND window = CreateWindowExW(0, L"STATIC", L"QAQR isolated autosave verification", WS_OVERLAPPEDWINDOW, 0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(window, "Create hidden test window");
    DXGI_SWAP_CHAIN_DESC desc = {}; desc.BufferDesc.Width = desc.BufferDesc.Height = 32; desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 1; desc.OutputWindow = window; desc.Windowed = TRUE;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; ComPtr<IDXGISwapChain> swap; D3D_FEATURE_LEVEL level;
    check(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swap, &device, &level, &context), "Create WARP swapchain through candidate");
    auto frame = [&]() { Sleep(20); check(swap->Present(0, 0), "Present"); };
    for (int i = 0; i < 5; i++) frame();
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\QAQR.Activity.v1.424241"); require(mapping, "Native activity exporter created mapping");
    auto view = (DWORD*)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 32); require(view, "Read mapping");
    require(view[0] == 0x31524151 && view[1] == 1 && view[5] == 2, "Activity header");
    Sleep(280); frame();
    float value; memcpy(&value, &view[7], 4);
    if (restore) {
      require(value == 9, "Last incremental value restored by actual INI parser");
      auto before = read(dir / "d3dx_user.ini");
      require(before.find("; QAQR incremental update") == std::string::npos, "Startup reload compacts the journal");
      require(before.find("$\\removed\\setting = 55") != std::string::npos, "Compaction retains unknown settings");
      for (int i = 0; i < 10; i++) frame();
      require(read(dir / "d3dx_user.ini") == before, "Restored idle state does not save");
      std::cout << "PASS restore latest value and idle after restart\n";
    } else {
      auto file = dir / "d3dx_user.ini";
      auto before = read(file);
      auto stamp = fs::last_write_time(file);
      float tick; memcpy(&tick, &view[4], 4);
      for (int i = 0; i < 20; i++) frame();
      float nextTick; memcpy(&nextTick, &view[4], 4);
      require(nextTick > tick, "Telemetry clock advances without disk saves");
      require(read(file) == before && fs::last_write_time(file) == stamp, "150000 unchanged variables never written");
      ComPtr<ID3D11ComputeShader> shader;
      check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader), "Create trigger");
      context->CSSetShader(shader.Get(), nullptr, 0);
      context->Dispatch(1, 1, 1);
      auto start = std::chrono::steady_clock::now(); check(swap->Present(0, 0), "Save frame");
      auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
      auto changed = read(file);
      require(changed.size() > before.size() && changed.size() - before.size() < 128 && changed.compare(0, before.size(), before) == 0, "One change appends a small delta and preserves all prior settings");
      require(changed.find("value = 7", before.size()) != std::string::npos, "Changed value written");
      context->Dispatch(1, 1, 1); frame();
      require(read(file) == changed, "Change then revert does not save");
      HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr); require(lock != INVALID_HANDLE_VALUE, "Lock for write failure test");
      context->Dispatch(1, 1, 1); frame();
      require(read(file) == changed, "Failed save preserves the file");
      CloseHandle(lock); frame();
      require(read(file).find("value = 9", changed.size()) != std::string::npos, "Failed save retries dirty values");
      require(read(file).find("$\\removed\\setting = 55") != std::string::npos, "Unknown values preserved");
      std::cout << "PASS unchanged=0 writes, delta_bytes=" << changed.size() - before.size() << ", baseline_bytes=" << before.size() << ", save_present_us=" << micros << ", reverted=0 writes, failure_retry=ok, memory_heartbeat=ok\n";
    }
    UnmapViewOfFile(view); CloseHandle(mapping); DestroyWindow(window);
    return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << "\n"; return 1; }
}
