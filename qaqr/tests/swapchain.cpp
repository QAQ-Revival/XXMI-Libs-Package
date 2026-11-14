// Offline probe for swap chains that are not the game's: a DirectComposition
// panel made on its own thread, as EFF's Ins panel does, must come back
// unwrapped so DirectComposition can take it, and neither it nor its size may
// become the resolution Mods read. Runs the real DLL on a WARP device.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
static void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) { std::ostringstream s; s << what << " HRESULT=0x" << std::hex << (unsigned)hr; throw std::runtime_error(s.str()); }
}
static void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }
static HMODULE moduleOf(const void* at) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &module);
    return module;
}
// Whether the object's methods are the DLL's, which is what a wrapper is
static bool wrappedBy(IUnknown* object, HMODULE dll) { return moduleOf(*(void**)object) == dll; }
static HWND makeWindow(int width, int height) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"QaqrSwapChainProbe";
        RegisterClassW(&wc); registered = true;
    }
    RECT r = {0, 0, width, height}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, L"QaqrSwapChainProbe", L"probe", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}
// SetContent is where a wrapped swap chain faults
static DWORD setContent(IDCompositionVisual* visual, IDXGISwapChain1* chain, HRESULT* hr) {
    __try { *hr = visual->SetContent(chain); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
static std::string lastResolution(const fs::path& log) {
    std::ifstream f(log); std::string line, last;
    const std::string needle = "Got resolution from swap chain: ";
    while (std::getline(f, line)) { auto at = line.find(needle); if (at != std::string::npos) last = line.substr(at + needle.size()); }
    while (!last.empty() && (last.back() == '\r' || last.back() == ' ')) last.pop_back();
    return last;
}
static bool logHas(const fs::path& log, const std::string& needle) {
    std::ifstream f(log); std::string line;
    while (std::getline(f, line)) if (line.find(needle) != std::string::npos) return true;
    return false;
}
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc >= 2, "Usage: swapchain.exe DLL [nocomposition]");
        // Without DirectComposition, to see what the panel does to the resolution on a DLL that faults there
        const bool noComposition = argc >= 3 && std::wstring(argv[2]) == L"nocomposition";
        const auto work = fs::current_path();
        for (auto ancestor = work;; ancestor = ancestor.parent_path()) {
            require(lstrcmpiW(ancestor.filename().c_str(), L"Mods") != 0 && lstrcmpiW(ancestor.filename().c_str(), L"Core") != 0 &&
                    lstrcmpiW(ancestor.filename().c_str(), L"ShaderFixes") != 0 && lstrcmpiW(ancestor.filename().c_str(), L"ShaderCache") != 0 &&
                    !fs::exists(ancestor / "d3dx.ini"), "Test outputs must be outside live importer and Mod directories");
            if (ancestor == ancestor.parent_path()) break;
        }
        fs::path dir = work / ("swapchain-test-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(dir);
        fs::copy_file(fs::absolute(argv[1]), dir / "d3d11.dll", fs::copy_options::overwrite_existing);
        {
            std::ofstream ini(dir / "d3dx.ini");
            ini << "[System]\ncheck_foreground_window = 0\nallow_check_interface = 1\nallow_create_device = 1\nallow_platform_update = 1\n"
                   "[Device]\nget_resolution_from = swap_chain\n"
                   "[Hunting]\nhunting = 0\n[Logging]\nlog_level = debug\nunbuffered = 1\nshow_warnings = 0\n"
                   "[Rendering]\ncache_shaders = 0\n";
        }
        HMODULE dll = LoadLibraryW((dir / L"d3d11.dll").c_str()); require(dll != nullptr, "Load XXMI DLL");
        auto create = (decltype(&D3D11CreateDevice))GetProcAddress(dll, "D3D11CreateDevice"); require(create != nullptr, "Find create export");
        const fs::path log = dir / "d3d11_log.txt";

        // The game: a device, a factory through the hooked export, a window and its swap chain
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL level;
        check(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context), "Create game device");
        ComPtr<IDXGIFactory2> factory; check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Create factory");
        require(logHas(log, "Successfully installed IDXGIFactory2->CreateSwapChainForComposition hook"), "Composition hook was not installed");
        HWND game = makeWindow(800, 600); require(game != nullptr, "Game window");
        DXGI_SWAP_CHAIN_DESC1 desc = {}; desc.Width = 800; desc.Height = 600; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> gameChain; check(factory->CreateSwapChainForHwnd(device.Get(), game, &desc, nullptr, nullptr, &gameChain), "Game swap chain");
        require(wrappedBy(gameChain.Get(), dll), "The game's swap chain was not wrapped");
        check(gameChain->Present(0, 0), "Game present");
        require(lastResolution(log) == "800x600", "Resolution after the game's swap chain: " + lastResolution(log));

        // The panel, on a thread of its own with a device of its own
        std::string failure; DWORD fault = 0; bool panelWrapped = true;
        std::thread panel([&] {
            try {
                ComPtr<ID3D11Device> panelDevice; ComPtr<ID3D11DeviceContext> panelContext; D3D_FEATURE_LEVEL panelLevel;
                check(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &panelDevice, &panelLevel, &panelContext), "Create panel device");
                ComPtr<IDXGIDevice> dxgiDevice; check(panelDevice.As(&dxgiDevice), "Panel IDXGIDevice");
                ComPtr<IDXGIAdapter> adapter; check(dxgiDevice->GetAdapter(&adapter), "Panel adapter");
                ComPtr<IDXGIFactory2> panelFactory; check(adapter->GetParent(IID_PPV_ARGS(&panelFactory)), "Panel factory");
                DXGI_SWAP_CHAIN_DESC1 pd = {}; pd.Width = 604; pd.Height = 640; pd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; pd.SampleDesc.Count = 1;
                pd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; pd.BufferCount = 2; pd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                pd.Scaling = DXGI_SCALING_STRETCH; pd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
                ComPtr<IDXGISwapChain1> chain; check(panelFactory->CreateSwapChainForComposition(panelDevice.Get(), &pd, nullptr, &chain), "Composition swap chain");
                panelWrapped = wrappedBy(chain.Get(), dll);
                HWND host = makeWindow(604, 640); require(host != nullptr, "Panel window");
                ComPtr<IDCompositionDevice> dcomp; ComPtr<IDCompositionTarget> target;
                HRESULT made = noComposition ? S_FALSE : DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp));
                if (made == S_OK) made = dcomp->CreateTargetForHwnd(host, TRUE, &target);
                // A session without a desktop compositor, as on some build machines
                if (FAILED(made)) std::cout << "SKIP DirectComposition is not available here: 0x" << std::hex << (unsigned)made << std::dec << "\n";
                if (made == S_OK) {
                    ComPtr<IDCompositionVisual> visual; check(dcomp->CreateVisual(&visual), "DComp visual");
                    HRESULT hr = S_OK;
                    fault = setContent(visual.Get(), chain.Get(), &hr);
                    if (fault) return;
                    check(hr, "SetContent");
                    check(target->SetRoot(visual.Get()), "SetRoot");
                    check(dcomp->Commit(), "Commit");
                }
                ComPtr<ID3D11Texture2D> buffer; check(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Panel buffer");
                ComPtr<ID3D11RenderTargetView> rtv; check(panelDevice->CreateRenderTargetView(buffer.Get(), nullptr, &rtv), "Panel RTV");
                const float clear[4] = {0, 0, 0, 0.5f}; panelContext->ClearRenderTargetView(rtv.Get(), clear);
                for (int i = 0; i < 3; i++) check(chain->Present(0, 0), "Panel present");
                rtv.Reset(); buffer.Reset();
                check(chain->ResizeBuffers(0, 640, 700, DXGI_FORMAT_UNKNOWN, 0), "Panel resize");
                check(chain->Present(0, 0), "Panel present after resize");
                DestroyWindow(host);
            } catch (const std::exception& e) { failure = e.what(); }
        });
        panel.join();
        if (fault) { std::ostringstream s; s << "IDCompositionVisual::SetContent faulted 0x" << std::hex << fault << (panelWrapped ? " on a wrapped swap chain" : ""); throw std::runtime_error(s.str()); }
        require(failure.empty(), failure);
        require(lastResolution(log) == "800x600", "The panel changed the resolution to " + lastResolution(log));
        require(!panelWrapped, "The composition swap chain was wrapped");

        // The game resizing still counts
        check(gameChain->ResizeBuffers(0, 1024, 768, DXGI_FORMAT_UNKNOWN, 0), "Game resize");
        check(gameChain->Present(0, 0), "Game present after resize");
        require(lastResolution(log) == "1024x768", "The game's resize did not set the resolution: " + lastResolution(log));
        check(device->GetDeviceRemovedReason(), "Device status");
        std::cout << "RESULT {\"pass\":true,\"composition_unwrapped\":true,\"set_content\":\"ok\",\"resolution\":\"" << lastResolution(log)
                  << "\",\"software_backend\":\"WARP\",\"actual_dll_loaded\":true}\n";
        DestroyWindow(game);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << "\n";
        return 1;
    }
}
