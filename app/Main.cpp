#include "Broker.h"
#include "resource.h"
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <objidl.h>
#include <objbase.h>
#include <gdiplus.h>
#include <array>
#include <deque>
#include <sstream>

using namespace sfm;
namespace {
constexpr UINT WM_TRAY = WM_APP + 1, WM_ROWS = WM_APP + 2, WM_REQUESTS = WM_APP + 3, WM_STATUS = WM_APP + 4;
constexpr int ID_START = 200, ID_TAB = 201, ID_ACTION1 = 202, ID_ACTION2 = 203, ID_ACTION3 = 204;
constexpr UINT ID_TRAY_OPEN = 300, ID_TRAY_TOGGLE = 301, ID_TRAY_LOGS = 302, ID_TRAY_EXIT = 303;
constexpr int ID_NAV_FIRST = 500;
constexpr int ID_OPT_BOOT = 601, ID_OPT_MONITOR = 602;

void Rounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    const float x = static_cast<float>(rect.left), y = static_cast<float>(rect.top);
    const float width = static_cast<float>(rect.right - rect.left - 1), height = static_cast<float>(rect.bottom - rect.top - 1);
    const float diameter = static_cast<float>(radius * 2);
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90); path.CloseFigure();
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(fill), GetGValue(fill), GetBValue(fill)));
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(border), GetGValue(border), GetBValue(border)), 1.0f);
    graphics.FillPath(&brush, &path); graphics.DrawPath(&pen, &path);
}

std::wstring Text(HWND window) {
    int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1); text.resize(length); return text;
}
void Error(HWND owner, const std::exception& error) {
    MessageBoxW(owner, Wide(error.what()).c_str(), L"Secure File Monitor", MB_OK | MB_ICONERROR);
}
struct RuleDialog {
    const Settings* settings;
    Rule result;
    bool edit = false;
};
INT_PTR CALLBACK RuleProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto dialog = reinterpret_cast<RuleDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<RuleDialog*>(lparam); SetWindowLongPtrW(window, DWLP_USER, lparam);
        int selected = 0;
        for (size_t i = 0; i < dialog->settings->files.size(); ++i) {
            const auto& file = dialog->settings->files[i];
            SendDlgItemMessageW(window, IDC_FILE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(file.path.c_str()));
            if (EqualPath(file.path, dialog->result.file)) selected = static_cast<int>(i);
        }
        SendDlgItemMessageW(window, IDC_FILE, CB_SETCURSEL, selected, 0);
        SetDlgItemTextW(window, IDC_PROGRAM, dialog->result.program.c_str());
        CheckDlgButton(window, IDC_READ, (dialog->result.operations & SfmRead) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window, IDC_WRITE, (dialog->result.operations & SfmWrite) ? BST_CHECKED : BST_UNCHECKED);
        for (const auto* text : {L"Ask", L"Allow", L"Deny"})
            SendDlgItemMessageW(window, IDC_ACTION, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendDlgItemMessageW(window, IDC_ACTION, CB_SETCURSEL, static_cast<int>(dialog->result.action), 0);
        SetWindowTextW(window, dialog->edit ? L"Edit file access rule" : L"Add file access rule");
        return TRUE;
    }
    if (message == WM_COMMAND && dialog) {
        if (LOWORD(wparam) == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
        try {
            if (LOWORD(wparam) == IDC_BROWSE) {
                auto file = BrowseFile(window, true);
                if (!file.empty()) SetDlgItemTextW(window, IDC_PROGRAM, file.c_str());
                return TRUE;
            }
            if (LOWORD(wparam) == IDOK) {
                int selected = static_cast<int>(SendDlgItemMessageW(window, IDC_FILE, CB_GETCURSEL, 0, 0));
                if (selected < 0) throw std::runtime_error("Choose a protected file.");
                dialog->result.file = dialog->settings->files.at(selected).path;
                dialog->result.program = Text(GetDlgItem(window, IDC_PROGRAM));
                if (!dialog->result.program.empty()) dialog->result.program = ResolveFile(dialog->result.program).path;
                dialog->result.operations = (IsDlgButtonChecked(window, IDC_READ) == BST_CHECKED ? SfmRead : 0u) |
                    (IsDlgButtonChecked(window, IDC_WRITE) == BST_CHECKED ? SfmWrite : 0u);
                if (!dialog->result.operations) throw std::runtime_error("Choose Read, Write, or both.");
                dialog->result.action = static_cast<Action>(SendDlgItemMessageW(window, IDC_ACTION, CB_GETCURSEL, 0, 0));
                EndDialog(window, IDOK); return TRUE;
            }
        } catch (const std::exception& error) { Error(window, error); }
    }
    return FALSE;
}

class App {
    HINSTANCE instance_;
    HWND window_ = nullptr, title_ = nullptr, subtitle_ = nullptr, start_ = nullptr;
    HWND status_ = nullptr, note_ = nullptr, prompt_ = nullptr;
    std::array<HWND, 3> actions_{};
    std::array<HWND, 4> lists_{};
    std::array<HWND, 5> navigation_{};
    HWND optionsHeading_ = nullptr;
    HWND chkStartOnBoot_ = nullptr;
    HWND chkAutoStart_ = nullptr;
    HWND lblOptDesc_ = nullptr;
    HWND driverHeading_ = nullptr;
    HWND lblDriverInfo_ = nullptr;
    HFONT font_ = nullptr, titleFont_ = nullptr;
    HBRUSH background_ = CreateSolidBrush(RGB(245, 247, 250));
    UINT taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    int page_ = 0;
    bool exiting_ = false, trayAdded_ = false, configHealthy_ = true;
    bool updatingList_ = false;
    std::filesystem::path data_, config_;
    Settings settings_;
    Logger logger_;
    Broker broker_{logger_};
    std::mutex uiMutex_;
    std::deque<LogRow> incomingRows_;
    std::deque<std::shared_ptr<PendingRequest>> incomingRequests_;
    std::wstring incomingStatus_;
    std::vector<std::shared_ptr<PendingRequest>> requests_;
    std::shared_ptr<PendingRequest> currentRequest_;
    std::atomic<ULONGLONG> totalEvents_{0}, deniedEvents_{0};
    int Scale(int value) const { return MulDiv(value, static_cast<int>(GetDpiForWindow(window_)), 96); }
    HWND Control(const wchar_t* klass, const wchar_t* text, DWORD style, int id = 0) {
        HWND result = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE); return result;
    }
    void Fonts() {
        if (font_) DeleteObject(font_);
        if (titleFont_) DeleteObject(titleFont_);
        font_ = CreateFontW(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        titleFont_ = CreateFontW(-Scale(26), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (HWND child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(child == title_ ? titleFont_ : font_), TRUE);
    }
    void Columns(HWND list, std::initializer_list<std::pair<const wchar_t*, int>> columns) {
        int i = 0;
        for (auto [text, width] : columns) {
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.pszText = const_cast<wchar_t*>(text); column.cx = Scale(width); column.iSubItem = i;
            ListView_InsertColumn(list, i++, &column);
        }
    }
    void Row(HWND list, const std::vector<std::wstring>& values, int index = -1) {
        if (index < 0) index = ListView_GetItemCount(list);
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = index;
        item.pszText = const_cast<wchar_t*>(values.front().c_str());
        int inserted = ListView_InsertItem(list, &item);
        for (size_t i = 1; i < values.size(); ++i)
            ListView_SetItemText(list, inserted, static_cast<int>(i), const_cast<wchar_t*>(values[i].c_str()));
    }
    int Selected(HWND list) const { return ListView_GetNextItem(list, -1, LVNI_SELECTED); }
    void Balloon(const std::wstring& title, const std::wstring& text, DWORD icon = NIIF_WARNING) {
        NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.uID = 1; data.uFlags = NIF_INFO;
        wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE); wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
        data.dwInfoFlags = icon; Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    void Tray() {
        NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.uID = 1;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; data.uCallbackMessage = WM_TRAY;
        UINT dpi = GetDpiForWindow(window_);
        data.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_MONITOR), IMAGE_ICON,
            GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_SHARED));
        wcscpy_s(data.szTip, broker_.IsMonitoring() ?
            (broker_.IsProcMonMode() ? L"Secure File Monitor - ProcMon mode" : L"Secure File Monitor - monitoring") :
            L"Secure File Monitor - stopped");
        if (!trayAdded_) {
            trayAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
            data.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &data);
        } else Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    void Show() { ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_); }
    void UpdateStatusLine(const std::wstring& customMessage = L"") {
        std::wstring driver = broker_.GetDriverStatusText();
        std::wstring line = L"Driver: " + driver + L"   |   ";
        if (!customMessage.empty()) {
            line += customMessage;
        } else if (broker_.IsMonitoring()) {
            size_t enabledCount = std::count_if(settings_.files.begin(), settings_.files.end(), [](const auto& f) { return f.enabled; });
            line += L"Monitoring active (" + std::to_wstring(enabledCount) + L" protected files, " +
                    std::to_wstring(settings_.rules.size()) + L" rules).";
        } else {
            line += L"Ready. Click Start monitoring to begin.";
        }
        SetWindowTextW(status_, line.c_str());
    }
    void Layout() {
        RECT area{}; GetClientRect(window_, &area); int width = area.right, height = area.bottom;
        // Move every control before repainting so a resize cannot draw a mix of
        // old and new positions or leave stale text in newly exposed areas.
        auto move = [](HWND h, int x, int y, int w, int v) { MoveWindow(h, x, y, (std::max)(1, w), (std::max)(1, v), FALSE); };
        move(title_, Scale(238), Scale(26), width - Scale(454), Scale(38));
        move(subtitle_, Scale(240), Scale(71), width - Scale(264), Scale(24));
        move(start_, width - Scale(206), Scale(30), Scale(178), Scale(38));
        for (int i = 0; i < 5; ++i) move(navigation_[i], Scale(14), Scale(123 + i * 49), Scale(180), Scale(41));
        for (int i = 0; i < 3; ++i) move(actions_[i], Scale(254 + i * 173), Scale(224), Scale(160), Scale(32));
        for (HWND list : lists_) move(list, Scale(254), Scale(273), width - Scale(298), height - Scale(407));
        move(optionsHeading_, Scale(274), Scale(279), width - Scale(338), Scale(22));
        move(chkStartOnBoot_, Scale(274), Scale(304), width - Scale(338), Scale(26));
        move(chkAutoStart_, Scale(274), Scale(336), width - Scale(338), Scale(26));
        move(lblOptDesc_, Scale(274), Scale(368), width - Scale(338), Scale(52));
        move(driverHeading_, Scale(274), Scale(451), width - Scale(338), Scale(22));
        move(lblDriverInfo_, Scale(274), Scale(475), width - Scale(338), Scale(88));
        move(note_, Scale(240), height - Scale(108), width - Scale(268), Scale(40));
        move(status_, Scale(240), height - Scale(58), width - Scale(268), Scale(44));
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    void Page(int page) {
        page_ = page;
        for (HWND nav : navigation_) InvalidateRect(nav, nullptr, FALSE);
        const wchar_t* texts[5][3] = {
            {L"Add file...", L"Remove selected", L"Enable / Disable"},
            {L"Add rule...", L"Edit selected...", L"Remove selected"},
            {L"Export session CSV...", L"Open logs folder", L"Clear view"},
            {L"Review request", L"Allow once", L"Block once"},
            {L"Open startup apps", L"Open config folder", L""}
        };
        const wchar_t* notes[] = {
            L"Select individual files on local NTFS volumes. Named streams are included. Stop monitoring to change this list.",
            L"Rules use full executable paths. Deny overrides Ask, then Allow. Editing rules clears temporary process permissions.",
            L"The view keeps the latest 2,000 events. JSONL and CSV session logs keep all received events, including denials and failures. Times are UTC.",
            L"The requesting program waits for your decision. Closing a prompt or reaching its 20-second deadline denies that operation.",
            L"Configure Windows startup and monitoring automation preferences."
        };
        for (int i = 0; i < 4; ++i) ShowWindow(lists_[i], i == page ? SW_SHOW : SW_HIDE);
        const int optShow = (page == 4) ? SW_SHOW : SW_HIDE;
        ShowWindow(optionsHeading_, optShow);
        ShowWindow(chkStartOnBoot_, optShow);
        ShowWindow(chkAutoStart_, optShow);
        ShowWindow(lblOptDesc_, optShow);
        ShowWindow(driverHeading_, optShow);
        ShowWindow(lblDriverInfo_, optShow);
        for (int i = 0; i < 3; ++i) {
            SetWindowTextW(actions_[i], texts[page][i]); ShowWindow(actions_[i], *texts[page][i] ? SW_SHOW : SW_HIDE);
            EnableWindow(actions_[i], page != 0 || !broker_.IsMonitoring());
        }
        SetWindowTextW(note_, notes[page]);
        // Repaint newly exposed areas and the selected page's controls.
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    void RefreshSettings() {
        struct Guard {
            bool& flag;
            Guard(bool& f) : flag(f) { flag = true; }
            ~Guard() { flag = false; }
        } guard(updatingList_);
        ListView_DeleteAllItems(lists_[0]); ListView_DeleteAllItems(lists_[1]);
        for (size_t i = 0; i < settings_.files.size(); ++i) {
            const auto& file = settings_.files[i];
            std::wstring state;
            if (!file.enabled) state = L"Disabled";
            else if (broker_.IsFaulted()) state = L"State unknown";
            else if (broker_.IsMonitoring()) state = L"Monitoring";
            else state = L"Stopped";
            Row(lists_[0], {file.enabled ? L"Enabled" : L"Disabled", file.path, L"Exact path + streams", state});
            ListView_SetCheckState(lists_[0], static_cast<int>(i), file.enabled ? TRUE : FALSE);
        }
        for (const auto& rule : settings_.rules)
            Row(lists_[1], {rule.file, rule.program.empty() ? L"Any program" : rule.program, OperationText(rule.operations), ActionText(rule.action)});
        SetWindowTextW(start_, broker_.IsMonitoring() ? L"Stop monitoring" : L"Start monitoring");
        SetWindowTextW(subtitle_, broker_.IsFaulted() ? L"Driver connection fault. Configured files may still be blocked." :
            broker_.IsMonitoring() ? (broker_.IsProcMonMode() ?
                L"Monitoring selected files via ProcMon driver (notification-only mode)." :
                L"Monitoring selected files. You're in control of each access.") :
            L"Choose your files. Set the rules. Stay in control.");
        CheckDlgButton(window_, ID_OPT_BOOT, settings_.startOnBoot ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window_, ID_OPT_MONITOR, settings_.autoStartMonitoring ? BST_CHECKED : BST_UNCHECKED);
        if (lblDriverInfo_) {
            std::wstring drvDetails = L"Current driver: " + broker_.GetDriverStatusText() + L"\n" +
                (broker_.IsProcMonMode() ?
                    L"Mode: Sysinternals ProcMon (PROCMON24.SYS / PROCMON25.SYS) - Passive notification mode.\n"
                    L"Pre-operation blocking is disabled; file accesses notify immediately via tray alerts." :
                 broker_.GetDriverType() == DriverType::SecureFileMonitor ?
                    L"Mode: SecureFileMonitor.sys Minifilter - Active interception and blocking mode.\n"
                    L"Pre-operation access is intercepted with 20-second decision prompts." :
                    L"Mode: Standby - No driver is currently intercepting or observing files.\n"
                    L"Click Start monitoring to load or connect to the driver.");
            SetWindowTextW(lblDriverInfo_, drvDetails.c_str());
        }
        UpdateStatusLine();
        Page(page_); Tray(); InvalidateRect(window_, nullptr, FALSE);
    }
    void Save(Settings next) {
        if (!configHealthy_) throw std::runtime_error("Fix or move the damaged settings file and restart before saving changes.");
        SaveSettings(config_, next); settings_ = std::move(next); broker_.UpdateRules(settings_.rules); RefreshSettings();
    }
    void Toggle() {
        try {
            if (broker_.IsMonitoring()) broker_.Pause();
            else {
                if (!configHealthy_) throw std::runtime_error("Resolve the configuration error and restart before monitoring.");
                broker_.Start(settings_);
            }
        } catch (...) { RefreshSettings(); throw; }
        RefreshSettings();
    }
    void OpenLogs() { ShellExecuteW(window_, L"open", (data_ / L"Logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    void Export() {
        wchar_t path[32768] = L"SecureFileMonitor-session.csv";
        OPENFILENAMEW dialog{sizeof(dialog)}; dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"CSV files\0*.csv\0"; dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(std::size(path)); dialog.lpstrDefExt = L"csv";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&dialog)) { logger_.ExportCsv(path); SetWindowTextW(status_, L"Session CSV exported."); }
    }
    void Action(int button) {
        if (page_ == 0) {
            if (broker_.IsMonitoring()) return;
            Settings next = settings_;
            if (button == 0) {
                auto path = BrowseFile(window_); if (path.empty()) return;
                auto file = ResolveFile(path);
                wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
                auto dataPrefix = data_.wstring() + L"\\";
                if (EqualPath(file.path, exe) || (file.path.size() >= dataPrefix.size() && EqualPath(file.path.substr(0, dataPrefix.size()), dataPrefix)))
                    throw std::runtime_error("The monitor cannot protect its own executable, settings, or logs.");
                for (const auto& existing : next.files)
                    if (EqualPath(existing.ntPath, file.ntPath) || EqualPath(existing.path, file.path))
                        throw std::runtime_error("This file is already in the protected list.");
                if (next.files.size() >= SFM_MAX_FILES) throw std::runtime_error("The protected file limit is 128.");
                next.files.push_back(std::move(file));
            } else if (button == 1) {
                int selected = Selected(lists_[0]); if (selected < 0) return;
                auto file = next.files.at(selected).path; next.files.erase(next.files.begin() + selected);
                std::erase_if(next.rules, [&](const Rule& rule) { return EqualPath(rule.file, file); });
            } else if (button == 2) {
                int selected = Selected(lists_[0]);
                if (selected < 0 || selected >= static_cast<int>(next.files.size())) return;
                next.files[selected].enabled = !next.files[selected].enabled;
            }
            Save(std::move(next));
        } else if (page_ == 1) {
            if (settings_.files.empty()) throw std::runtime_error("Add a protected file first.");
            int selected = Selected(lists_[1]);
            if (button != 0 && selected < 0) return;
            Settings next = settings_;
            if (button == 2) next.rules.erase(next.rules.begin() + selected);
            else {
                RuleDialog dialog{&settings_, button == 1 ? settings_.rules.at(selected) : Rule{}, button == 1};
                if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_RULE), window_, RuleProc, reinterpret_cast<LPARAM>(&dialog)) != IDOK) return;
                if (button == 0) next.rules.push_back(dialog.result); else next.rules.at(selected) = dialog.result;
            }
            Save(std::move(next));
        } else if (page_ == 2) {
            if (button == 0) Export(); else if (button == 1) OpenLogs(); else ListView_DeleteAllItems(lists_[2]);
        } else if (page_ == 3) {
            int selected = Selected(lists_[3]); if (selected < 0 || selected >= static_cast<int>(requests_.size())) return;
            if (button == 0) ShowPrompt(requests_[selected]);
            else broker_.Decide(requests_[selected], button == 1, false);
            RefreshRequests();
        } else if (page_ == 4) {
            if (button == 0) ShellExecuteW(window_, L"open", L"ms-settings:startupapps", nullptr, nullptr, SW_SHOWNORMAL);
            else if (button == 1) ShellExecuteW(window_, L"open", data_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    void RefreshRequests() {
        for (const auto& request : requests_) {
            if (!request->completed && NowUtc() >= request->request.DeadlineUtc) broker_.Expire(request);
        }
        std::erase_if(requests_, [](const auto& request) { return request->completed.load(); });
        int selected = Selected(lists_[3]);
        ListView_DeleteAllItems(lists_[3]);
        for (const auto& request : requests_) {
            auto seconds = (std::max)(0LL, (request->request.DeadlineUtc - NowUtc() + 9999999) / 10000000);
            Row(lists_[3], {std::to_wstring(seconds) + L" s", std::to_wstring(request->request.ProcessId),
                request->program.empty() ? L"Unknown / exited process" : request->program,
                request->file, OperationText(request->request.Operation)});
        }
        if (selected >= 0 && selected < static_cast<int>(requests_.size())) ListView_SetItemState(lists_[3], selected, LVIS_SELECTED, LVIS_SELECTED);
        if (prompt_ && currentRequest_ && currentRequest_->completed) {
            DestroyWindow(prompt_); prompt_ = nullptr; currentRequest_.reset();
        }
        if (!prompt_ && !requests_.empty()) ShowPrompt(requests_.front());
    }
    void ShowPrompt(const std::shared_ptr<PendingRequest>& request) {
        if (request->completed) return;
        if (prompt_) { SetForegroundWindow(prompt_); return; }
        currentRequest_ = request;
        prompt_ = CreateDialogParamW(instance_, MAKEINTRESOURCEW(IDD_REQUEST), window_, PromptProc, reinterpret_cast<LPARAM>(this));
        if (!prompt_) { broker_.Decide(request, false, false); return; }
        ShowWindow(prompt_, SW_SHOW); SetForegroundWindow(prompt_);
    }
    void PromptDecision(bool allow, bool currentOnly = false) {
        if (!currentRequest_) return;
        auto request = currentRequest_;
        bool remember = !currentOnly && IsDlgButtonChecked(prompt_, IDC_REMEMBER) == BST_CHECKED;
        bool session = !currentOnly && IsDlgButtonChecked(prompt_, IDC_SESSION) == BST_CHECKED;
        // Persist first so a following ReadFile cannot race an approved CreateFile.
        if (remember && !request->program.empty() && !request->completed && NowUtc() < request->request.DeadlineUtc) {
            Settings next = settings_;
            ULONG operations = request->request.Operation & (SfmRead | SfmWrite);
            for (auto& rule : next.rules) {
                if (EqualPath(rule.file, request->file) && EqualPath(rule.program, request->program)) rule.operations &= ~operations;
            }
            std::erase_if(next.rules, [](const Rule& rule) { return !rule.operations; });
            next.rules.push_back({request->file, request->program, operations, allow ? Action::Allow : Action::Deny});
            Save(std::move(next));
        }
        broker_.Decide(request, allow, session && !remember);
        DestroyWindow(prompt_); prompt_ = nullptr; currentRequest_.reset(); RefreshRequests();
    }
    static INT_PTR CALLBACK PromptProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, DWLP_USER));
        if (message == WM_INITDIALOG) {
            app = reinterpret_cast<App*>(lparam); SetWindowLongPtrW(window, DWLP_USER, lparam);
            auto request = app->currentRequest_;
            std::wstring details = L"File: " + request->file + L"\r\n\r\nProgram: " +
                (request->program.empty() ? L"Unknown (could not verify executable path)" : request->program) +
                L"\r\nPID: " + std::to_wstring(request->request.ProcessId) + L"\r\nAccess: " +
                OperationText(request->request.Operation) + L"\r\n\r\n" + request->explanation;
            SetDlgItemTextW(window, IDC_DETAILS, details.c_str());
            CheckDlgButton(window, IDC_SESSION, BST_CHECKED);
            EnableWindow(GetDlgItem(window, IDC_REMEMBER), !request->program.empty());
            SetTimer(window, 1, 100, nullptr);
            SendMessageW(window, WM_TIMER, 1, 0);
            return TRUE;
        }
        if (!app) return FALSE;
        try {
            if (message == WM_TIMER && app->currentRequest_) {
                auto seconds = (std::max)(0LL, (app->currentRequest_->request.DeadlineUtc - NowUtc() + 9999999) / 10000000);
                std::wstring text = L"Automatically blocked in " + std::to_wstring(seconds) + L" seconds.";
                SetDlgItemTextW(window, IDC_COUNTDOWN, text.c_str());
                return TRUE;
            }
            if (message == WM_CLOSE) { app->PromptDecision(false, true); return TRUE; }
            if (message == WM_COMMAND) {
                if (LOWORD(wparam) == IDC_ALLOW) { app->PromptDecision(true); return TRUE; }
                if (LOWORD(wparam) == IDC_BLOCK || LOWORD(wparam) == IDCANCEL) { app->PromptDecision(false, LOWORD(wparam) == IDCANCEL); return TRUE; }
                if (LOWORD(wparam) == IDC_SESSION && IsDlgButtonChecked(window, IDC_SESSION) == BST_CHECKED)
                    CheckDlgButton(window, IDC_REMEMBER, BST_UNCHECKED);
                if (LOWORD(wparam) == IDC_REMEMBER && IsDlgButtonChecked(window, IDC_REMEMBER) == BST_CHECKED)
                    CheckDlgButton(window, IDC_SESSION, BST_UNCHECKED);
            }
        } catch (const std::exception& error) { Error(window, error); }
        return FALSE;
    }
    void DrainRows() {
        std::deque<LogRow> rows;
        { std::lock_guard lock(uiMutex_); rows.swap(incomingRows_); }
        // WM_SETREDRAW(TRUE) adds WS_VISIBLE. Never send it to a hidden page.
        const bool logVisible = (GetWindowLongPtrW(lists_[2], GWL_STYLE) & WS_VISIBLE) != 0;
        if (logVisible) SendMessageW(lists_[2], WM_SETREDRAW, FALSE, 0);
        for (const auto& row : rows) {
            const auto& event = row.event;
            wchar_t result[32]{}; swprintf_s(result, L"0x%08lX", static_cast<ULONG>(event.Status));
            Row(lists_[2], {Timestamp(event.Request.TimeUtc), std::to_wstring(event.Request.ProcessId),
                row.program.empty() ? L"Unknown / exited process" : row.program, row.file,
                OperationText(event.Request.Operation), event.Decision == SfmAllow ? L"Allow" : L"Deny",
                ReasonText(event.Reason), result, std::to_wstring(event.Request.RequestedBytes) + L" / " + std::to_wstring(event.TransferredBytes)}, 0);
            if (ListView_GetItemCount(lists_[2]) > 2000) ListView_DeleteItem(lists_[2], 2000);
        }
        if (logVisible) {
            SendMessageW(lists_[2], WM_SETREDRAW, TRUE, 0);
            RedrawWindow(lists_[2], nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        // Access events only change the log and counters, not settings or tabs.
        RECT area{}; GetClientRect(window_, &area);
        RECT counters{Scale(238), Scale(113), area.right - Scale(28), Scale(192)};
        InvalidateRect(window_, &counters, FALSE);
    }
    void Create() {
        Fonts();
        title_ = Control(L"STATIC", L"Secure File Monitor", 0);
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
        subtitle_ = Control(L"STATIC", L"", 0);
        start_ = Control(L"BUTTON", L"Start monitoring", WS_TABSTOP | BS_OWNERDRAW, ID_START);
        int tabIndex = 0;
        for (const auto* text : {L"Protected files", L"Access rules", L"Live access log", L"Pending requests", L"Options"}) {
            navigation_[tabIndex] = Control(L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, ID_NAV_FIRST + tabIndex);
            ++tabIndex;
        }
        for (int i = 0; i < 3; ++i) actions_[i] = Control(L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, ID_ACTION1 + i);
        for (int i = 0; i < 4; ++i) {
            lists_[i] = Control(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 400 + i);
            DWORD exStyle = LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP;
            if (i == 0) exStyle |= LVS_EX_CHECKBOXES;
            ListView_SetExtendedListViewStyle(lists_[i], exStyle);
            SetWindowTheme(lists_[i], L"Explorer", nullptr);
        }
        Columns(lists_[0], {{L"Enable", 100}, {L"Protected file", 560}, {L"Scope", 160}, {L"State", 120}});
        Columns(lists_[1], {{L"File", 370}, {L"Application", 370}, {L"Access", 150}, {L"Decision", 100}});
        Columns(lists_[2], {{L"Time (UTC)", 190}, {L"PID", 70}, {L"Program", 240}, {L"File", 300}, {L"Operation", 130},
            {L"Decision", 80}, {L"Reason", 180}, {L"NTSTATUS", 100}, {L"Bytes: requested / actual", 175}});
        Columns(lists_[3], {{L"Time left", 80}, {L"PID", 70}, {L"Program", 310}, {L"File", 370}, {L"Access", 150}});

        // Section borders are painted by the parent. Full-size group-box child
        // windows overlap the sibling controls and clip the parent's background.
        optionsHeading_ = Control(L"STATIC", L"Startup & Automation Options", SS_NOPREFIX);
        chkStartOnBoot_ = Control(L"BUTTON", L"Start application when Windows starts (at logon with admin privileges)",
            WS_TABSTOP | BS_AUTOCHECKBOX, ID_OPT_BOOT);
        chkAutoStart_ = Control(L"BUTTON", L"Start monitoring automatically when application launches",
            WS_TABSTOP | BS_AUTOCHECKBOX, ID_OPT_MONITOR);
        lblOptDesc_ = Control(L"STATIC",
            L"• Windows Startup: Automatically launches Secure File Monitor with administrator privileges on user logon.\n"
            L"• Auto-Start: Immediately initiates monitoring on launch if protected files are configured.",
            0);
        driverHeading_ = Control(L"STATIC", L"Driver Information & Mode", SS_NOPREFIX);
        lblDriverInfo_ = Control(L"STATIC", L"", 0);

        note_ = Control(L"STATIC", L"", 0);
        status_ = Control(L"STATIC", L"", 0);
        broker_.onNotification = [this](auto file, auto program, auto op) {
            Balloon(L"Protected file accessed (ProcMon)", op + L": " + file + (program.empty() ? L"" : (L"\nProgram: " + program)), NIIF_INFO);
        };
        broker_.onRequest = [this](auto request) {
            { std::lock_guard lock(uiMutex_); incomingRequests_.push_back(std::move(request)); }
            PostMessageW(window_, WM_REQUESTS, 0, 0);
        };
        broker_.onRows = [this](auto rows) {
            totalEvents_ += rows.size();
            for (const auto& row : rows) if (row.event.Decision == SfmDeny) ++deniedEvents_;
            bool notify;
            {
                std::lock_guard lock(uiMutex_); notify = incomingRows_.empty();
                for (auto& row : rows) { incomingRows_.push_back(std::move(row)); if (incomingRows_.size() > 2000) incomingRows_.pop_front(); }
            }
            if (notify) PostMessageW(window_, WM_ROWS, 0, 0);
        };
        broker_.onStatus = [this](auto message) {
            { std::lock_guard lock(uiMutex_); incomingStatus_ = std::move(message); }
            PostMessageW(window_, WM_STATUS, 0, 0);
        };
        RefreshSettings(); Layout(); SetTimer(window_, 1, 250, nullptr);
        const DWORD cornerPreference = 2; // DWMWCP_ROUND; ignored on Windows 10.
        DwmSetWindowAttribute(window_, 33, &cornerPreference, sizeof(cornerPreference));
        COLORREF caption = RGB(245, 247, 250);
        DwmSetWindowAttribute(window_, 35, &caption, sizeof(caption));

        if (settings_.autoStartMonitoring && !settings_.files.empty() && !broker_.IsMonitoring()) {
            try {
                Toggle();
            } catch (const std::exception& error) {
                UpdateStatusLine(L"Auto-start monitoring failed: " + Wide(error.what()));
            }
        }
    }
    void Paint() {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window_, &paint);
        RECT area{}; GetClientRect(window_, &area);
        FillRect(dc, &area, background_);
        RECT sidebar{0, 0, Scale(210), area.bottom}; FillRect(dc, &sidebar, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        HICON icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_MONITOR), IMAGE_ICON, Scale(40), Scale(40), LR_SHARED));
        DrawIconEx(dc, Scale(22), Scale(28), icon, Scale(40), Scale(40), 0, nullptr, DI_NORMAL);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(29, 43, 62)); SelectObject(dc, font_);
        RECT brand{Scale(72), Scale(29), Scale(194), Scale(78)};
        DrawTextW(dc, L"SECURE FILE\nMONITOR", -1, &brand, DT_LEFT | DT_NOPREFIX);
        RECT sidebarNote{Scale(23), area.bottom - Scale(94), Scale(188), area.bottom - Scale(16)};
        SetTextColor(dc, RGB(98, 110, 128));
        DrawTextW(dc, broker_.IsFaulted() ? L"DRIVER STATE UNKNOWN\nReconnect to recover\nFiles may stay blocked" :
            broker_.IsMonitoring() ? (broker_.IsProcMonMode() ?
                L"PROCMON ACTIVE\nOfficial signed driver\nNotification mode" :
                L"MONITORING ACTIVE\nLocal NTFS files\n20-second decisions") :
            L"MONITORING STOPPED\nWindows 10 / 11\nNative file protection", -1, &sidebarNote, DT_LEFT | DT_WORDBREAK);
        const std::wstring values[] = {std::to_wstring(settings_.files.size()), std::to_wstring(settings_.rules.size()), std::to_wstring(totalEvents_), std::to_wstring(deniedEvents_)};
        const wchar_t* labels[] = {L"Protected files", L"Access rules", L"Received events", L"Blocked accesses"};
        const int gap = Scale(12), left = Scale(238), cardWidth = (area.right - left - Scale(28) - 3 * gap) / 4;
        for (int i = 0; i < 4; ++i) {
            RECT card{left + i * (cardWidth + gap), Scale(113), left + i * (cardWidth + gap) + cardWidth, Scale(192)};
            Rounded(dc, card, Scale(9), RGB(255, 255, 255), RGB(228, 233, 241));
            RECT number{card.left + Scale(17), card.top + Scale(9), card.right - Scale(12), card.top + Scale(44)};
            SelectObject(dc, titleFont_); SetTextColor(dc, i == 3 ? RGB(173, 62, 58) : RGB(25, 58, 111));
            DrawTextW(dc, values[i].c_str(), -1, &number, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            RECT label{number.left, card.top + Scale(49), card.right - Scale(8), card.bottom - Scale(9)};
            SelectObject(dc, font_); SetTextColor(dc, RGB(98, 110, 128));
            DrawTextW(dc, labels[i], -1, &label, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        RECT panel{Scale(238), Scale(207), area.right - Scale(28), area.bottom - Scale(120)};
        Rounded(dc, panel, Scale(10), RGB(255, 255, 255), RGB(228, 233, 241));
        if (page_ == 4) {
            RECT options{Scale(254), Scale(273), area.right - Scale(44), Scale(433)};
            RECT driver{Scale(254), Scale(445), area.right - Scale(44), Scale(575)};
            Rounded(dc, options, Scale(6), RGB(255, 255, 255), RGB(215, 221, 231));
            Rounded(dc, driver, Scale(6), RGB(255, 255, 255), RGB(215, 221, 231));
        }
        EndPaint(window_, &paint);
    }
    void DrawButton(const DRAWITEMSTRUCT* item) {
        const bool selected = item->CtlID >= ID_NAV_FIRST && item->CtlID < ID_NAV_FIRST + 5 && static_cast<int>(item->CtlID) - ID_NAV_FIRST == page_;
        const bool navigation = item->CtlID >= ID_NAV_FIRST && item->CtlID < ID_NAV_FIRST + 5;
        const bool primary = item->CtlID == ID_START;
        const bool pressed = (item->itemState & ODS_SELECTED) != 0, disabled = (item->itemState & ODS_DISABLED) != 0;
        COLORREF fill = primary ? RGB(33, 96, 205) : selected ? RGB(231, 239, 253) : RGB(255, 255, 255);
        if (pressed) fill = primary ? RGB(24, 74, 161) : RGB(218, 228, 244);
        if (disabled) fill = RGB(246, 247, 249);
        COLORREF border = primary ? fill : navigation ? fill : RGB(215, 221, 231);
        FillRect(item->hDC, &item->rcItem, primary ? background_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        Rounded(item->hDC, item->rcItem, Scale(6), fill, border);
        SetBkMode(item->hDC, TRANSPARENT); SelectObject(item->hDC, font_);
        SetTextColor(item->hDC, disabled ? RGB(160, 167, 178) : primary ? RGB(255, 255, 255) : selected ? RGB(27, 79, 161) : RGB(40, 53, 71));
        RECT text = item->rcItem; if (navigation) text.left += Scale(16);
        DrawTextW(item->hDC, Text(item->hwndItem).c_str(), -1, &text,
            DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | (navigation ? DT_LEFT : DT_CENTER));
        if (item->itemState & ODS_FOCUS) { RECT focus = item->rcItem; InflateRect(&focus, -Scale(3), -Scale(3)); DrawFocusRect(item->hDC, &focus); }
    }
    void Exit() {
        // Explicit Exit disarms; merely closing the main window keeps the tray broker alive.
        if (broker_.IsMonitoring() || broker_.IsFaulted()) broker_.Pause();
        exiting_ = true; broker_.Shutdown();
        logger_.Diagnostic("session_end", totalEvents_, L"Application exited"); logger_.Flush();
        if (prompt_) { DestroyWindow(prompt_); prompt_ = nullptr; }
        DestroyWindow(window_);
    }
    LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == taskbarCreated_) { trayAdded_ = false; Tray(); return 0; }
        switch (message) {
        case WM_CREATE: Create(); return 0;
        case WM_SIZE: if (wparam != SIZE_MINIMIZED && title_) Layout(); return 0;
        case WM_DPICHANGED: {
            auto rect = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER);
            Fonts(); Layout(); return 0;
        }
        case WM_GETMINMAXINFO: {
            // The Options controls end at y=575; reserve the panel padding and
            // footer below them, including non-client borders at the current DPI.
            RECT minimum{0, 0, Scale(1030), Scale(711)};
            AdjustWindowRectExForDpi(&minimum, static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)),
                FALSE, static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)), GetDpiForWindow(window_));
            auto info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top}; return 0;
        }
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC: {
            HWND ctl = reinterpret_cast<HWND>(lparam);
            SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wparam), RGB(29, 43, 62));
            if (ctl == chkStartOnBoot_ || ctl == chkAutoStart_ || ctl == lblOptDesc_ ||
                ctl == optionsHeading_ || ctl == driverHeading_ || ctl == lblDriverInfo_) {
                return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
            }
            return reinterpret_cast<LRESULT>(background_);
        }
        // Paint() fills the entire invalid region with the final background.
        // A separate gray erase causes flashes behind the white Options panel.
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: Paint(); return 0;
        case WM_DRAWITEM: DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
        case WM_COMMAND:
            if (LOWORD(wparam) == ID_START || LOWORD(wparam) == ID_TRAY_TOGGLE) Toggle();
            else if (LOWORD(wparam) >= ID_ACTION1 && LOWORD(wparam) <= ID_ACTION3) Action(LOWORD(wparam) - ID_ACTION1);
            else if (LOWORD(wparam) == ID_TRAY_OPEN) Show();
            else if (LOWORD(wparam) == ID_TRAY_LOGS) OpenLogs();
            else if (LOWORD(wparam) == ID_TRAY_EXIT) Exit();
            else if (LOWORD(wparam) >= ID_NAV_FIRST && LOWORD(wparam) < ID_NAV_FIRST + 5) Page(LOWORD(wparam) - ID_NAV_FIRST);
            else if (LOWORD(wparam) == ID_OPT_BOOT) {
                bool enabled = (IsDlgButtonChecked(window_, ID_OPT_BOOT) == BST_CHECKED);
                Settings next = settings_;
                next.startOnBoot = enabled;
                try {
                    Save(std::move(next));
                    ConfigureWindowsStartup(enabled);
                } catch (...) { RefreshSettings(); throw; }
                UpdateStatusLine(enabled ? L"Configured to start at Windows startup." : L"Removed from Windows startup.");
            }
            else if (LOWORD(wparam) == ID_OPT_MONITOR) {
                bool enabled = (IsDlgButtonChecked(window_, ID_OPT_MONITOR) == BST_CHECKED);
                Settings next = settings_;
                next.autoStartMonitoring = enabled;
                try { Save(std::move(next)); }
                catch (...) { RefreshSettings(); throw; }
                UpdateStatusLine(enabled ? L"Auto-start monitoring enabled." : L"Auto-start monitoring disabled.");
            }
            return 0;
        case WM_NOTIFY: {
            auto notification = reinterpret_cast<NMHDR*>(lparam);
            if (notification->code == LVN_GETEMPTYMARKUP && notification->idFrom >= 400 && notification->idFrom <= 403) {
                auto empty = reinterpret_cast<NMLVEMPTYMARKUP*>(lparam); empty->dwFlags = EMF_CENTERED;
                const wchar_t* messages[] = {L"No protected files yet. Add a file to get started.",
                    L"No rules yet. Unmatched access will ask for your decision.",
                    L"No access events received. Start monitoring to see real file activity.",
                    L"You're all caught up. No access requests are waiting."};
                wcscpy_s(empty->szMarkup, messages[notification->idFrom - 400]); return TRUE;
            }
            if (notification->code == LVN_ITEMCHANGED && notification->idFrom == 400 && !updatingList_) {
                auto nmlv = reinterpret_cast<NMLISTVIEW*>(lparam);
                if ((nmlv->uChanged & LVIF_STATE) &&
                    ((nmlv->uNewState & LVIS_STATEIMAGEMASK) != (nmlv->uOldState & LVIS_STATEIMAGEMASK)) &&
                    ((nmlv->uOldState & LVIS_STATEIMAGEMASK) != 0) &&
                    ((nmlv->uNewState & LVIS_STATEIMAGEMASK) != 0)) {
                    int index = nmlv->iItem;
                    if (index >= 0 && index < static_cast<int>(settings_.files.size())) {
                        if (broker_.IsMonitoring()) {
                            updatingList_ = true;
                            ListView_SetCheckState(lists_[0], index, settings_.files[index].enabled ? TRUE : FALSE);
                            updatingList_ = false;
                            MessageBoxW(window_, L"Stop monitoring before changing enabled files.", L"Secure File Monitor", MB_OK | MB_ICONWARNING);
                            return 0;
                        }
                        bool checked = ListView_GetCheckState(lists_[0], index) != 0;
                        if (settings_.files[index].enabled != checked) {
                            settings_.files[index].enabled = checked;
                            Save(settings_);
                        }
                    }
                }
            }
            if (notification->code == NM_DBLCLK && notification->idFrom == 400) Action(2);
            if (notification->code == NM_DBLCLK && notification->idFrom == 401) Action(1);
            if (notification->code == NM_DBLCLK && notification->idFrom == 403) Action(0);
            return 0;
        }
        case WM_ROWS: DrainRows(); return 0;
        case WM_REQUESTS: {
            std::deque<std::shared_ptr<PendingRequest>> requests;
            { std::lock_guard lock(uiMutex_); requests.swap(incomingRequests_); }
            for (const auto& request : requests) if (!request->completed) requests_.push_back(request);
            if (!requests.empty()) {
                auto request = requests.back();
                Balloon(L"File access needs a decision", OperationText(request->request.Operation) + L": " + request->file);
            }
            RefreshRequests(); return 0;
        }
        case WM_STATUS: {
            std::wstring status; { std::lock_guard lock(uiMutex_); status = incomingStatus_; }
            RefreshSettings(); UpdateStatusLine(status); return 0;
        }
        case WM_TIMER: RefreshRequests(); return 0;
        case WM_TRAY:
            if (LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT || LOWORD(lparam) == NIN_BALLOONUSERCLICK || LOWORD(lparam) == WM_LBUTTONDBLCLK) {
                Show(); if (!requests_.empty()) { Page(3); ShowPrompt(requests_.front()); }
            } else if (LOWORD(lparam) == WM_CONTEXTMENU) {
                POINT point{}; GetCursorPos(&point); HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open Secure File Monitor");
                AppendMenuW(menu, MF_STRING, ID_TRAY_TOGGLE, broker_.IsMonitoring() ? L"Stop monitoring" : L"Start monitoring");
                AppendMenuW(menu, MF_STRING, ID_TRAY_LOGS, L"Open logs folder"); AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit"); SetForegroundWindow(window_);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr); DestroyMenu(menu);
                PostMessageW(window_, WM_NULL, 0, 0);
            }
            return 0;
        case WM_CLOSE:
            if (!trayAdded_) Exit();
            else { ShowWindow(window_, SW_HIDE); Balloon(L"Secure File Monitor", L"Still available in the notification area. Use Exit in the tray menu to quit.", NIIF_INFO); }
            return 0;
        case WM_QUERYENDSESSION: broker_.Shutdown(); return TRUE;
        case WM_ENDSESSION: if (wparam) { exiting_ = true; DestroyWindow(window_); } return 0;
        case WM_DESTROY: {
            NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.uID = 1; Shell_NotifyIconW(NIM_DELETE, &data);
            PostQuitMessage(0); return 0;
        }
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, message, wparam, lparam);
        try { return app->Message(message, wparam, lparam); }
        catch (const std::exception& error) { Error(window, error); return 0; }
    }
public:
    explicit App(HINSTANCE instance) : instance_(instance), data_(DataDirectory()), config_(data_ / L"settings.sfm") {
        logger_.Open(data_ / L"Logs");
        try {
            settings_ = LoadSettings(config_);
            settings_.startOnBoot = settings_.startOnBoot || IsWindowsStartupEnabled();
            size_t prevCount = settings_.files.size();
            EnsureDefaultProtectedFiles(settings_);
            if (settings_.files.size() != prevCount) {
                SaveSettings(config_, settings_);
            }
        }
        catch (const std::exception& error) { configHealthy_ = false; Error(nullptr, error); }
    }
    ~App() {
        broker_.Shutdown(); if (font_) DeleteObject(font_); if (titleFont_) DeleteObject(titleFont_); DeleteObject(background_);
    }
    int Run(int show) {
        WNDCLASSEXW klass{sizeof(klass)}; klass.lpfnWndProc = WindowProc; klass.hInstance = instance_;
        klass.lpszClassName = L"SecureFileMonitor.MainWindow"; klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_MONITOR)); klass.hIconSm = klass.hIcon;
        if (!RegisterClassExW(&klass)) throw std::runtime_error("Cannot register main window.");
        if (!CreateWindowExW(0, klass.lpszClassName, L"Secure File Monitor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1240, 800, nullptr, nullptr, instance_, this))
            throw std::runtime_error("Cannot create main window.");
        ShowWindow(window_, show); UpdateWindow(window_);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (prompt_ && IsDialogMessageW(prompt_, &message)) continue;
            if (IsDialogMessageW(window_, &message)) continue;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }
};
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    Handle singleton(CreateMutexW(nullptr, TRUE, L"Local\\SecureFileMonitor.UI"));
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"SecureFileMonitor.MainWindow", nullptr);
        if (existing) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES}; InitCommonControlsEx(&controls);
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput graphicsInput; ULONG_PTR graphicsToken = 0;
    if (Gdiplus::GdiplusStartup(&graphicsToken, &graphicsInput, nullptr) != Gdiplus::Ok) return 1;
    int result = 1;
    try { App app(instance); result = app.Run(show); } catch (const std::exception& error) { Error(nullptr, error); }
    if (SUCCEEDED(com)) CoUninitialize();
    Gdiplus::GdiplusShutdown(graphicsToken);
    return result;
}
