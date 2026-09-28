#include "ProcMonMonitor.h"
#include "Support.h"
#include <mutex>

#pragma warning(push)
#pragma warning(disable: 4100 4244 4324 4458 4996)
#include "../sdk/procmonsdk/sdk.hpp"
#pragma warning(pop)

namespace sfm {

class SfmProcMonCallback : public IEventCallback {
    ProcMonMonitor::Impl* owner_;
public:
    explicit SfmProcMonCallback(ProcMonMonitor::Impl* owner) : owner_(owner) {}
    BOOL DoEvent(const CRefPtr<CEventView> pEventView) override;
};

class ProcMonMonitor::Impl {
    std::mutex mutex_;
    std::vector<ProtectedFile> files_;
    EventCallback callback_;
    bool callbackRegistered_ = false;
    bool running_ = false;

public:
    bool Start(const std::vector<ProtectedFile>& files, EventCallback callback) {
        std::lock_guard lock(mutex_);
        files_ = files;
        callback_ = callback;

        CDrvLoader& drvLoader = Singleton<CDrvLoader>::getInstance();
        if (!drvLoader.IsReady()) {
            TCHAR sysDir[MAX_PATH] = {0};
            GetSystemDirectory(sysDir, MAX_PATH);
            std::wstring drv25 = std::wstring(sysDir) + L"\\drivers\\PROCMON25.SYS";
            if (PathFileExistsW(drv25.c_str())) {
                drvLoader.Init(TEXT("PROCMON25"), TEXT("PROCMON25.SYS"));
            } else {
                drvLoader.Init(TEXT("PROCMON24"), TEXT("PROCMON24.SYS"));
            }
        }

        CMonitorContoller& monctl = Singleton<CMonitorContoller>::getInstance();
        if (!monctl.Connect()) {
            return false;
        }

        if (!callbackRegistered_) {
            CEventMgr& eventMgr = Singleton<CEventMgr>::getInstance();
            eventMgr.RegisterCallback(new SfmProcMonCallback(this));
            callbackRegistered_ = true;
        }

        monctl.SetMonitor(TRUE, TRUE, FALSE);
        if (!monctl.Start()) {
            return false;
        }

        running_ = true;
        return true;
    }

    void Stop() {
        std::lock_guard lock(mutex_);
        if (running_) {
            CMonitorContoller& monctl = Singleton<CMonitorContoller>::getInstance();
            monctl.Stop();
            running_ = false;
        }
    }

    void UpdateFiles(const std::vector<ProtectedFile>& files) {
        std::lock_guard lock(mutex_);
        files_ = files;
    }

    bool IsRunning() const {
        return running_;
    }

    void OnEvent(const CRefPtr<CEventView>& eventView) {
        if (eventView.IsNull() || !running_) return;

        CString eventPathStr = eventView->GetPath();
        if (eventPathStr.IsEmpty()) return;
        std::wstring eventPath = (LPCTSTR)eventPathStr;

        std::wstring matchedFile;
        {
            std::lock_guard lock(mutex_);
            for (const auto& f : files_) {
                if (MatchesFile(eventPath, f.path) || MatchesFile(eventPath, f.ntPath) ||
                    (!f.path.empty() && EqualPath(eventPath, f.path)) ||
                    (!f.ntPath.empty() && EqualPath(eventPath, f.ntPath))) {
                    matchedFile = f.path;
                    break;
                }
            }
        }
        if (matchedFile.empty()) return;

        DWORD pid = eventView->GetProcessId();
        CString imgPath = eventView->GetImagePath();
        std::wstring program = !imgPath.IsEmpty() ? (LPCTSTR)imgPath : (LPCTSTR)eventView->GetProcessName();

        PLOG_ENTRY pre = eventView->GetPreEventEntry();
        CString opName = StrMapOperation(pre);
        CString opLower = opName;
        opLower.MakeLower();

        ULONG op = SfmOpen;
        if (opLower.Find(_T("read")) >= 0) op |= SfmRead;
        if (opLower.Find(_T("write")) >= 0 || opLower.Find(_T("setinfo")) >= 0 || opLower.Find(_T("append")) >= 0) op |= SfmWrite;

        NTSTATUS status = eventView->GetResult();

        EventCallback cb;
        {
            std::lock_guard lock(mutex_);
            cb = callback_;
        }
        if (cb) {
            cb(matchedFile, pid, program, op, status);
        }
    }
};

BOOL SfmProcMonCallback::DoEvent(const CRefPtr<CEventView> pEventView) {
    if (owner_) {
        owner_->OnEvent(pEventView);
    }
    return TRUE;
}

ProcMonMonitor::ProcMonMonitor() : impl_(std::make_unique<Impl>()) {}
ProcMonMonitor::~ProcMonMonitor() { Stop(); }

bool ProcMonMonitor::Start(const std::vector<ProtectedFile>& files, EventCallback callback) {
    if (impl_->Start(files, callback)) {
        active_ = true;
        return true;
    }
    active_ = false;
    return false;
}

void ProcMonMonitor::Stop() {
    active_ = false;
    if (impl_) impl_->Stop();
}

void ProcMonMonitor::UpdateFiles(const std::vector<ProtectedFile>& files) {
    if (impl_) impl_->UpdateFiles(files);
}

} // namespace sfm
