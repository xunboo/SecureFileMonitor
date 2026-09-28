#include "../app/Support.h"
#include <iostream>
#include <fstream>
#include <functional>
#include <stdexcept>

using namespace sfm;
static int passed = 0, failed = 0;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void Test(const char* name, const std::function<void()>& body) {
    try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) { ++failed; std::cout << "FAIL " << name << ": " << error.what() << '\n'; }
}
static void Throws(const std::function<void()>& body) {
    bool threw = false; try { body(); } catch (const std::exception&) { threw = true; }
    Require(threw, "Expected rejection");
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::cerr << "Usage: SecureFileMonitor.Tests <scratch-directory>\n"; return 2; }
    std::filesystem::path scratch = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(scratch);
    const std::wstring file = L"C:\\Documents\\important.txt", program = L"C:\\Apps\\editor.exe";
    Test("unmatched access asks", [&] { Require(Evaluate({}, file, program, SfmRead).action == Action::Ask, "Unexpected default"); });
    Test("allow an exact read", [&] {
        Require(Evaluate({{file, program, SfmRead, Action::Allow}}, file, program, SfmRead).action == Action::Allow, "Read not allowed");
    });
    Test("read-only rule cannot authorize a write", [&] {
        Require(Evaluate({{file, program, SfmRead, Action::Allow}}, file, program, SfmWrite).action == Action::Ask, "Write allowed");
    });
    Test("combined open needs every access bit", [&] {
        Require(Evaluate({{file, program, SfmRead, Action::Allow}}, file, program, SfmOpen | SfmRead | SfmWrite).action == Action::Ask, "Partial allow escaped");
    });
    Test("separate allows combine", [&] {
        Require(Evaluate({{file, program, SfmRead, Action::Allow}, {file, program, SfmWrite, Action::Allow}}, file, program, 7).action == Action::Allow, "Allows did not combine");
    });
    Test("deny wins regardless of order", [&] {
        std::vector<Rule> rules{{file, {}, 3, Action::Allow}, {file, program, SfmWrite, Action::Deny}};
        Require(Evaluate(rules, file, program, 7).action == Action::Deny, "Deny lost");
        std::reverse(rules.begin(), rules.end());
        Require(Evaluate(rules, file, program, 7).action == Action::Deny, "Order changed precedence");
    });
    Test("ask takes precedence over allow", [&] {
        Require(Evaluate({{file, {}, 3, Action::Allow}, {file, program, 1, Action::Ask}}, file, program, 1).action == Action::Ask, "Ask lost");
    });
    Test("different programs do not inherit permissions", [&] {
        Require(Evaluate({{file, program, 3, Action::Allow}}, file, L"C:\\Other\\editor.exe", 1).action == Action::Ask, "Basename match bypass");
    });
    Test("unknown program cannot match executable-specific rule", [&] {
        Require(Evaluate({{file, program, 3, Action::Allow}}, file, {}, 1).action == Action::Ask, "Unknown process allowed");
    });
    Test("explicit any-program rule matches unknown program", [&] {
        Require(Evaluate({{file, {}, 3, Action::Deny}}, file, {}, 1).action == Action::Deny, "Wildcard rule missed");
    });
    Test("paths compare ordinally without case", [&] {
        Require(Evaluate({{file, program, 3, Action::Allow}}, L"c:\\documents\\IMPORTANT.TXT", L"c:\\apps\\EDITOR.EXE", 1).action == Action::Allow, "Case mismatch");
    });
    Test("path prefix does not match a sibling", [&] {
        Require(!MatchesFile(file + L".backup", file), "Sibling matched");
        Require(!MatchesFile(file + L"\\child", file), "Directory child matched");
        Require(MatchesFile(file + L":private:$DATA", file), "Named stream missed");
    });
    Test("deny write leaves allow read intact", [&] {
        std::vector<Rule> rules{{file, program, 1, Action::Allow}, {file, {}, 2, Action::Deny}};
        Require(Evaluate(rules, file, program, 1).action == Action::Allow, "Read affected by write deny");
    });
    Test("UTF-8 handles Unicode file paths", [&] {
        std::wstring text = L"C:\\文档\\résumé-🔒.txt";
        Require(Wide(Utf8(text)) == text, "Unicode round trip failed");
        Throws([] { Wide(std::string("\xff")); });
    });
    Test("JSON escapes quotes backslashes and control characters", [&] {
        Require(JsonEscape("a\"b\\c\n\t") == "a\\\"b\\\\c\\u000a\\u0009", "Incorrect JSON escaping");
    });
    Test("CSV prevents formula injection and escapes quotes", [&] {
        Require(CsvEscape("=1+1") == "\"'=1+1\"", "Formula not neutralized");
        Require(CsvEscape("a,\"b\"") == "\"a,\"\"b\"\"\"", "Incorrect CSV escaping");
    });
    Test("settings persist Unicode and escaped paths atomically", [&] {
        Settings settings{{{file, {}}, {L"C:\\文档\\résumé.txt", {}}}, {{file, program, 1, Action::Allow}, {file, {}, 2, Action::Deny}}};
        auto path = scratch / L"roundtrip.sfm"; SaveSettings(path, settings); auto loaded = LoadSettings(path);
        Require(loaded.files.size() == 2 && loaded.files[1].path == settings.files[1].path, "Files lost");
        Require(loaded.rules.size() == 2 && loaded.rules[0].program == program, "Rules lost");
        Require(loaded.rules[1].action == Action::Deny, "Action lost");
        Require(!std::filesystem::exists(path.wstring() + L".tmp"), "Uncommitted temp file");
    });
    Test("corrupt configuration is rejected and preserved", [&] {
        auto path = scratch / L"corrupt.sfm"; { std::ofstream output(path); output << "SecureFileMonitor 1\nrule broken\n"; }
        Throws([&] { LoadSettings(path); }); Require(std::filesystem::exists(path), "Corrupt configuration removed");
    });
    Test("invalid configuration cannot replace valid settings", [&] {
        auto path = scratch / L"validate.sfm"; Settings settings{{{file, {}}}, {}}; SaveSettings(path, settings);
        settings.rules.push_back({file, {}, 99, Action::Allow}); Throws([&] { SaveSettings(path, settings); });
        Require(LoadSettings(path).rules.empty(), "Invalid save changed disk");
    });
    Test("orphan rules and duplicate files are rejected", [&] {
        Throws([&] { SaveSettings(scratch / L"orphan.sfm", {{}, {{file, program, 1, Action::Allow}}}); });
        Throws([&] { SaveSettings(scratch / L"duplicate.sfm", {{{file, {}}, {file, {}}}, {}}); });
    });
    Test("driver and broker wire layouts match", [&] {
        Require(sizeof(SfmRequest) == 2112 && sizeof(SfmEvent) == 2144 && sizeof(SfmReply) == 8, "Wire size mismatch");
        Require(offsetof(SfmPollResult, Events) == 40, "Batch offset mismatch");
        Require(sizeof(SfmConfiguration) == 262160, "Config size mismatch");
    });
    Test("PID creation time is verified before applying app rules", [&] {
        Require(!ProcessPath(GetCurrentProcessId(), 0).empty(), "Own process not resolved");
        Require(ProcessPath(GetCurrentProcessId(), 1).empty(), "PID reuse check failed");
    });
    Test("session logs contain completion and audit-gap data", [&] {
        Logger logger; logger.Open(scratch / L"logs");
        LogRow row; row.file = file; row.program = program;
        row.event.Request.Version = SFM_PROTOCOL_VERSION; row.event.Request.RequestId = 12;
        row.event.Request.TimeUtc = NowUtc(); row.event.CompletedUtc = NowUtc();
        row.event.Request.Operation = SfmWrite; row.event.Request.RequestedBytes = 16;
        row.event.Request.ProcessId = GetCurrentProcessId(); wcscpy_s(row.event.Request.Path, L"\\Device\\HarddiskVolume1\\important.txt");
        row.event.Decision = SfmDeny; row.event.Reason = SfmTimeout; row.event.Phase = SfmBlocked;
        row.event.Status = static_cast<LONG>(0xC0000022);
        logger.Append(row); logger.Diagnostic("lost_events", 7, L"overflow"); logger.Flush();
        logger.ExportCsv(scratch / L"export.csv");
        std::ifstream input(logger.Path()); std::string all((std::istreambuf_iterator<char>(input)), {});
        Require(all.find("\"decision\":\"deny\"") != std::string::npos, "Decision missing");
        Require(all.find("\"ntstatus\":\"0xC0000022\"") != std::string::npos, "Status missing");
        Require(all.find("\"count\":7") != std::string::npos, "Audit gap missing");
        Require(std::filesystem::file_size(scratch / L"export.csv") > 200, "Export missing");
    });
    std::cout << '\n' << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}

