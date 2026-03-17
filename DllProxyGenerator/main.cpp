#include <iostream>
#include <Windows.h>
#include <imagehlp.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_map>

#pragma comment(lib, "imagehlp.lib")



class ScopedHandle {
  HANDLE handle;

public:
  explicit ScopedHandle(HANDLE h) : handle(h) {}
  ~ScopedHandle() {
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr)
      CloseHandle(handle);
  }
  operator HANDLE() const { return handle; }
  bool IsValid() const { return handle != INVALID_HANDLE_VALUE && handle != nullptr; }
};

class ScopedView {
  void *ptr;

public:
  explicit ScopedView(void *p) : ptr(p) {}
  ~ScopedView() {
    if (ptr)
      UnmapViewOfFile(ptr);
  }
  operator void *() const { return ptr; }
  bool IsValid() const { return ptr != nullptr; }
};

class ScopedLoadedImage {
  HANDLE fileHandle;
  HANDLE mappingHandle;
  void *baseAddress;

public:
  explicit ScopedLoadedImage(const std::wstring &dllPath)
      : fileHandle(INVALID_HANDLE_VALUE), mappingHandle(nullptr), baseAddress(nullptr) {
    fileHandle = CreateFileW(dllPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fileHandle != INVALID_HANDLE_VALUE) {
      mappingHandle = CreateFileMapping(fileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
      if (mappingHandle) {
        baseAddress = MapViewOfFile(mappingHandle, FILE_MAP_READ, 0, 0, 0);
      }
    }
  }

  ~ScopedLoadedImage() {
    if (baseAddress)
      UnmapViewOfFile(baseAddress);

    if (mappingHandle)
      CloseHandle(mappingHandle);

    if (fileHandle != INVALID_HANDLE_VALUE)
      CloseHandle(fileHandle);
  }

  bool IsValid() const { return baseAddress != nullptr; }
  void *GetBase() const { return baseAddress; }
};

std::string LoadTemplate(const std::wstring &templateName) {
  std::ifstream file(L"templates/" + templateName);
  if (!file.is_open()) {
    std::wcerr << L"Failed to open template: " << templateName << std::endl;
    return "";
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}



std::string ReplaceTemplate(const std::string &templateStr,
                            const std::unordered_map<std::string, std::string> &replacements) {
  std::string result;
  result.reserve(templateStr.length());

  for (std::size_t i = 0; i < templateStr.length();) {
    if (i + 1 < templateStr.length() && templateStr[i] == '{' && templateStr[i + 1] == '{') {
      std::size_t endPos = templateStr.find("}}", i + 2);
      if (endPos != std::string::npos) {
        std::string key = templateStr.substr(i + 2, endPos - (i + 2));
        auto it = replacements.find(key);
        if (it != replacements.end()) {
          result += it->second;
          i = endPos + 2;
          continue;
        }
      }
    }
    result += templateStr[i];
    ++i;
  }
  return result;
}

std::string ToAnsiString(const std::wstring &wstr) {
  std::string result;
  result.reserve(wstr.length());
  for (wchar_t wc : wstr) {
    result += static_cast<char>(wc);
  }
  return result;
}

bool GetImageFileHeaders(const std::wstring &dllPath, IMAGE_NT_HEADERS &ntHeaders) {
  ScopedHandle fileHandle(CreateFileW(dllPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!fileHandle.IsValid())
    return false;

  ScopedHandle imageHandle(CreateFileMapping(fileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr));
  if (!imageHandle.IsValid())
    return false;

  ScopedView imagePtr(MapViewOfFile(imageHandle, FILE_MAP_READ, 0, 0, 0));
  if (!imagePtr.IsValid())
    return false;

  PIMAGE_NT_HEADERS ntHeaderPtr = ImageNtHeader(imagePtr);
  if (ntHeaderPtr == nullptr)
    return false;

  ntHeaders = *ntHeaderPtr;
  return true;
}

void ListDLLFunctions(const std::wstring &dllPath, std::vector<std::string> &exportedNames) {
  exportedNames.clear();

  ScopedLoadedImage loadedImage(dllPath);
  if (!loadedImage.IsValid()) {
    return;
  }

  unsigned long directorySize;
  PIMAGE_EXPORT_DIRECTORY exportDirectory = (PIMAGE_EXPORT_DIRECTORY)ImageDirectoryEntryToData(
      loadedImage.GetBase(), false, IMAGE_DIRECTORY_ENTRY_EXPORT, &directorySize);

  if (exportDirectory == nullptr) {
    return;
  }

  PIMAGE_NT_HEADERS ntHeaders = ImageNtHeader(loadedImage.GetBase());
  if (ntHeaders == nullptr) {
    return;
  }

  DWORD *nameRVAs =
      (DWORD *)ImageRvaToVa(ntHeaders, loadedImage.GetBase(), exportDirectory->AddressOfNames, nullptr);
  if (nameRVAs == nullptr) {
    return;
  }

  for (std::size_t i = 0; i < exportDirectory->NumberOfNames; i++) {
    const char *funcName = (const char *)ImageRvaToVa(ntHeaders, loadedImage.GetBase(), nameRVAs[i], nullptr);
    if (funcName) {
      exportedNames.push_back(funcName);
    }
  }
}

void GenerateDEF(const std::wstring &baseDllName, const std::vector<std::string> &exportedNames) {
  std::string content = LoadTemplate(L"export_def.template");
  if (content.empty())
    return;

  std::string exports;
  const std::string exportLineTemplate = R"(  {{FUNC_NAME}}=Fake{{FUNC_NAME}} @{{ORDINAL}}
)";

  for (std::size_t i = 0; i < exportedNames.size(); i++) {
    exports += ReplaceTemplate(exportLineTemplate,
                               {{"FUNC_NAME", exportedNames[i]}, {"ORDINAL", std::to_string(i + 1)}});
  }

  std::string baseDllAnsiName = ToAnsiString(baseDllName);
  content = ReplaceTemplate(content, {{"DLL_NAME", baseDllAnsiName}, {"EXPORTS", exports}});

  std::ofstream(baseDllName + L".def") << content;
}

void GenerateMainCPP(const std::wstring &baseDllName, const std::vector<std::string> &exportedNames, WORD fileType) {
  std::string content = LoadTemplate(L"main_cpp.template");
  if (content.empty())
    return;

  std::string baseDllAnsiName = ToAnsiString(baseDllName);
  std::string members, exports, calls;

  const std::string memberTemplate = R"(    FARPROC Orignal{{FUNC_NAME}};
)";

  const std::string exportTemplate =
      (fileType == IMAGE_FILE_MACHINE_AMD64)
          ? R"(extern "C" void Fake{{FUNC_NAME}}();
)"
          : R"(__declspec(naked) void Fake{{FUNC_NAME}}() { _asm { jmp[{{DLL_NAME}}.Orignal{{FUNC_NAME}}] } }
)";

  const std::string callTemplate =
      R"(            {{DLL_NAME}}.Orignal{{FUNC_NAME}} = GetProcAddress({{DLL_NAME}}.dllHandle, "{{FUNC_NAME}}");
)";

  for (const auto &funcName : exportedNames) {
    members += ReplaceTemplate(memberTemplate, {{"FUNC_NAME", funcName}});

    std::string exportCodeTemplate = exportTemplate;
    std::unordered_map<std::string, std::string> exportReplacements = {{"FUNC_NAME", funcName}};
    if (fileType != IMAGE_FILE_MACHINE_AMD64) {
      exportReplacements["DLL_NAME"] = baseDllAnsiName;
    }
    exports += ReplaceTemplate(exportCodeTemplate, exportReplacements);

    calls += ReplaceTemplate(callTemplate, {{"DLL_NAME", baseDllAnsiName}, {"FUNC_NAME", funcName}});
  }

  content = ReplaceTemplate(content,
                             {{"DLL_NAME", baseDllAnsiName},
                              {"STRUCT_MEMBERS", members},
                              {"EXPORT_FUNCTIONS", exports},
                              {"GET_PROC_ADDRESS_CALLS", calls}});

  std::ofstream(baseDllName + L".cpp") << content;
}

void GenerateASM(const std::wstring &baseDllName, const std::vector<std::string> &exportedNames) {
  std::string baseDllAnsiName = ToAnsiString(baseDllName);
  std::string asmFunctions;

  const std::string asmFuncTemplate = R"(Fake{{FUNC_NAME}} proc
  jmp qword ptr [{{DLL_NAME}} + {{OFFSET}}]
Fake{{FUNC_NAME}} endp

)";

  for (std::size_t i = 0; i < exportedNames.size(); ++i) {
    asmFunctions += ReplaceTemplate(asmFuncTemplate,
                                    {{"FUNC_NAME", exportedNames[i]},
                                     {"DLL_NAME", baseDllAnsiName},
                                     {"OFFSET", std::to_string(8 + i * 8)}});
  }

  std::string content = R"(.data
extern {{DLL_NAME}} : qword

.code

{{ASM_FUNCTIONS}}
end
)";

  content = ReplaceTemplate(content, {{"DLL_NAME", baseDllAnsiName}, {"ASM_FUNCTIONS", asmFunctions}});

  std::ofstream(baseDllName + L".asm") << content;
}

void GenerateCMake(const std::wstring &baseDllName, WORD fileType) {
  std::string content = LoadTemplate(L"cmake_lists.template");
  if (content.empty())
    return;

  std::string archSetting, archCheck;
  if (fileType == IMAGE_FILE_MACHINE_AMD64) {
    archCheck = "if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)\n"
                "    message(FATAL_ERROR \"This project is for 64-bit (x64) and must be built with an x64 generator.\")\n"
                "endif()";
    archSetting = "enable_language(ASM_MASM)\nset(ASM_SOURCES {{DLL_NAME}}.asm)";
  } else {
    archCheck = "if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)\n"
                "    message(FATAL_ERROR \"This project is for 32-bit (x86) and must be built with a 32-bit generator. Use 'cmake -A Win32 ..'\")\n"
                "endif()";
    archSetting = "set(ASM_SOURCES \"\")";
  }

  std::string baseDllAnsiName = ToAnsiString(baseDllName);
  content = ReplaceTemplate(content,
                             {{"ARCH_CHECK", archCheck},
                              {"ARCH_SPECIFIC_SETTING", archSetting},
                              {"DLL_NAME", baseDllAnsiName}});

  std::ofstream(L"CMakeLists.txt") << content;
}

int wmain(int argc, wchar_t *argv[]) {
  if (argc < 2)
    return 1;

  std::vector<std::wstring> args(argv, argv + argc);

  WORD fileType = 0;
  std::vector<std::string> exportedNames;

  IMAGE_NT_HEADERS ntHeaders;
  if (GetImageFileHeaders(args[1], ntHeaders)) {
    fileType = ntHeaders.FileHeader.Machine;
  }

  std::wstring baseDllName = std::filesystem::path(args[1]).stem().wstring();

  ListDLLFunctions(args[1], exportedNames);

  GenerateDEF(baseDllName, exportedNames);
  GenerateMainCPP(baseDllName, exportedNames, fileType);

  if (fileType == IMAGE_FILE_MACHINE_AMD64)
    GenerateASM(baseDllName, exportedNames);

  GenerateCMake(baseDllName, fileType);

  return 0;
}
