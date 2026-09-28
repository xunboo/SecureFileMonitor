#pragma once
#include <Windows.h>
#include "../shared/Protocol.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace sfm {
enum class Action { Ask, Allow, Deny };
struct ProtectedFile {
    std::wstring path;
    std::wstring ntPath;
    bool enabled = true;
};
struct Rule {
    std::wstring file;
    std::wstring program; // empty means any program
    ULONG operations = SfmRead | SfmWrite;
    Action action = Action::Ask;
};
struct Settings {
    std::vector<ProtectedFile> files;
    std::vector<Rule> rules;
    bool startOnBoot = false;
    bool autoStartMonitoring = false;
};
struct Evaluation {
    Action action = Action::Ask;
    std::wstring explanation = L"No matching rule";
};

inline bool EqualPath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
class AccessNotificationFilter {
    using Clock = std::chrono::steady_clock;
    struct RecentAccess {
        std::wstring file, program;
        ULONG operation;
        DWORD unknownProgramPid;
        Clock::time_point lastSeen;
    };
    std::vector<RecentAccess> recent_;
public:
    void Clear() { recent_.clear(); }
    // Call under the broker state lock. Repeated accesses extend the quiet period.
    bool ShouldNotify(const std::wstring& file, const std::wstring& program,
                      ULONG operation, DWORD pid, Clock::time_point now = Clock::now()) {
        std::erase_if(recent_, [now](const RecentAccess& access) {
            return now - access.lastSeen >= std::chrono::seconds(10);
        });
        // Known executables group across process instances; unresolved programs
        // use the PID so unrelated unknown processes do not suppress each other.
        const DWORD unknownProgramPid = program.empty() ? pid : 0;
        for (auto& access : recent_) {
            if (access.operation == operation && access.unknownProgramPid == unknownProgramPid &&
                EqualPath(access.file, file) && EqualPath(access.program, program)) {
                access.lastSeen = now;
                return false;
            }
        }
        recent_.push_back({file, program, operation, unknownProgramPid, now});
        return true;
    }
};
inline bool MatchesFile(const std::wstring& request, const std::wstring& target) {
    // Include named streams of the selected file, never sibling prefixes.
    if (EqualPath(request, target)) return true;
    return request.size() > target.size() && request[target.size()] == L':' &&
        EqualPath(request.substr(0, target.size()), target);
}
inline Evaluation Evaluate(const std::vector<Rule>& rules, const std::wstring& file,
                           const std::wstring& program, ULONG operations) {
    operations &= SfmRead | SfmWrite;
    if (!operations) return {Action::Ask, L"Metadata/open request"};
    ULONG allowed = 0;
    bool ask = false;
    for (const auto& rule : rules) {
        if (!EqualPath(rule.file, file) || (!rule.program.empty() &&
            (program.empty() || !EqualPath(rule.program, program)))) continue;
        const ULONG covered = rule.operations & operations;
        if (!covered) continue;
        if (rule.action == Action::Deny) return {Action::Deny, L"Matching deny rule"};
        if (rule.action == Action::Ask) ask = true;
        if (rule.action == Action::Allow) allowed |= covered;
    }
    if (ask) return {Action::Ask, L"Matching ask rule"};
    if ((allowed & operations) == operations) return {Action::Allow, L"Matching allow rule"};
    return {Action::Ask, L"No rule allows every requested operation"};
}
inline std::wstring OperationText(ULONG operation) {
    std::wstring result;
    if (operation & SfmOpen) result = L"Open";
    if (operation & SfmRead) result += result.empty() ? L"Read" : L" + read";
    if (operation & SfmWrite) result += result.empty() ? L"Write" : L" + write";
    return result.empty() ? L"Other" : result;
}
inline std::wstring ActionText(Action action) {
    return action == Action::Allow ? L"Allow" : action == Action::Deny ? L"Deny" : L"Ask";
}
inline std::wstring ReasonText(ULONG reason) {
    switch (reason) {
    case SfmRule: return L"Saved rule";
    case SfmUser: return L"User decision";
    case SfmTimeout: return L"Decision timed out";
    case SfmDisconnected: return L"Broker disconnected";
    case SfmOverload: return L"Too many pending requests";
    case SfmUnsafeToPend: return L"Cannot safely defer this I/O";
    case SfmResourceFailure: return L"Resource allocation failed";
    case SfmStopping: return L"Monitoring stopped";
    case SfmInvalidReply: return L"Invalid broker reply";
    case SfmSessionRule: return L"Process session permission";
    case SfmProcMon: return L"ProcMon observation";
    default: return L"Unknown";
    }
}
inline std::string JsonEscape(const std::string& input) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result;
}
inline std::string CsvEscape(std::string input) {
    // Spreadsheet-safe export, including user-controlled file names.
    if (!input.empty() && (input[0] == '=' || input[0] == '+' || input[0] == '-' ||
        input[0] == '@' || input[0] == '\t' || input[0] == '\r')) input.insert(0, "'");
    std::string result = "\"";
    for (char c : input) { if (c == '"') result += '"'; result += c; }
    return result + '"';
}
}

