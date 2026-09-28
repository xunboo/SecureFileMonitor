#pragma once
#include "Support.h"
#include <fltuser.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <thread>

namespace sfm {
struct PendingRequest {
    SfmRequest request{};
    ULONGLONG messageId = 0;
    std::wstring file;
    std::wstring program;
    std::wstring explanation;
    std::atomic<bool> completed{false};
};
class Broker {
    Logger& logger_;
    Handle port_, stopEvent_;
    std::thread receiver_, poller_;
    std::atomic<bool> connected_{false}, monitoring_{false}, faulted_{false};
    std::mutex stateMutex_, sendMutex_;
    Settings settings_;
    struct SessionPermission { ULONG pid; ULONGLONG created; std::wstring file; ULONG operations; bool allow; ULONGLONG token; };
    std::vector<SessionPermission> permissions_;
    std::map<ULONGLONG, std::shared_ptr<PendingRequest>> pending_;
    std::map<std::pair<ULONG, ULONGLONG>, std::wstring> processes_;
    ULONGLONG dropped_ = 0, nameFailures_ = 0;
    void ReceiveLoop();
    void PollLoop();
    bool PollEvents();
    void HandleRequest(ULONGLONG id, const SfmRequest& request);
    bool SendReply(const std::shared_ptr<PendingRequest>& request, bool allow, ULONG reason);
    std::wstring ResolveProcess(ULONG pid, ULONGLONG created);
    void Configure(const Settings& settings, bool enabled);
    void Fault(const std::wstring& message);
public:
    std::function<void(std::shared_ptr<PendingRequest>)> onRequest;
    std::function<void(std::vector<LogRow>)> onRows;
    std::function<void(std::wstring)> onStatus;
    explicit Broker(Logger& logger) : logger_(logger) {}
    ~Broker();
    void Start(const Settings& settings);
    void Pause();
    void Shutdown();
    void UpdateRules(const std::vector<Rule>& rules);
    bool Decide(const std::shared_ptr<PendingRequest>& request, bool allow, bool processSession);
    void Expire(const std::shared_ptr<PendingRequest>& request);
    bool IsMonitoring() const { return monitoring_; }
    bool IsConnected() const { return connected_; }
    bool IsFaulted() const { return faulted_; }
};
}
