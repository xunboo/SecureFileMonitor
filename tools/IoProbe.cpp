#include <Windows.h>
#include <iostream>
#include <string>

// Exercise actual CreateFile/ReadFile/WriteFile calls from a different process.
int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || (wcscmp(argv[1], L"read") && wcscmp(argv[1], L"write"))) {
        std::wcerr << L"Usage: IoProbe.exe read|write <existing-file> [--pause-before-io]\n"
                      L"write appends one diagnostic line; use a disposable test file.\n";
        return 2;
    }
    const bool write = wcscmp(argv[1], L"write") == 0;
    HANDLE file = CreateFileW(argv[2], write ? FILE_APPEND_DATA : GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { std::cerr << "CreateFile failed: " << GetLastError() << '\n'; return 1; }
    std::cout << "Opened. PID: " << GetCurrentProcessId() << '\n';
    if (argc > 3 && wcscmp(argv[3], L"--pause-before-io") == 0) {
        std::cout << "Enable monitoring now, then press Enter to exercise an existing handle.\n"; std::cin.get();
    }
    DWORD transferred = 0; BOOL ok;
    if (write) {
        const char text[] = "SecureFileMonitor IoProbe test\r\n";
        ok = WriteFile(file, text, sizeof(text) - 1, &transferred, nullptr);
    } else { char data[4096]{}; ok = ReadFile(file, data, sizeof(data), &transferred, nullptr); }
    DWORD error = ok ? ERROR_SUCCESS : GetLastError(); CloseHandle(file);
    std::cout << (write ? "WriteFile" : "ReadFile") << ": " << (ok ? "success" : "denied/failed")
        << ", Windows error " << error << ", bytes " << transferred << '\n';
    return ok ? 0 : 1;
}

