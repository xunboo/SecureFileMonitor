#pragma once
#include "Core.h"
#include <mutex>
#include <filesystem>

namespace sfm {
class Handle {
    HANDLE value_ = INVALID_HANDLE_VALUE;
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) reset(other.release()); return *this; }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() { HANDLE result = value_; value_ = INVALID_HANDLE_VALUE; return result; }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) { if (*this) CloseHandle(value_); value_ = value; }
};
std::wstring ErrorText(DWORD error = GetLastError());
std::string Utf8(const std::wstring& text);
std::wstring Wide(const std::string& text);
LONGLONG NowUtc();
std::wstring Timestamp(LONGLONG time, bool filename = false);
std::filesystem::path DataDirectory();
ProtectedFile ResolveFile(const std::wstring& path);
std::wstring ProcessPath(ULONG pid, ULONGLONG expectedCreationTime);
Settings LoadSettings(const std::filesystem::path& path);
void SaveSettings(const std::filesystem::path& path, const Settings& settings);
std::wstring BrowseFile(HWND owner, bool executable = false);
bool IsAdministrator();

struct LogRow {
    SfmEvent event{};
    std::wstring file;
    std::wstring program;
};
class Logger {
    mutable std::mutex mutex_;
    Handle json_;
    Handle csv_;
    std::filesystem::path jsonPath_;
    std::filesystem::path csvPath_;
    void Write(HANDLE file, const std::string& text);
public:
    void Open(const std::filesystem::path& directory);
    void Append(const LogRow& row);
    void Diagnostic(const std::string& kind, ULONGLONG count, const std::wstring& detail);
    void Flush();
    void ExportCsv(const std::filesystem::path& destination);
    std::filesystem::path Path() const;
};
}
