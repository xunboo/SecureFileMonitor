#include "Broker.h"
#include <stdexcept>

namespace sfm {
struct ReceivedMessage { FILTER_MESSAGE_HEADER header; SfmRequest request; };
struct ReplyMessage { FILTER_REPLY_HEADER header; SfmReply reply; };
static_assert(offsetof(ReceivedMessage, request) == 16);
static_assert(offsetof(ReplyMessage, reply) == 16);

Broker::~Broker() { Shutdown(); }
void Broker::Configure(const Settings& settings, bool enabled) {
    auto config = std::make_unique<SfmConfiguration>();
    config->Header = {SFM_PROTOCOL_VERSION, SfmConfigure};
    config->Enabled = enabled ? 1u : 0u;
    config->FileCount = static_cast<ULONG>(settings.files.size());
    if (config->FileCount > SFM_MAX_FILES) throw std::runtime_error("Too many protected files.");
    for (size_t i = 0; i < settings.files.size(); ++i) {
        if (settings.files[i].ntPath.empty() || settings.files[i].ntPath.size() >= SFM_MAX_PATH)
            throw std::runtime_error("A protected file has not been resolved.");
        wcscpy_s(config->Paths[i], settings.files[i].ntPath.c_str());
    }
    DWORD returned = 0;
    std::lock_guard lock(sendMutex_);
    HRESULT hr = FilterSendMessage(port_.get(), config.get(), sizeof(*config), nullptr, 0, &returned);
    if (FAILED(hr)) throw std::runtime_error("Driver configuration failed: " + Utf8(ErrorText(HRESULT_CODE(hr))));
}
void Broker::Start(const Settings& settings) {
    if (monitoring_) return;
    if (!IsAdministrator()) throw std::runtime_error(
        "Configuration is available without elevation. To intercept file access, close this app "
        "using Exit in the tray menu, then launch SecureFileMonitor.App.exe using 'Run as administrator'.");
    if (settings.files.empty()) throw std::runtime_error("Add at least one file before starting monitoring.");
    if (!connected_) {
        if (receiver_.joinable() || poller_.joinable()) Shutdown();
        HANDLE raw = INVALID_HANDLE_VALUE;
        HRESULT hr = FilterConnectCommunicationPort(SFM_PORT_NAME, 0, nullptr, 0, nullptr, &raw);
        if (FAILED(hr)) throw std::runtime_error(
            "The SecureFileMonitor driver is not loaded or its port is unavailable.\n\n"
            "Build and install the WDK driver, run 'fltmc load SecureFileMonitor' as administrator, "
            "then click Start monitoring. No file access is being intercepted.\n\n" + Utf8(ErrorText(HRESULT_CODE(hr))));
        port_.reset(raw);
        stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stopEvent_) { port_.reset(); throw std::runtime_error("Cannot create broker stop event."); }
        // Recover a fail-closed configuration left by a crashed broker before
        // resolving files. Our process is now exempt, and this acknowledgement
        // establishes a known stopped state even if later path resolution fails.
        try { Configure({}, false); }
        catch (...) { port_.reset(); stopEvent_.reset(); throw; }
        connected_ = true; faulted_ = false;
        try {
            receiver_ = std::thread(&Broker::ReceiveLoop, this);
            poller_ = std::thread(&Broker::PollLoop, this);
        } catch (...) { Shutdown(); throw; }
    }
    Settings resolved = settings;
    for (auto& file : resolved.files) file.ntPath = ResolveFile(file.path).ntPath;
    {
        std::lock_guard lock(stateMutex_);
        settings_ = resolved; permissions_.clear();
    }
    // Set before enabling the kernel so the first request cannot race the UI state.
    monitoring_ = true;
    try { Configure(resolved, true); }
    catch (...) { monitoring_ = false; throw; }
    try { logger_.Diagnostic("monitoring_started", resolved.files.size(), L"Monitoring selected local NTFS paths"); }
    catch (...) {
        try { Pause(); } catch (...) { /* Pause disarms before trying to write its log. */ }
        throw;
    }
    if (onStatus) onStatus(L"Monitoring is active. Unanswered requests are denied after 20 seconds.");
}
void Broker::Pause() {
    if (!port_) { monitoring_ = false; return; }
    Settings snapshot;
    {
        std::lock_guard lock(stateMutex_); snapshot = settings_;
    }
    // Do not claim monitoring is stopped until the kernel acknowledges it.
    Configure(snapshot, false);
    monitoring_ = false; faulted_ = false;
    std::vector<std::shared_ptr<PendingRequest>> requests;
    {
        std::lock_guard lock(stateMutex_);
        for (const auto& [id, request] : pending_) { (void)id; requests.push_back(request); }
        permissions_.clear();
    }
    for (const auto& request : requests) SendReply(request, false, SfmStopping);
    logger_.Diagnostic("monitoring_stopped", 0, L"New accesses are no longer intercepted; draining completion logs");
    if (onStatus) onStatus(L"Monitoring stopped. The driver no longer intercepts selected files.");
}
void Broker::Shutdown() {
    if (!port_) return;
    try { Pause(); } catch (...) { /* A broken connection leaves configured paths fail-closed. */ }
    if (stopEvent_) SetEvent(stopEvent_.get());
    CancelIoEx(port_.get(), nullptr);
    if (receiver_.joinable()) receiver_.join();
    if (poller_.joinable()) poller_.join();
    {
        std::lock_guard lock(sendMutex_); port_.reset();
    }
    stopEvent_.reset(); connected_ = false; monitoring_ = false;
    std::lock_guard lock(stateMutex_);
    for (const auto& [id, request] : pending_) { (void)id; request->completed = true; }
    pending_.clear(); permissions_.clear();
}
void Broker::UpdateRules(const std::vector<Rule>& rules) {
    std::lock_guard lock(stateMutex_); settings_.rules = rules;
    permissions_.clear(); // A rule edit revokes temporary process permissions.
}
std::wstring Broker::ResolveProcess(ULONG pid, ULONGLONG created) {
    const auto key = std::make_pair(pid, created);
    {
        std::lock_guard lock(stateMutex_);
        auto it = processes_.find(key);
        if (it != processes_.end()) return it->second;
    }
    auto path = ProcessPath(pid, created);
    if (!path.empty()) {
        std::lock_guard lock(stateMutex_);
        if (processes_.size() >= 4096) processes_.erase(processes_.begin());
        processes_[key] = path;
    }
    return path;
}
bool Broker::SendReply(const std::shared_ptr<PendingRequest>& request, bool allow, ULONG reason) {
    if (request->completed.exchange(true)) return false;
    ReplyMessage message{};
    message.header.MessageId = request->messageId;
    message.header.Status = 0;
    message.reply = {allow ? SfmAllow : SfmDeny, reason};
    HRESULT hr;
    {
        std::lock_guard lock(sendMutex_);
        hr = FilterReplyMessage(port_.get(), &message.header,
            sizeof(FILTER_REPLY_HEADER) + sizeof(SfmReply));
    }
    {
        std::lock_guard lock(stateMutex_); pending_.erase(request->messageId);
    }
    if (FAILED(hr) && onStatus)
        onStatus(L"A decision arrived after the driver deadline or after disconnect. It was not applied.");
    return SUCCEEDED(hr);
}
bool Broker::Decide(const std::shared_ptr<PendingRequest>& request, bool allow, bool processSession) {
    if (!request || request->completed || !monitoring_) return false;
    if (NowUtc() >= request->request.DeadlineUtc) {
        SendReply(request, false, SfmTimeout); return false;
    }
    if (processSession && request->request.ProcessCreatedUtc) {
        std::lock_guard lock(stateMutex_);
        if (permissions_.size() >= 4096) permissions_.erase(permissions_.begin());
        permissions_.push_back({request->request.ProcessId, request->request.ProcessCreatedUtc,
            request->file, request->request.Operation & (SfmRead | SfmWrite), allow, request->messageId});
    }
    // Publish before releasing the blocked open; the next read can arrive immediately.
    if (!SendReply(request, allow, SfmUser)) {
        std::lock_guard lock(stateMutex_);
        std::erase_if(permissions_, [&](const auto& permission) { return permission.token == request->messageId; });
        return false;
    }
    return true;
}
void Broker::Expire(const std::shared_ptr<PendingRequest>& request) { SendReply(request, false, SfmTimeout); }
void Broker::HandleRequest(ULONGLONG id, const SfmRequest& data) {
    auto request = std::make_shared<PendingRequest>();
    request->request = data; request->messageId = id;
    request->program = ResolveProcess(data.ProcessId, data.ProcessCreatedUtc);
    Evaluation result;
    bool found = false;
    {
        std::lock_guard lock(stateMutex_);
        for (const auto& file : settings_.files) {
            if (MatchesFile(data.Path, file.ntPath)) { request->file = file.path; found = true; break; }
        }
        result = Evaluate(settings_.rules, request->file, request->program, data.Operation);
        if (result.action == Action::Ask) {
            // Most recent session choice takes precedence over earlier session choices.
            for (auto it = permissions_.rbegin(); it != permissions_.rend(); ++it) {
                if (it->pid == data.ProcessId && it->created == data.ProcessCreatedUtc &&
                    EqualPath(it->file, request->file) &&
                    (it->operations & (data.Operation & 3)) == (data.Operation & 3)) {
                    result = {it->allow ? Action::Allow : Action::Deny, L"Process session permission"}; break;
                }
            }
        }
        pending_[id] = request;
    }
    if (!monitoring_ || !found) { SendReply(request, false, SfmStopping); return; }
    if (NowUtc() >= data.DeadlineUtc) { SendReply(request, false, SfmTimeout); return; }
    if (result.action != Action::Ask) {
        SendReply(request, result.action == Action::Allow,
            result.explanation == L"Process session permission" ? SfmSessionRule : SfmRule);
        return;
    }
    request->explanation = result.explanation;
    if (onRequest) onRequest(request);
    else SendReply(request, false, SfmDisconnected);
}
void Broker::Fault(const std::wstring& message) {
    monitoring_ = false;
    connected_ = false;
    faulted_ = true;
    if (onStatus) onStatus(L"Connection/logging fault: " + message +
        L" Configured files may remain blocked. Reconnect or unload the driver to recover.");
    if (stopEvent_) SetEvent(stopEvent_.get());
}
void Broker::ReceiveLoop() {
    try {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) throw std::runtime_error("Cannot create receive event");
        while (WaitForSingleObject(stopEvent_.get(), 0) == WAIT_TIMEOUT) {
            ReceivedMessage message{}; OVERLAPPED overlapped{};
            ResetEvent(event.get()); overlapped.hEvent = event.get();
            HRESULT hr = FilterGetMessage(port_.get(), &message.header, sizeof(message), &overlapped);
            if (hr == HRESULT_FROM_WIN32(ERROR_IO_PENDING)) {
                HANDLE waits[] = {stopEvent_.get(), event.get()};
                DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait != WAIT_OBJECT_0 + 1) CancelIoEx(port_.get(), &overlapped);
                DWORD transferred = 0;
                if (!GetOverlappedResult(port_.get(), &overlapped, &transferred, TRUE)) {
                    if (WaitForSingleObject(stopEvent_.get(), 0) == WAIT_OBJECT_0) break;
                    throw std::runtime_error(Utf8(ErrorText()));
                }
                if (transferred != sizeof(message)) throw std::runtime_error("Unexpected driver message size");
            } else if (FAILED(hr)) throw std::runtime_error(Utf8(ErrorText(HRESULT_CODE(hr))));
            if (message.request.Version != SFM_PROTOCOL_VERSION ||
                message.header.ReplyLength != sizeof(ReplyMessage) ||
                !wmemchr(message.request.Path, L'\0', SFM_MAX_PATH))
                throw std::runtime_error("Incompatible driver communication protocol");
            HandleRequest(message.header.MessageId, message.request);
        }
    } catch (const std::exception& error) { Fault(Wide(error.what())); }
}
bool Broker::PollEvents() {
    SfmCommandHeader command{SFM_PROTOCOL_VERSION, SfmPoll};
    auto batch = std::make_unique<SfmPollResult>(); DWORD bytes = 0; HRESULT hr;
    {
        std::lock_guard lock(sendMutex_);
        hr = FilterSendMessage(port_.get(), &command, sizeof(command), batch.get(), sizeof(*batch), &bytes);
    }
    if (FAILED(hr)) throw std::runtime_error("Reading driver events failed: " + Utf8(ErrorText(HRESULT_CODE(hr))));
    if (bytes != sizeof(*batch) || batch->Version != SFM_PROTOCOL_VERSION || batch->Count > SFM_EVENT_BATCH)
        throw std::runtime_error("Invalid event batch from driver");
    if (batch->DroppedEvents != dropped_) {
        logger_.Diagnostic("lost_events", batch->DroppedEvents >= dropped_ ? batch->DroppedEvents - dropped_ : batch->DroppedEvents,
            L"Driver ring buffer overflow/resource failure; audit is incomplete");
        dropped_ = batch->DroppedEvents;
        if (onStatus) onStatus(L"Audit gap: the driver event buffer overflowed. See the session log for lost-event counts.");
    }
    if (batch->NameFailures != nameFailures_) {
        logger_.Diagnostic("name_lookup_failures", batch->NameFailures >= nameFailures_ ? batch->NameFailures - nameFailures_ : batch->NameFailures,
            L"Unresolved I/O names bypassed path matching");
        nameFailures_ = batch->NameFailures;
    }
    std::vector<LogRow> rows;
    for (ULONG i = 0; i < batch->Count; ++i) {
        if (!wmemchr(batch->Events[i].Request.Path, L'\0', SFM_MAX_PATH))
            throw std::runtime_error("Unterminated path in driver event");
        LogRow row; row.event = batch->Events[i]; row.file = row.event.Request.Path;
        {
            std::lock_guard lock(stateMutex_);
            for (const auto& file : settings_.files) {
                if (MatchesFile(row.file, file.ntPath)) {
                    row.file = file.path + row.file.substr(file.ntPath.size()); break;
                }
            }
        }
        row.program = ResolveProcess(row.event.Request.ProcessId, row.event.Request.ProcessCreatedUtc);
        logger_.Append(row); rows.push_back(std::move(row));
    }
    if (!rows.empty() && onRows) onRows(std::move(rows));
    return batch->Count == SFM_EVENT_BATCH;
}
void Broker::PollLoop() {
    try {
        ULONGLONG lastFlush = GetTickCount64();
        while (WaitForSingleObject(stopEvent_.get(), 75) == WAIT_TIMEOUT) {
            // Bound each burst so deadline handling still runs under sustained I/O.
            for (int i = 0; i < 16 && PollEvents(); ++i) {}
            std::vector<std::shared_ptr<PendingRequest>> expired;
            {
                std::lock_guard lock(stateMutex_);
                for (const auto& [id, request] : pending_) {
                    (void)id;
                    if (request->request.DeadlineUtc - NowUtc() <= 2500000) expired.push_back(request);
                }
            }
            for (const auto& request : expired) SendReply(request, false, SfmTimeout);
            if (GetTickCount64() - lastFlush >= 1000) { logger_.Flush(); lastFlush = GetTickCount64(); }
        }
        // Capture denials and completions already queued during orderly shutdown.
        for (int i = 0; i < 100 && PollEvents(); ++i) {}
        logger_.Flush();
    } catch (const std::exception& error) { Fault(Wide(error.what())); }
}
}
