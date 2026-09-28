#pragma once
#include "Core.h"
#include <functional>
#include <vector>
#include <string>
#include <memory>

namespace sfm {

class ProcMonMonitor {
public:
    using EventCallback = std::function<void(
        const std::wstring& file,
        DWORD pid,
        const std::wstring& program,
        ULONG operation,
        LONG status)>;

    ProcMonMonitor();
    ~ProcMonMonitor();

    bool Start(const std::vector<ProtectedFile>& files, EventCallback callback);
    void Stop();
    bool IsActive() const { return active_; }
    void UpdateFiles(const std::vector<ProtectedFile>& files);

    class Impl;
private:
    std::unique_ptr<Impl> impl_;
    bool active_ = false;
};

} // namespace sfm
