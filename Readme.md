# DllProxyGenerator (C++20)

Automated tool to generate Proxy DLL source code (CPP, DEF, and x64 ASM) from an existing DLL.

## Usage

1. Run the generator with the path to your target DLL:
   ```bash
   DllProxyGenerator.exe <path_to_dll>
   ```
2. The tool will output `xxx.cpp`, `xxx.def`, `xxx.asm` (for x64), and a `CMakeLists.txt`.

## Build Generated Proxy

Use CMake to build the generated project. **Architecture must match the original DLL**:

### For 64-bit (x64) DLLs:
```bash
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

### For 32-bit (x86) DLLs:
```bash
mkdir build && cd build
cmake -A Win32 ..
cmake --build . --config Release
```

The generated CMake project automatically handles MASM for x64, static CRT linking, and PDB generation. It also includes architecture safeguards to ensure correct toolchain usage.
