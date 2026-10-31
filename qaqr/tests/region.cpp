#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <stdexcept>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) { std::ostringstream s; s << what << " HRESULT=0x" << std::hex << (unsigned)hr; throw std::runtime_error(s.str()); }
}
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
struct Case { const char* name; unsigned offset, size; const char* commands; };
const Case cases[] = {
{"observed",24512,20656,"ResourceDst = copy ResourceSource->Region(24512, 20656)\ncs-t0 = ref ResourceDst\n"},
{"zero_offset",0,256,"ResourceDst = copy ResourceSource->Region(0, 256)\ncs-t0 = ref ResourceDst\n"},
{"source_end_clamp",353792,256,"ResourceDst = copy ResourceSource->Region(353792, 1024)\ncs-t0 = ref ResourceDst\n"},
{"destination_limit",24512,64,"ResourceSmall = copy ResourceSource->Region(24512, 256)\ncs-t0 = ref ResourceSmall\n"},
{"empty_keeps_previous",24512,256,"ResourceKeep = copy ResourceSource->Region(24512, 256)\nResourceKeep = copy ResourceSource->Region(24512, 0)\ncs-t0 = ref ResourceKeep\n"},
{"out_of_bounds_keeps_previous",24512,256,"ResourceKeep = copy ResourceSource->Region(24512, 256)\nResourceKeep = copy ResourceSource->Region(354064, 256)\ncs-t0 = ref ResourceKeep\n"},
{"unaligned_keeps_previous",24512,256,"ResourceKeep = copy ResourceSource->Region(24512, 256)\nResourceKeep = copy ResourceSource->Region(24513, 256)\ncs-t0 = ref ResourceKeep\n"},
// SRV REF retains the pre-existing end-offset convention; this patch only changes COPY.
{"ref_srv",24512,256,"cs-t0 = ref ResourceSource->Region(24512, 24768)\n"},
{"copy_from_cb_slot",256,256,"cs-cb2 = ref ResourceCB\nResourceDst = copy cs-cb2->Region(256, 256)\ncs-t0 = ref ResourceDst\n"},
{"ref_cb",256,256,"cs-cb2 = ref ResourceCB->Region(256, 256)\n"},
{"efmi_fx_chain",24512,20656,"ResourceDst = copy ResourceSource->Region(24512, 20656)\ncs-cb0 = ref ResourceCB\ncs-u0 = ref ResourceDst\nrun = CustomShaderFXConfig\ncs-u0 = null\nvs-cb3 = copy ResourceDst\n"},
{"copy_cb_reuse",0,65536,"vs-cb3 = copy ResourceCB\n"},
{"creation_failure_keeps_previous",24512,256,"ResourceKeep = copy ResourceSource->Region(24512, 256)\nResourceKeep = copy ResourceRaw\nResourceKeep = copy ResourceRaw\ncs-t0 = ref ResourceKeep\n"},
// ShapeKey output: a structured compute UAV copied into an existing plain VB.
{"structured_to_vertex",0,354048,"vb0 = ref ResourceVertex\ncs-u5 = copy ResourceSource\nResourceVertex = copy cs-u5\ncs-u5 = null\nvb0 = ref ResourceVertex\n"},
{"structured_to_index",0,354048,"ib = ref ResourceIndex\nResourceIndex = copy ResourceSource\nib = ref ResourceIndex\n"}
};
int wmain(int argc, wchar_t** argv) {
    bool baseline = false;
    unsigned selected = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    try {
        require(argc>=3,"Usage: native_region_test.exe DLL baseline|patched|reference [index] [FX-HLSL]");
        const std::wstring mode=argv[2];
        require(mode==L"baseline" || mode==L"patched" || mode==L"reference","Unknown test mode");
        baseline=mode==L"baseline";
        selected=argc>3 ? std::stoul(argv[3]) : 0;
        require(!baseline || selected==0,"Baseline mode is only valid for the observed failing case");
        const auto work=fs::current_path();
        require(!fs::exists(work/"SKILL.md"),"Use a separate test work directory");
        for(auto ancestor=work;;ancestor=ancestor.parent_path()){
            require(lstrcmpiW(ancestor.filename().c_str(),L"Mods")!=0 &&
                    lstrcmpiW(ancestor.filename().c_str(),L"Core")!=0 &&
                    lstrcmpiW(ancestor.filename().c_str(),L"ShaderFixes")!=0 &&
                    lstrcmpiW(ancestor.filename().c_str(),L"ShaderCache")!=0 &&
                    !fs::exists(ancestor/"d3dx.ini"),
                    "Test outputs must be outside live importer and Mod directories");
            if(ancestor==ancestor.parent_path())break;
        }
        fs::path sourceDll=fs::absolute(argv[1]);
        require(selected < std::size(cases), "Invalid test index");
        fs::path dir=work/((baseline?std::string("runtime-test-baseline-"):std::string("runtime-test-patched-"))+cases[selected].name+"-"+std::to_string(GetCurrentProcessId()));
        fs::create_directories(dir);
        fs::copy_file(sourceDll,dir/"d3d11.dll",fs::copy_options::overwrite_existing);
        std::vector<unsigned> data(354048/4);
        for(unsigned i=0;i<data.size();i++)data[i]=0x12300000+i;
        { std::ofstream f(dir/"source.buf",std::ios::binary);f.write((char*)data.data(),data.size()*4); }
        { std::ofstream f(dir/"cb.buf",std::ios::binary);f.write((char*)data.data(),65536); }
        const char* hlsl="[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {}";
        ComPtr<ID3DBlob> bytecode,errors;
        check(D3DCompile(hlsl,std::strlen(hlsl),"RegionCopyTrigger",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&bytecode,&errors),"Compile trigger");
        auto bytes=(const unsigned char*)bytecode->GetBufferPointer();
        unsigned long long hash=0;
        for(int i=4;i<12;i++)hash=(hash<<8)|bytes[i];
        std::ofstream ini(dir/"d3dx.ini");
        ini << R"INI([System]
check_foreground_window = 0
allow_check_interface = 1
allow_create_device = 1
allow_platform_update = 1
[Hunting]
hunting = 0
[Logging]
log_level = debug
unbuffered = 1
show_warnings = 0
[Rendering]
shader_hash = embedded
cache_shaders = 0
[ResourceSource]
type = RWStructuredBuffer
stride = 16
bind_flags = shader_resource unordered_access
filename = source.buf
[ResourceCB]
type = Buffer
bind_flags = constant_buffer
filename = cb.buf
[ResourceDst]
type = RWStructuredBuffer
stride = 16
format = UNKNOWN
[ResourceKeep]
type = RWStructuredBuffer
stride = 16
format = UNKNOWN
[ResourceRaw]
type = RWByteAddressBuffer
bind_flags = shader_resource unordered_access
filename = cb.buf
[ResourceVertex]
type = Buffer
stride = 16
filename = source.buf
[ResourceIndex]
type = Buffer
format = R32_UINT
filename = source.buf
[ResourceSmall]
type = RWStructuredBuffer
stride = 16
format = UNKNOWN
byte_width = 64
[ShaderOverrideTrigger]
hash = )INI" << std::hex << std::setfill('0') << std::setw(16) << hash << std::dec;
        ini << "\nrun = CommandListTest\n[CommandListTest]\n";
        ini<<"cs-t7 = ref ResourceSource\ncs-cb7 = ref ResourceCB\n";
        ini<<cases[selected].commands;
        if(std::string(cases[selected].name)=="efmi_fx_chain"){
            require(argc>4,"FX-chain test requires an explicit candidate HLSL path");
            fs::path hlslPath=fs::absolute(argv[4]);
            fs::copy_file(hlslPath,dir/"FXConfig.hlsl",fs::copy_options::overwrite_existing);
            ini<<"[CustomShaderFXConfig]\ncs = FXConfig.hlsl\nx0 = 0\ny0 = 0\nz0 = 0\nw0 = 0\ndispatch = 1, 1, 1\ncs-u0 = null\n";
        }
        ini.close();
        HMODULE dll=LoadLibraryW((dir/L"d3d11.dll").c_str());require(dll!=nullptr,"Load XXMI DLL");
        auto create=(decltype(&D3D11CreateDevice))GetProcAddress(dll,"D3D11CreateDevice");require(create!=nullptr,"Find create export");
        D3D_FEATURE_LEVEL level;
        check(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context),"Create WARP through XXMI");
        ComPtr<ID3D11ComputeShader> shader;
        check(device->CreateComputeShader(bytes,bytecode->GetBufferSize(),nullptr,&shader),"Create trigger shader");
        context->CSSetShader(shader.Get(),nullptr,0);
        unsigned passed=0;
        for(unsigned frame=0;frame<2;frame++){
            const auto& test=cases[selected];
            if(frame){
                // Reuse the same allocations with new contents, as successive
                // character draws do. Clear the output so a skipped copy/bind
                // cannot pass by leaving the previous draw's result in place.
                for(auto& value:data)value+=0x100000;
                ComPtr<ID3D11ShaderResourceView> sourceView;
                context->CSGetShaderResources(7,1,&sourceView);
                require(sourceView!=nullptr,"Source SRV missing");
                ComPtr<ID3D11Resource> source;sourceView->GetResource(&source);
                context->UpdateSubresource(source.Get(),0,nullptr,data.data(),0,0);
                ComPtr<ID3D11Buffer> sourceCB;
                context->CSGetConstantBuffers(7,1,&sourceCB);
                require(sourceCB!=nullptr,"Source CB missing");
                context->UpdateSubresource(sourceCB.Get(),0,nullptr,data.data(),0,0);
                ID3D11ShaderResourceView* noView=nullptr;
                context->CSSetShaderResources(0,1,&noView);
                ID3D11Buffer* noBuffer=nullptr;
                context->VSSetConstantBuffers(3,1,&noBuffer);
                context->CSSetConstantBuffers(2,1,&noBuffer);
            }
            context->Dispatch(1,1,1);
            if(std::string(test.name)=="ref_cb"){
                ComPtr<ID3D11DeviceContext1> context1;check(context.As(&context1),"Query context1");
                ComPtr<ID3D11Buffer> cb;UINT first=0,count=0;
                context1->CSGetConstantBuffers1(2,1,&cb,&first,&count);
                require(cb && first==16 && count==16,"REF CB range changed");
            } else {
                ComPtr<ID3D11Buffer> buffer;
                ComPtr<ID3D11ShaderResourceView> view;
                const bool outputCB=std::string(test.name)=="efmi_fx_chain" || std::string(test.name)=="copy_cb_reuse";
                const bool outputVertex=std::string(test.name)=="structured_to_vertex";
                const bool outputIndex=std::string(test.name)=="structured_to_index";
                if(outputVertex){
                    UINT stride=0,offset=0;
                    context->IAGetVertexBuffers(0,1,&buffer,&stride,&offset);
                    require(buffer && stride==16 && offset==0,"ShapeKey output VB or stride missing");
                }else if(outputIndex){
                    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;UINT offset=0;
                    context->IAGetIndexBuffer(&buffer,&format,&offset);
                    require(buffer && format==DXGI_FORMAT_R32_UINT && offset==0,"Output IB or format missing");
                }else if(outputCB){
                    context->VSGetConstantBuffers(3,1,&buffer);
                    require(buffer!=nullptr,"FX output constant buffer missing");
                }else{
                    context->CSGetShaderResources(0,1,&view);
                    require(view!=nullptr,"Result SRV missing: INI command did not execute");
                    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
                    check(resource.As(&buffer),"Get result buffer");
                }
                D3D11_BUFFER_DESC desc;buffer->GetDesc(&desc);
                if(outputCB)
                    require((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && !(desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED),"FX output buffer type mismatch");
                if(outputVertex || outputIndex)
                    require(!(desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && desc.StructureByteStride==0,"Input-assembler buffer inherited structured metadata");
                if(std::string(test.name)=="ref_srv"){
                    D3D11_SHADER_RESOURCE_VIEW_DESC v;view->GetDesc(&v);
                    require(desc.ByteWidth==354048 && v.Buffer.FirstElement*16==test.offset && v.Buffer.NumElements*16==test.size,"REF SRV range changed");
                }else{
                    if(!baseline && desc.ByteWidth!=test.size){std::ostringstream s;s<<"Destination size "<<desc.ByteWidth<<", expected "<<test.size;throw std::runtime_error(s.str());}
                    D3D11_BUFFER_DESC rd={};rd.ByteWidth=desc.ByteWidth;rd.Usage=D3D11_USAGE_STAGING;rd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Buffer> readback;check(device->CreateBuffer(&rd,nullptr,&readback),"Create readback");
                    context->CopyResource(readback.Get(),buffer.Get());
                    D3D11_MAPPED_SUBRESOURCE mapped;check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Read result");
                    bool equal;
                    if(std::string(test.name)=="efmi_fx_chain"){
                        equal=std::memcmp(mapped.pData,data.data(),64)==0 &&
                            std::memcmp((char*)mapped.pData+64,(char*)data.data()+test.offset+64,test.size-64)==0;
                    }else equal=std::memcmp(mapped.pData,(char*)data.data()+test.offset,test.size)==0;
                    context->Unmap(readback.Get(),0);require(equal,"Copied data differs");
                }
            }
            passed++;std::cout<<"PASS "<<test.name<<" frame="<<frame<<"\n";
        }
        check(device->GetDeviceRemovedReason(),"Device status");
        std::cout<<"RESULT {\"pass\":true,\"cases\":"<<passed<<",\"software_backend\":\"WARP\",\"actual_dll_loaded\":true}\n";
        return baseline?2:0;
    }catch(const std::exception& e){
        std::cerr<<"FAIL "<<e.what()<<"\n";
        if(device)std::cerr<<"Device reason: 0x"<<std::hex<<(unsigned)device->GetDeviceRemovedReason()<<"\n";
        if(baseline && device && device->GetDeviceRemovedReason()==DXGI_ERROR_DEVICE_REMOVED){std::cout<<"RESULT {\"baseline_failure_reproduced\":true}\n";return 0;}
        return 1;
    }
}
