// hlsl2dxbc: compiles a small HLSL file with Windows' own compiler and writes the container, so the container
// checksum (which DXVK checks) can be tested against a real one.
//   hlsl2dxbc.exe in.hlsl out.dxbc entry profile
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char** argv) {
    if (argc < 5) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 3;
    static char src[1 << 16];
    size_t n = fread(src, 1, sizeof src, f);
    fclose(f);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = (decltype(&D3DCompile))GetProcAddress(m, "D3DCompile");
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(src, n, "x.hlsl", nullptr, nullptr, argv[3], argv[4], 0, 0, &code, &err);
    if (FAILED(hr)) { printf("fail 0x%lx %s\n", (unsigned long)hr, err ? (char*)err->GetBufferPointer() : ""); return 1; }
    f = fopen(argv[2], "wb");
    fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), f);
    fclose(f);
    printf("ok %zu\n", (size_t)code->GetBufferSize());
    return 0;
}
