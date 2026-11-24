#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

struct WindowSearch
{
    DWORD pid;
    HWND hwnd{};
};

BOOL CALLBACK FindWindowForProcess(HWND hwnd, LPARAM data)
{
    auto& result = *reinterpret_cast<WindowSearch*>(data);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != result.pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    result.hwnd = hwnd;
    return FALSE;
}

void LogModules(DWORD pid)
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot, &module))
    {
        do
        {
            std::wstring name = module.szModule;
            if (name.starts_with(L"RTSS") || name.starts_with(L"nvspcap") ||
                name.starts_with(L"VkLayer_") || name == L"vulkan-1.dll")
                std::wcout << L"Loaded " << module.szExePath << L'\n';
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
}

bool DropFile(HWND window, const std::wstring& path)
{
    const auto size = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    if (!memory)
        return false;
    auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
    if (!drop)
    {
        GlobalFree(memory);
        return false;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    memcpy(drop + 1, path.data(), path.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    if (PostMessageW(window, WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0))
        return true;
    GlobalFree(memory);
    return false;
}

bool ExerciseWindow(HWND window)
{
    for (int i = 0; i < 8; ++i)
    {
        const auto width = 840 + 50 * (i % 5);
        const auto height = 540 + 32 * (i % 4);
        if (!SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
            return false;
        std::cout << "Resize " << width << 'x' << height << '\n';
        Sleep(170);
        if (i % 3 == 1)
        {
            ShowWindowAsync(window, SW_MINIMIZE);
            Sleep(400);
            if (!IsIconic(window))
                return false;
            ShowWindowAsync(window, SW_RESTORE);
            Sleep(450);
            if (IsIconic(window))
                return false;
            std::cout << "Minimize/restore completed\n";
        }
        if (i % 2 == 0)
        {
            wchar_t absolute[MAX_PATH]{};
            const auto name = i % 4 == 0 ? L"Environments/sunset.hdr" : L"Environments/studio.hdr";
            const auto length = GetFullPathNameW(name, MAX_PATH, absolute, nullptr);
            if (!length || length >= MAX_PATH || !DropFile(window, absolute))
                return false;
            std::wcout << L"Dropped " << absolute << L'\n';
            Sleep(350);
        }
    }
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 6)
    {
        std::cerr << "Usage: validation-launch LOG normal|loader-only|isolated wait|stress|interactive APP ARGUMENTS\n";
        return 2;
    }
    const std::wstring mode = argv[2];
    const std::wstring action = argv[3];
    if ((mode != L"normal" && mode != L"loader-only" && mode != L"isolated") ||
        (action != L"wait" && action != L"stress" && action != L"interactive"))
        return 2;
    SetEnvironmentVariableW(L"VK_LAYER_VALIDATE_SYNC", L"1");
    SetEnvironmentVariableW(L"VK_LOADER_DEBUG", L"layer");
    SetEnvironmentVariableW(L"DISABLE_RTSS_LAYER", mode == L"normal" ? nullptr : L"1");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attributes(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size))
        return 3;
    DWORD64 policy = PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON;
    if (mode == L"isolated" &&
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
                                   &policy, sizeof(policy), nullptr, nullptr))
        return 4;
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto output = CreateFileW(argv[1], GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE)
        return 5;
    const auto input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &inherit, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = output;
    startup.StartupInfo.hStdInput = input;
    PROCESS_INFORMATION process{};
    auto command = L"\"" + std::wstring(argv[4]) + L"\" " + argv[5];
    const auto started = CreateProcessW(argv[4], command.data(), nullptr, nullptr, TRUE,
                                         EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                         &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(output);
    CloseHandle(input);
    if (!started)
        return 6;
    CloseHandle(process.hThread);

    bool stressPassed = true;
    if (action == L"stress")
    {
        WindowSearch search{process.dwProcessId};
        for (int attempt = 0; attempt < 100 && !search.hwnd; ++attempt)
        {
            EnumWindows(FindWindowForProcess, reinterpret_cast<LPARAM>(&search));
            if (!search.hwnd && WaitForSingleObject(process.hProcess, 50) != WAIT_TIMEOUT)
                break;
        }
        stressPassed = search.hwnd != nullptr;
        if (search.hwnd)
        {
            Sleep(900);
            LogModules(process.dwProcessId);
            stressPassed = ExerciseWindow(search.hwnd);
            PostMessageW(search.hwnd, WM_CLOSE, 0, 0);
        }
    }
    const auto wait = WaitForSingleObject(process.hProcess, action == L"interactive" ? INFINITE : 15000);
    if (wait != WAIT_OBJECT_0)
    {
        CloseHandle(process.hProcess);
        return 12;
    }
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    std::cout << "Aether exit=" << exitCode << '\n';
    if (!stressPassed)
    {
        std::cerr << "Desktop window exercise failed. Check the Aether log.\n";
        return 13;
    }
    return static_cast<int>(exitCode);
}
