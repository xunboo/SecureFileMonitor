#include "Support.h"
#include <commdlg.h>
#include <shlobj.h>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace sfm {
std::wstring ErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = buffer ? buffer : L"Windows error " + std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!length) throw std::runtime_error("Invalid UTF-16 text");
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}
std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (!length) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length);
    return result;
}
LONGLONG NowUtc() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime; value.HighPart = ft.dwHighDateTime;
    return static_cast<LONGLONG>(value.QuadPart);
}
std::wstring Timestamp(LONGLONG time, bool filename) {
    ULARGE_INTEGER value{}; value.QuadPart = static_cast<ULONGLONG>(time);
    FILETIME ft{value.LowPart, value.HighPart}; SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    wchar_t output[80]{};
    if (filename) swprintf_s(output, L"%04u%02u%02u-%02u%02u%02u-%03u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    else swprintf_s(output, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return output;
}
std::filesystem::path DataDirectory() {
    wchar_t* path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
        throw std::runtime_error("Cannot find LocalAppData");
    std::filesystem::path result = std::filesystem::path(path) / L"SecureFileMonitor";
    CoTaskMemFree(path);
    std::filesystem::create_directories(result);
    return result;
}
static std::wstring FinalPath(HANDLE file, DWORD flags) {
    std::vector<wchar_t> buffer(32768);
    DWORD size = GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (!size || size >= buffer.size()) throw std::runtime_error("Cannot resolve file path: " + Utf8(ErrorText()));
    return std::wstring(buffer.data(), size);
}
ProtectedFile ResolveFile(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file) throw std::runtime_error("Cannot open the selected file: " + Utf8(ErrorText()));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        throw std::runtime_error("Choose an existing file, not a directory.");
    wchar_t fsName[32]{};
    if (!GetVolumeInformationByHandleW(file.get(), nullptr, 0, nullptr, nullptr, nullptr, fsName, 32) ||
        _wcsicmp(fsName, L"NTFS") != 0)
        throw std::runtime_error("This version supports files on local NTFS volumes.");
    ProtectedFile result{FinalPath(file.get(), VOLUME_NAME_DOS), FinalPath(file.get(), VOLUME_NAME_NT)};
    if (result.path.rfind(L"\\\\?\\UNC\\", 0) == 0 || result.ntPath.rfind(L"\\Device\\Mup\\", 0) == 0)
        throw std::runtime_error("Network files are not supported.");
    if (result.path.rfind(L"\\\\?\\", 0) == 0) result.path.erase(0, 4);
    if (result.ntPath.size() >= SFM_MAX_PATH)
        throw std::runtime_error("The resolved path exceeds the driver limit of 1023 characters.");
    if (result.path.find(L':', 2) != std::wstring::npos)
        throw std::runtime_error("Select the base file. Its alternate data streams are included automatically.");
    return result;
}
std::wstring ProcessPath(ULONG pid, ULONGLONG expectedCreationTime) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return {};
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) return {};
    ULARGE_INTEGER time{}; time.LowPart = created.dwLowDateTime; time.HighPart = created.dwHighDateTime;
    if (expectedCreationTime && time.QuadPart != expectedCreationTime) return {};
    wchar_t buffer[32768]{}; DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!QueryFullProcessImageNameW(process.get(), 0, buffer, &size)) return {};
    return std::wstring(buffer, size);
}

static void ValidateSettings(const Settings& settings) {
    if (settings.files.size() > SFM_MAX_FILES || settings.rules.size() > 4096)
        throw std::runtime_error("Configuration exceeds file or rule limits.");
    for (size_t i = 0; i < settings.files.size(); ++i) {
        const auto& file = settings.files[i];
        if (file.path.empty() || file.path.size() >= 32768 || file.path.find(L'\0') != std::wstring::npos || file.path.find_first_of(L"\r\n\t") != std::wstring::npos)
            throw std::runtime_error("Invalid configured file path.");
        for (size_t j = 0; j < i; ++j)
            if (EqualPath(file.path, settings.files[j].path)) throw std::runtime_error("Duplicate configured file.");
    }
    for (const auto& rule : settings.rules) {
        if (!(rule.operations & (SfmRead | SfmWrite)) || (rule.operations & ~(SfmRead | SfmWrite)) ||
            (rule.action != Action::Ask && rule.action != Action::Allow && rule.action != Action::Deny))
            throw std::runtime_error("Invalid rule operation or action.");
        if (!std::any_of(settings.files.begin(), settings.files.end(), [&](const auto& f) { return EqualPath(f.path, rule.file); }))
            throw std::runtime_error("Rule refers to a file outside the protected list.");
        if (rule.program.find(L'\0') != std::wstring::npos || rule.program.find_first_of(L"\r\n\t") != std::wstring::npos)
            throw std::runtime_error("Invalid program path in rule.");
    }
}
Settings LoadSettings(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    if (std::filesystem::file_size(path) > 4 * 1024 * 1024)
        throw std::runtime_error("Configuration file is too large.");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read configuration.");
    std::string line;
    if (!std::getline(input, line) || line != "SecureFileMonitor 1")
        throw std::runtime_error("Unsupported or damaged configuration. The file has been left unchanged.");
    Settings result;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream row(line); std::string kind, pathText, programText;
        row >> kind;
        if (kind == "file") {
            if (!(row >> std::quoted(pathText))) throw std::runtime_error("Invalid file configuration.");
            result.files.push_back({Wide(pathText), {}});
        } else if (kind == "rule") {
            ULONG operations = 0; int action = 0;
            if (!(row >> std::quoted(pathText) >> std::quoted(programText) >> operations >> action) || action < 0 || action > 2)
                throw std::runtime_error("Invalid rule configuration.");
            result.rules.push_back({Wide(pathText), Wide(programText), operations, static_cast<Action>(action)});
        } else throw std::runtime_error("Unknown configuration entry.");
        row >> std::ws;
        if (!row.eof()) throw std::runtime_error("Unexpected data in configuration.");
    }
    ValidateSettings(result);
    return result;
}
void SaveSettings(const std::filesystem::path& path, const Settings& settings) {
    ValidateSettings(settings);
    std::ostringstream text; text << "SecureFileMonitor 1\n";
    for (const auto& file : settings.files) text << "file " << std::quoted(Utf8(file.path)) << '\n';
    for (const auto& rule : settings.rules)
        text << "rule " << std::quoted(Utf8(rule.file)) << ' ' << std::quoted(Utf8(rule.program))
             << ' ' << rule.operations << ' ' << static_cast<int>(rule.action) << '\n';
    auto temporary = path; temporary += L".tmp";
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) throw std::runtime_error("Cannot save configuration: " + Utf8(ErrorText()));
    const std::string bytes = text.str(); DWORD written = 0;
    if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() ||
        !FlushFileBuffers(file.get())) throw std::runtime_error("Cannot flush configuration to disk.");
    file.reset();
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot commit configuration: " + Utf8(ErrorText()));
}
std::wstring BrowseFile(HWND owner, bool executable) {
    wchar_t path[32768]{}; OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner; dialog.lpstrFile = path; dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter = executable ? L"Applications (*.exe)\0*.exe\0All files\0*.*\0" : L"All files\0*.*\0";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? path : L"";
}
bool IsAdministrator() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY; PSID sid = nullptr;
    BOOL member = FALSE;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &sid)) {
        CheckTokenMembership(nullptr, sid, &member); FreeSid(sid);
    }
    return member != FALSE;
}

void Logger::Write(HANDLE file, const std::string& text) {
    DWORD written = 0;
    if (!WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written != text.size())
        throw std::runtime_error("Writing access logs failed: " + Utf8(ErrorText()));
}
void Logger::Open(const std::filesystem::path& directory) {
    std::lock_guard lock(mutex_);
    std::filesystem::create_directories(directory);
    auto base = L"session-" + Timestamp(NowUtc(), true) + L"-" + std::to_wstring(GetCurrentProcessId());
    jsonPath_ = directory / (base + L".jsonl"); csvPath_ = directory / (base + L".csv");
    json_.reset(CreateFileW(jsonPath_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    csv_.reset(CreateFileW(csvPath_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!json_ || !csv_) throw std::runtime_error("Cannot create session logs: " + Utf8(ErrorText()));
    Write(csv_.get(), "\xEF\xBB\xBFtimestamp_utc,completed_utc,request_id,pid,process_created_utc,program,file,operation,decision,reason,phase,ntstatus,requested_bytes,transferred_bytes,offset,desired_access\r\n");
    Write(json_.get(), "{\"type\":\"session_start\",\"utc\":\"" + Utf8(Timestamp(NowUtc())) + "\"}\n");
}
void Logger::Append(const LogRow& row) {
    std::lock_guard lock(mutex_);
    const auto& e = row.event; const auto& r = e.Request;
    const std::string time = Utf8(Timestamp(r.TimeUtc)), completed = Utf8(Timestamp(e.CompletedUtc));
    const std::string file = Utf8(row.file), program = Utf8(row.program), op = Utf8(OperationText(r.Operation));
    const std::string decision = e.Decision == SfmAllow ? "allow" : "deny", reason = Utf8(ReasonText(e.Reason));
    const char* phase = e.Phase == SfmBlocked ? "blocked" : "completed";
    char status[16]{}; sprintf_s(status, "0x%08lX", static_cast<ULONG>(e.Status));
    std::ostringstream json;
    json << "{\"type\":\"access\",\"timestamp_utc\":\"" << time << "\",\"completed_utc\":\"" << completed
        << "\",\"request_id\":" << r.RequestId << ",\"pid\":" << r.ProcessId
        << ",\"process_created_utc\":" << r.ProcessCreatedUtc << ",\"program\":\"" << JsonEscape(program)
        << "\",\"file\":\"" << JsonEscape(file) << "\",\"nt_path\":\"" << JsonEscape(Utf8(r.Path))
        << "\",\"operation\":\"" << op << "\",\"decision\":\"" << decision << "\",\"reason\":\"" << reason
        << "\",\"phase\":\"" << phase << "\",\"ntstatus\":\"" << status << "\",\"requested_bytes\":"
        << r.RequestedBytes << ",\"transferred_bytes\":" << e.TransferredBytes << ",\"offset\":" << r.Offset
        << ",\"desired_access\":" << r.DesiredAccess << "}\n";
    std::ostringstream csv;
    csv << CsvEscape(time) << ',' << CsvEscape(completed) << ',' << r.RequestId << ',' << r.ProcessId << ','
        << r.ProcessCreatedUtc << ',' << CsvEscape(program) << ',' << CsvEscape(file) << ',' << CsvEscape(op)
        << ',' << decision << ',' << CsvEscape(reason) << ',' << phase << ',' << status << ',' << r.RequestedBytes
        << ',' << e.TransferredBytes << ',' << r.Offset << ',' << r.DesiredAccess << "\r\n";
    Write(json_.get(), json.str()); Write(csv_.get(), csv.str());
}
void Logger::Diagnostic(const std::string& kind, ULONGLONG count, const std::wstring& detail) {
    std::lock_guard lock(mutex_);
    auto time = Utf8(Timestamp(NowUtc()));
    Write(json_.get(), "{\"type\":\"" + JsonEscape(kind) + "\",\"utc\":\"" + time + "\",\"count\":" +
        std::to_string(count) + ",\"detail\":\"" + JsonEscape(Utf8(detail)) + "\"}\n");
    // A diagnostic row cannot be mistaken for a successful access.
    Write(csv_.get(), CsvEscape(time) + ",,,0,,," + CsvEscape(Utf8(detail)) + ",diagnostic,," +
        CsvEscape(kind) + ",diagnostic,," + std::to_string(count) + ",,,\r\n");
}
void Logger::Flush() {
    std::lock_guard lock(mutex_);
    if (json_ && !FlushFileBuffers(json_.get())) throw std::runtime_error("Cannot flush JSONL log.");
    if (csv_ && !FlushFileBuffers(csv_.get())) throw std::runtime_error("Cannot flush CSV log.");
}
void Logger::ExportCsv(const std::filesystem::path& destination) {
    std::lock_guard lock(mutex_);
    if (!csv_) throw std::runtime_error("Start a monitoring session before exporting.");
    if (!FlushFileBuffers(csv_.get()) || !CopyFileW(csvPath_.c_str(), destination.c_str(), FALSE))
        throw std::runtime_error("Cannot export CSV: " + Utf8(ErrorText()));
}
std::filesystem::path Logger::Path() const { std::lock_guard lock(mutex_); return jsonPath_; }
}
