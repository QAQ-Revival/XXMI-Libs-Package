// Offline probe for share_duplicate_resources: [Resource] sections whose files
// are identical share one GPU resource, unless something writes into them or
// they are created differently. Runs the real DLL on a WARP device.
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
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) { std::ostringstream s; s << what << " HRESULT=0x" << std::hex << (unsigned)hr; throw std::runtime_error(s.str()); }
}
void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }
// 4x4 32 bpp BMP, decoded through the WIC loader like the PNGs mods ship:
void writeBmp(const fs::path& path, unsigned seed) {
    fs::create_directories(path.parent_path());
    std::vector<unsigned char> pixels(4*4*4);
    for (unsigned i=0;i<pixels.size();i++) pixels[i]=(unsigned char)(seed*37+i*11);
    BITMAPFILEHEADER file={};BITMAPINFOHEADER info={};
    info.biSize=sizeof(info);info.biWidth=4;info.biHeight=4;info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+(DWORD)pixels.size();
    std::ofstream f(path,std::ios::binary);
    f.write((char*)&file,sizeof(file));f.write((char*)&info,sizeof(info));f.write((char*)pixels.data(),pixels.size());
}
void writeBuffer(const fs::path& path, unsigned seed) {
    std::vector<unsigned> data(64);
    for (unsigned i=0;i<data.size();i++) data[i]=seed*0x1000+i;
    std::ofstream f(path,std::ios::binary);f.write((char*)data.data(),data.size()*4);
}
unsigned countLines(const fs::path& log, const std::string& needle) {
    std::ifstream f(log);std::string line;unsigned n=0;
    while (std::getline(f,line)) if (line.find(needle)!=std::string::npos) n++;
    return n;
}
// Slot of each section in the command list below:
enum Slot { A, B, DIFF, WRITTEN, RT1, RT2, SRGB, BUF1, BUF2, BUFSTRUCT, SLOTS };
const char* slotNames[] = {"ModA","ModB","Different","Written","RT1","RT2","sRGB","Buffer1","Buffer2","Structured"};
int wmain(int argc, wchar_t** argv) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    try {
        require(argc>=3,"Usage: share.exe DLL on|off");
        const std::wstring mode=argv[2];
        require(mode==L"on" || mode==L"off","Unknown test mode");
        const bool on=mode==L"on";
        const auto work=fs::current_path();
        for(auto ancestor=work;;ancestor=ancestor.parent_path()){
            require(lstrcmpiW(ancestor.filename().c_str(),L"Mods")!=0 &&
                    lstrcmpiW(ancestor.filename().c_str(),L"Core")!=0 &&
                    lstrcmpiW(ancestor.filename().c_str(),L"ShaderFixes")!=0 &&
                    lstrcmpiW(ancestor.filename().c_str(),L"ShaderCache")!=0 &&
                    !fs::exists(ancestor/"d3dx.ini"),
                    "Test outputs must be outside live importer and Mod directories");
            if(ancestor==ancestor.parent_path())break;
        }
        fs::path dir=work/((on?std::string("share-test-on-"):std::string("share-test-off-"))+std::to_string(GetCurrentProcessId()));
        fs::create_directories(dir);
        fs::copy_file(fs::absolute(argv[1]),dir/"d3d11.dll",fs::copy_options::overwrite_existing);
        // Two mods shipping the same wardrobe texture in their own folders:
        writeBmp(dir/"ModA"/"tex.bmp",1);
        writeBmp(dir/"ModB"/"tex.bmp",1);
        writeBmp(dir/"other"/"different.bmp",2);
        for (const char* name : {"written","rt1","rt2","srgb"}) writeBmp(dir/"other"/(std::string(name)+".bmp"),1);
        writeBuffer(dir/"buf1.buf",3);writeBuffer(dir/"buf2.buf",3);writeBuffer(dir/"bufstruct.buf",3);
        { std::ofstream f(dir/"ModA"/"mod.ini"); f<<"namespace = ModA\n[ResourceTex]\nfilename = tex.bmp\n"; }
        { std::ofstream f(dir/"ModB"/"mod.ini"); f<<"namespace = ModB\n[ResourceTex]\nfilename = tex.bmp\n"; }
        const char* hlsl="[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {}";
        ComPtr<ID3DBlob> bytecode,errors;
        check(D3DCompile(hlsl,std::strlen(hlsl),"ShareTrigger",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&bytecode,&errors),"Compile trigger");
        auto bytes=(const unsigned char*)bytecode->GetBufferPointer();
        unsigned long long hash=0;
        for(int i=4;i<12;i++)hash=(hash<<8)|bytes[i];
        std::ofstream ini(dir/"d3dx.ini");
        ini << "[System]\ncheck_foreground_window = 0\nallow_check_interface = 1\nallow_create_device = 1\nallow_platform_update = 1\n"
               // Read the mods' includes on the first config load already:
               "skip_early_includes_load = 0\n"
               "[Hunting]\nhunting = 0\n[Logging]\nlog_level = debug\nunbuffered = 1\nshow_warnings = 0\n"
               "[Rendering]\nshader_hash = embedded\ncache_shaders = 0\nshare_duplicate_resources = " << (on?1:0) << "\n"
               "[Include]\ninclude = ModA\\mod.ini\ninclude = ModB\\mod.ini\n"
               "[Constants]\nglobal $never = 0\n"
               "[ResourceDifferent]\nfilename = other\\different.bmp\n"
               "[ResourceWritten]\nfilename = other\\written.bmp\n"
               "[ResourceRT1]\nfilename = other\\rt1.bmp\nbind_flags = shader_resource render_target\n"
               "[ResourceRT2]\nfilename = other\\rt2.bmp\nbind_flags = shader_resource render_target\n"
               "[ResourceSRGB]\nfilename = other\\srgb.bmp\ncolor_space = sRGB\n"
               "[ResourceBuffer1]\ntype = Buffer\nformat = R32_UINT\nfilename = buf1.buf\n"
               "[ResourceBuffer2]\ntype = Buffer\nformat = R32_UINT\nfilename = buf2.buf\n"
               "[ResourceStructured]\ntype = StructuredBuffer\nstride = 16\nfilename = bufstruct.buf\n"
               "[ShaderOverrideTrigger]\nhash = " << std::hex << std::setfill('0') << std::setw(16) << hash << std::dec << "\n"
               "run = CommandListTest\n[CommandListTest]\n"
               "cs-t0 = ref Resource\\ModA\\Tex\n"
               "cs-t1 = ref Resource\\ModB\\Tex\n"
               "cs-t2 = ref ResourceDifferent\n"
               "cs-t3 = ref ResourceWritten\n"
               "cs-t4 = ref ResourceRT1\n"
               "cs-t5 = ref ResourceRT2\n"
               "cs-t6 = ref ResourceSRGB\n"
               "cs-t7 = ref ResourceBuffer1\n"
               "cs-t8 = ref ResourceBuffer2\n"
               "cs-t9 = ref ResourceStructured\n"
               // Never runs, but a section a command list may write into
               // must keep a copy of its own:
               "if $never == 1\nResourceWritten = copy ResourceDifferent\nendif\n";
        ini.close();
        HMODULE dll=LoadLibraryW((dir/L"d3d11.dll").c_str());require(dll!=nullptr,"Load XXMI DLL");
        auto create=(decltype(&D3D11CreateDevice))GetProcAddress(dll,"D3D11CreateDevice");require(create!=nullptr,"Find create export");
        D3D_FEATURE_LEVEL level;
        check(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context),"Create WARP through XXMI");
        ComPtr<ID3D11ComputeShader> shader;
        check(device->CreateComputeShader(bytes,bytecode->GetBufferSize(),nullptr,&shader),"Create trigger shader");
        context->CSSetShader(shader.Get(),nullptr,0);
        ComPtr<ID3D11Resource> first[SLOTS];
        for(unsigned frame=0;frame<2;frame++){
            ID3D11ShaderResourceView* none[SLOTS]={};
            context->CSSetShaderResources(0,SLOTS,none);
            context->Dispatch(1,1,1);
            ComPtr<ID3D11Resource> res[SLOTS];
            for(unsigned i=0;i<SLOTS;i++){
                ComPtr<ID3D11ShaderResourceView> view;
                context->CSGetShaderResources(i,1,&view);
                require(view!=nullptr,std::string("SRV missing for ")+slotNames[i]+": INI command did not execute or the file did not load");
                view->GetResource(&res[i]);
                if(frame) require(res[i]==first[i],std::string("Resource changed between frames: ")+slotNames[i]);
                else first[i]=res[i];
            }
            // Identical files loaded the same way share only when enabled:
            require((res[A]==res[B])==on,on?"Identical textures of two mods were not shared":"Textures were shared while disabled");
            require((res[BUF1]==res[BUF2])==on,on?"Identical buffers were not shared":"Buffers were shared while disabled");
            // Never shared: different contents, written by a command list,
            // render targets, other load flags, other buffer layout:
            require(res[DIFF]!=res[A],"Different contents shared");
            require(res[WRITTEN]!=res[A],"Written resource shared");
            require(res[RT1]!=res[RT2] && res[RT1]!=res[A],"Render target shared");
            require(res[SRGB]!=res[A],"sRGB load shared with default load");
            require(res[BUFSTRUCT]!=res[BUF1],"Structured buffer shared with typed buffer");
            D3D11_TEXTURE2D_DESC desc;ComPtr<ID3D11Texture2D> tex;
            check(res[B].As(&tex),"Shared texture type");tex->GetDesc(&desc);
            require(desc.Width==4 && desc.Height==4 && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE),"Shared texture description");
            std::cout<<"PASS share_"<<(on?"on":"off")<<" frame="<<frame<<"\n";
        }
        check(device->GetDeviceRemovedReason(),"Device status");
        const fs::path log=dir/"d3d11_log.txt";
        const unsigned loaded=countLines(log,"Loading custom resource");
        const unsigned sharedTextures=countLines(log,"Sharing the texture already loaded");
        const unsigned sharedBuffers=countLines(log,"Sharing the buffer already loaded");
        // Seven texture sections; with sharing on, ModB's comes from ModA's:
        if(loaded!=(on?6u:7u) || sharedTextures!=(on?1u:0u) || sharedBuffers!=(on?1u:0u)){
            std::ostringstream s;s<<"Log counts loaded="<<loaded<<" shared_textures="<<sharedTextures<<" shared_buffers="<<sharedBuffers;
            throw std::runtime_error(s.str());
        }
        std::cout<<"RESULT {\"pass\":true,\"mode\":\""<<(on?"on":"off")<<"\",\"loading_lines\":"<<loaded
                 <<",\"sharing_texture_lines\":"<<sharedTextures<<",\"sharing_buffer_lines\":"<<sharedBuffers
                 <<",\"software_backend\":\"WARP\",\"actual_dll_loaded\":true}\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"FAIL "<<e.what()<<"\n";
        if(device)std::cerr<<"Device reason: 0x"<<std::hex<<(unsigned)device->GetDeviceRemovedReason()<<"\n";
        return 1;
    }
}
