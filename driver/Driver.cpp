#include <fltKernel.h>
#include <ntstrsafe.h>
#include "../shared/Protocol.h"

// This is a development minifilter. See docs/driver-validation.md before loading.
// No C++ runtime, exceptions, STL, or static constructors are used in the kernel.
#define SFM_TAG 'mFSS'

struct OperationContext { SfmEvent Event; };
static PFLT_FILTER gFilter;
static PFLT_PORT gServerPort;
static PFLT_PORT gClientPort;
static PEPROCESS gBrokerProcess;
static EX_PUSH_LOCK gConfigurationLock;
static SfmConfiguration* gConfiguration;
static volatile LONG gEnabled;
static volatile LONG gStopping;
static volatile LONG gPending;
static volatile LONG64 gSequence;
static volatile LONG64 gDropped;
static volatile LONG64 gNameFailures;
static volatile LONG64 gBypassed;
static KSPIN_LOCK gEventLock;
static SfmEvent* gEvents;
static ULONG gEventHead;
static ULONG gEventCount;

static void LockShared() { KeEnterCriticalRegion(); ExAcquirePushLockShared(&gConfigurationLock); }
static void UnlockShared() { ExReleasePushLockShared(&gConfigurationLock); KeLeaveCriticalRegion(); }
static void LockExclusive() { KeEnterCriticalRegion(); ExAcquirePushLockExclusive(&gConfigurationLock); }
static void UnlockExclusive() { ExReleasePushLockExclusive(&gConfigurationLock); KeLeaveCriticalRegion(); }

static void LogEvent(_In_ const SfmEvent* event) {
    KIRQL oldIrql;
    KeAcquireSpinLock(&gEventLock, &oldIrql);
    if (gEventCount == SFM_EVENT_CAPACITY) {
        InterlockedIncrement64(&gDropped);
    } else {
        ULONG index = (gEventHead + gEventCount) % SFM_EVENT_CAPACITY;
        RtlCopyMemory(&gEvents[index], event, sizeof(*event));
        ++gEventCount;
    }
    KeReleaseSpinLock(&gEventLock, oldIrql);
}
static void FinishBlocked(_Inout_ PFLT_CALLBACK_DATA data, _Inout_ OperationContext* context, ULONG reason) {
    data->IoStatus.Status = STATUS_ACCESS_DENIED;
    data->IoStatus.Information = 0;
    context->Event.Decision = SfmDeny;
    context->Event.Reason = reason;
    context->Event.Status = STATUS_ACCESS_DENIED;
    context->Event.Phase = SfmBlocked;
    LARGE_INTEGER time; KeQuerySystemTime(&time);
    context->Event.CompletedUtc = time.QuadPart;
    LogEvent(&context->Event);
}
static BOOLEAN IsMonitored(_In_ PCUNICODE_STRING name) {
    BOOLEAN matched = FALSE;
    LockShared();
    if (gConfiguration && gConfiguration->Enabled) {
        for (ULONG i = 0; i < gConfiguration->FileCount; ++i) {
            UNICODE_STRING target; RtlInitUnicodeString(&target, gConfiguration->Paths[i]);
            if (RtlEqualUnicodeString(name, &target, TRUE) ||
                (name->Length > target.Length && name->Buffer[target.Length / sizeof(WCHAR)] == L':' &&
                 RtlPrefixUnicodeString(&target, name, TRUE))) {
                matched = TRUE; break;
            }
        }
    }
    UnlockShared();
    return matched;
}

static VOID NTAPI DecideOperation(_In_ PFLT_DEFERRED_IO_WORKITEM workItem,
    _In_ PFLT_CALLBACK_DATA data, _In_opt_ PVOID rawContext) {
    auto context = static_cast<OperationContext*>(rawContext);
    SfmReply reply{SfmDeny, SfmDisconnected};
    ULONG replyLength = sizeof(reply);
    LARGE_INTEGER deadline; deadline.QuadPart = context->Event.Request.DeadlineUtc;
    NTSTATUS status = STATUS_PORT_DISCONNECTED;
    if (!InterlockedCompareExchange(&gStopping, 0, 0)) {
        status = FltSendMessage(gFilter, &gClientPort, &context->Event.Request,
            sizeof(SfmRequest), &reply, &replyLength, &deadline);
    }
    ULONG reason = reply.Reason;
    BOOLEAN allow = FALSE;
    // STATUS_TIMEOUT is an NT_SUCCESS status: do not use NT_SUCCESS here.
    if (status == STATUS_SUCCESS && replyLength == sizeof(reply) &&
        reply.Decision <= SfmAllow &&
        (reply.Reason == SfmRule || reply.Reason == SfmUser || reply.Reason == SfmTimeout ||
         reply.Reason == SfmStopping || reply.Reason == SfmSessionRule)) {
        allow = reply.Decision == SfmAllow && reply.Reason != SfmTimeout && reply.Reason != SfmStopping;
    } else if (status == STATUS_TIMEOUT) reason = SfmTimeout;
    else if (status == STATUS_PORT_DISCONNECTED) reason = SfmDisconnected;
    else reason = SfmInvalidReply;
    InterlockedDecrement(&gPending);
    if (allow) {
        context->Event.Decision = SfmAllow;
        context->Event.Reason = reason;
        FltCompletePendedPreOperation(data, FLT_PREOP_SUCCESS_WITH_CALLBACK, context);
    } else {
        FinishBlocked(data, context, reason);
        ExFreePoolWithTag(context, SFM_TAG);
        FltCompletePendedPreOperation(data, FLT_PREOP_COMPLETE, nullptr);
    }
    FltFreeDeferredIoWorkItem(workItem);
}

static FLT_PREOP_CALLBACK_STATUS NTAPI PreOperation(_Inout_ PFLT_CALLBACK_DATA data,
    _In_ PCFLT_RELATED_OBJECTS objects, _Outptr_result_maybenull_ PVOID* completionContext) {
    UNREFERENCED_PARAMETER(objects);
    *completionContext = nullptr;
    if (!InterlockedCompareExchange(&gEnabled, 0, 0) || InterlockedCompareExchange(&gStopping, 0, 0))
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    PEPROCESS process = FltGetRequestorProcess(data);
    LockShared();
    BOOLEAN broker = process && process == gBrokerProcess;
    UnlockShared();
    // Never block the broker, memory manager, or kernel-originated file-system I/O.
    if (broker || data->RequestorMode == KernelMode ||
        FlagOn(data->Iopb->IrpFlags, IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO) ||
        (data->Iopb->MajorFunction == IRP_MJ_CREATE &&
         FlagOn(data->Iopb->OperationFlags, SL_OPEN_PAGING_FILE))) {
        InterlockedIncrement64(&gBypassed);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }
    // A fast-I/O request cannot be pended. Force an IRP retry for enforcement.
    if (FLT_IS_FASTIO_OPERATION(data)) return FLT_PREOP_DISALLOW_FASTIO;
    if (!FLT_IS_IRP_OPERATION(data)) {
        InterlockedIncrement64(&gBypassed);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    PFLT_FILE_NAME_INFORMATION name = nullptr;
    NTSTATUS status = FltGetFileNameInformation(data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_ALWAYS_ALLOW_CACHE_LOOKUP, &name);
    if (!NT_SUCCESS(status)) {
        InterlockedIncrement64(&gNameFailures);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }
    if (!IsMonitored(&name->Name)) {
        FltReleaseFileNameInformation(name);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }
    auto context = static_cast<OperationContext*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(OperationContext), SFM_TAG));
    if (!context) {
        FltReleaseFileNameInformation(name);
        InterlockedIncrement64(&gDropped);
        data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }
    auto request = &context->Event.Request;
    request->Version = SFM_PROTOCOL_VERSION;
    request->RequestId = static_cast<ULONGLONG>(InterlockedIncrement64(&gSequence));
    LARGE_INTEGER time; KeQuerySystemTime(&time);
    request->TimeUtc = time.QuadPart;
    request->DeadlineUtc = time.QuadPart + static_cast<LONGLONG>(SFM_DECISION_SECONDS) * 10000000;
    request->ProcessId = FltGetRequestorProcessId(data);
    request->ProcessCreatedUtc = process ? static_cast<ULONGLONG>(PsGetProcessCreateTimeQuadPart(process)) : 0;
    // A longer ADS name must be denied rather than silently truncated for rule matching.
    if (name->Name.Length >= sizeof(request->Path)) {
        RtlCopyMemory(request->Path, name->Name.Buffer, sizeof(request->Path) - sizeof(WCHAR));
        FltReleaseFileNameInformation(name);
        FinishBlocked(data, context, SfmResourceFailure);
        ExFreePoolWithTag(context, SFM_TAG);
        return FLT_PREOP_COMPLETE;
    }
    RtlCopyMemory(request->Path, name->Name.Buffer, name->Name.Length);
    FltReleaseFileNameInformation(name);

    switch (data->Iopb->MajorFunction) {
    case IRP_MJ_CREATE: {
        request->Operation = SfmOpen;
        ACCESS_MASK access = data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;
        request->DesiredAccess = access;
        if (FlagOn(access, FILE_READ_DATA | FILE_READ_EA | FILE_READ_ATTRIBUTES | FILE_EXECUTE |
            READ_CONTROL | GENERIC_READ | GENERIC_EXECUTE | GENERIC_ALL | MAXIMUM_ALLOWED)) request->Operation |= SfmRead;
        if (FlagOn(access, FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
            DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED)) request->Operation |= SfmWrite;
        ULONG disposition = data->Iopb->Parameters.Create.Options >> 24;
        if (disposition == FILE_SUPERSEDE || disposition == FILE_CREATE || disposition == FILE_OVERWRITE ||
            disposition == FILE_OVERWRITE_IF || disposition == FILE_OPEN_IF ||
            FlagOn(data->Iopb->Parameters.Create.Options, FILE_DELETE_ON_CLOSE)) request->Operation |= SfmWrite;
        if (!(request->Operation & (SfmRead | SfmWrite))) request->Operation |= SfmRead;
        break;
    }
    case IRP_MJ_READ:
        request->Operation = SfmRead;
        request->RequestedBytes = data->Iopb->Parameters.Read.Length;
        request->Offset = data->Iopb->Parameters.Read.ByteOffset.QuadPart;
        break;
    case IRP_MJ_WRITE:
        request->Operation = SfmWrite;
        request->RequestedBytes = data->Iopb->Parameters.Write.Length;
        request->Offset = data->Iopb->Parameters.Write.ByteOffset.QuadPart;
        break;
    }

    if (InterlockedIncrement(&gPending) > SFM_MAX_PENDING) {
        InterlockedDecrement(&gPending);
        FinishBlocked(data, context, SfmOverload);
        ExFreePoolWithTag(context, SFM_TAG);
        return FLT_PREOP_COMPLETE;
    }
    PFLT_DEFERRED_IO_WORKITEM item = FltAllocateDeferredIoWorkItem();
    status = item ? FltQueueDeferredIoWorkItem(item, data, DecideOperation, DelayedWorkQueue, context)
                  : STATUS_INSUFFICIENT_RESOURCES;
    if (!NT_SUCCESS(status)) {
        if (item) FltFreeDeferredIoWorkItem(item);
        InterlockedDecrement(&gPending);
        FinishBlocked(data, context, status == STATUS_INSUFFICIENT_RESOURCES ? SfmResourceFailure : SfmUnsafeToPend);
        ExFreePoolWithTag(context, SFM_TAG);
        return FLT_PREOP_COMPLETE;
    }
    return FLT_PREOP_PENDING;
}

static FLT_POSTOP_CALLBACK_STATUS NTAPI PostOperation(_Inout_ PFLT_CALLBACK_DATA data,
    _In_ PCFLT_RELATED_OBJECTS objects, _In_opt_ PVOID rawContext, _In_ FLT_POST_OPERATION_FLAGS flags) {
    UNREFERENCED_PARAMETER(objects);
    auto context = static_cast<OperationContext*>(rawContext);
    if (!context) return FLT_POSTOP_FINISHED_PROCESSING;
    // The context is nonpaged and the ring is protected by a spinlock, so no
    // user-mode communication or paged allocation is needed on the completion path.
    if (!FlagOn(flags, FLTFL_POST_OPERATION_DRAINING)) {
        context->Event.Status = data->IoStatus.Status;
        context->Event.Phase = SfmCompleted;
        if (!(context->Event.Request.Operation & SfmOpen)) context->Event.TransferredBytes = data->IoStatus.Information;
        LARGE_INTEGER time; KeQuerySystemTime(&time); context->Event.CompletedUtc = time.QuadPart;
        LogEvent(&context->Event);
    } else {
        InterlockedIncrement64(&gDropped);
    }
    ExFreePoolWithTag(context, SFM_TAG);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

static NTSTATUS NTAPI Connect(_In_ PFLT_PORT clientPort, _In_opt_ PVOID serverCookie,
    _In_reads_bytes_opt_(contextSize) PVOID connectionContext, _In_ ULONG contextSize,
    _Outptr_result_maybenull_ PVOID* connectionCookie) {
    UNREFERENCED_PARAMETER(serverCookie); UNREFERENCED_PARAMETER(connectionContext);
    UNREFERENCED_PARAMETER(contextSize); *connectionCookie = nullptr;
    LockExclusive();
    gClientPort = clientPort;
    gBrokerProcess = PsGetCurrentProcess(); ObReferenceObject(gBrokerProcess);
    UnlockExclusive();
    return STATUS_SUCCESS;
}
static VOID NTAPI Disconnect(_In_opt_ PVOID cookie) {
    UNREFERENCED_PARAMETER(cookie);
    FltCloseClientPort(gFilter, &gClientPort);
    LockExclusive();
    PEPROCESS process = gBrokerProcess; gBrokerProcess = nullptr;
    UnlockExclusive();
    if (process) ObDereferenceObject(process);
    // Retain enabled paths after a crash: further intercepted operations are denied.
}
static NTSTATUS NTAPI Message(_In_opt_ PVOID cookie,
    _In_reads_bytes_opt_(inputLength) PVOID input, _In_ ULONG inputLength,
    _Out_writes_bytes_to_opt_(outputLength, *returned) PVOID output, _In_ ULONG outputLength,
    _Out_ PULONG returned) {
    UNREFERENCED_PARAMETER(cookie);
    *returned = 0;
    if (!input || inputLength < sizeof(SfmCommandHeader)) return STATUS_INVALID_PARAMETER;
    SfmCommandHeader header{};
    __try { RtlCopyMemory(&header, input, sizeof(header)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
    if (header.Version != SFM_PROTOCOL_VERSION) return STATUS_REVISION_MISMATCH;
    if (header.Command == SfmConfigure) {
        if (inputLength != sizeof(SfmConfiguration)) return STATUS_INVALID_BUFFER_SIZE;
        auto next = static_cast<SfmConfiguration*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(SfmConfiguration), SFM_TAG));
        if (!next) return STATUS_INSUFFICIENT_RESOURCES;
        NTSTATUS status = STATUS_SUCCESS;
        __try { RtlCopyMemory(next, input, sizeof(*next)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { status = GetExceptionCode(); }
        if (NT_SUCCESS(status) && (next->Enabled > 1 || next->FileCount > SFM_MAX_FILES ||
            (next->Enabled && !next->FileCount))) status = STATUS_INVALID_PARAMETER;
        if (NT_SUCCESS(status)) {
            for (ULONG i = 0; i < next->FileCount; ++i) {
                size_t length = 0;
                if (!NT_SUCCESS(RtlStringCchLengthW(next->Paths[i], SFM_MAX_PATH, &length)) || length < 9 ||
                    _wcsnicmp(next->Paths[i], L"\\Device\\", 8) != 0) { status = STATUS_INVALID_PARAMETER; break; }
            }
        }
        if (!NT_SUCCESS(status)) { ExFreePoolWithTag(next, SFM_TAG); return status; }
        LockExclusive();
        auto previous = gConfiguration; gConfiguration = next;
        InterlockedExchange(&gEnabled, static_cast<LONG>(next->Enabled));
        UnlockExclusive();
        if (previous) ExFreePoolWithTag(previous, SFM_TAG);
        return STATUS_SUCCESS;
    }
    if (header.Command == SfmPoll) {
        if (inputLength != sizeof(header) || !output || outputLength < sizeof(SfmPollResult))
            return STATUS_INVALID_BUFFER_SIZE;
        auto result = static_cast<SfmPollResult*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(SfmPollResult), SFM_TAG));
        if (!result) return STATUS_INSUFFICIENT_RESOURCES;
        result->Version = SFM_PROTOCOL_VERSION;
        result->DroppedEvents = static_cast<ULONGLONG>(InterlockedCompareExchange64(&gDropped, 0, 0));
        result->NameFailures = static_cast<ULONGLONG>(InterlockedCompareExchange64(&gNameFailures, 0, 0));
        result->BypassedOperations = static_cast<ULONGLONG>(InterlockedCompareExchange64(&gBypassed, 0, 0));
        result->PendingRequests = static_cast<ULONG>(InterlockedCompareExchange(&gPending, 0, 0));
        result->Enabled = static_cast<ULONG>(InterlockedCompareExchange(&gEnabled, 0, 0));
        KIRQL oldIrql; KeAcquireSpinLock(&gEventLock, &oldIrql);
        while (result->Count < SFM_EVENT_BATCH && gEventCount) {
            RtlCopyMemory(&result->Events[result->Count++], &gEvents[gEventHead], sizeof(SfmEvent));
            gEventHead = (gEventHead + 1) % SFM_EVENT_CAPACITY; --gEventCount;
        }
        KeReleaseSpinLock(&gEventLock, oldIrql);
        NTSTATUS status = STATUS_SUCCESS;
        __try { RtlCopyMemory(output, result, sizeof(*result)); *returned = sizeof(*result); }
        __except (EXCEPTION_EXECUTE_HANDLER) { status = GetExceptionCode(); }
        if (!NT_SUCCESS(status)) InterlockedAdd64(&gDropped, result->Count);
        ExFreePoolWithTag(result, SFM_TAG);
        return status;
    }
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS NTAPI InstanceSetup(_In_ PCFLT_RELATED_OBJECTS objects, _In_ FLT_INSTANCE_SETUP_FLAGS flags,
    _In_ DEVICE_TYPE deviceType, _In_ FLT_FILESYSTEM_TYPE fileSystem) {
    UNREFERENCED_PARAMETER(objects); UNREFERENCED_PARAMETER(flags);
    return deviceType == FILE_DEVICE_DISK_FILE_SYSTEM && fileSystem == FLT_FSTYPE_NTFS ?
        STATUS_SUCCESS : STATUS_FLT_DO_NOT_ATTACH;
}
static NTSTATUS NTAPI InstanceQueryTeardown(_In_ PCFLT_RELATED_OBJECTS objects, _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS flags) {
    UNREFERENCED_PARAMETER(objects); UNREFERENCED_PARAMETER(flags); return STATUS_SUCCESS;
}
static NTSTATUS NTAPI Unload(_In_ FLT_FILTER_UNLOAD_FLAGS flags) {
    UNREFERENCED_PARAMETER(flags);
    InterlockedExchange(&gStopping, 1); InterlockedExchange(&gEnabled, 0);
    FltCloseCommunicationPort(gServerPort);
    FltCloseClientPort(gFilter, &gClientPort);
    FltUnregisterFilter(gFilter);
    if (gBrokerProcess) { ObDereferenceObject(gBrokerProcess); gBrokerProcess = nullptr; }
    if (gConfiguration) ExFreePoolWithTag(gConfiguration, SFM_TAG);
    ExFreePoolWithTag(gEvents, SFM_TAG);
    return STATUS_SUCCESS;
}

static const FLT_OPERATION_REGISTRATION Operations[] = {
    { IRP_MJ_CREATE, 0, PreOperation, PostOperation },
    { IRP_MJ_READ, 0, PreOperation, PostOperation },
    { IRP_MJ_WRITE, 0, PreOperation, PostOperation },
    { IRP_MJ_OPERATION_END }
};
static const FLT_REGISTRATION Registration = {
    sizeof(FLT_REGISTRATION), FLT_REGISTRATION_VERSION, 0, nullptr, Operations,
    Unload, InstanceSetup, InstanceQueryTeardown, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};
extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driver, _In_ PUNICODE_STRING registryPath) {
    UNREFERENCED_PARAMETER(registryPath);
    ExInitializePushLock(&gConfigurationLock); KeInitializeSpinLock(&gEventLock);
    gEvents = static_cast<SfmEvent*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,
        sizeof(SfmEvent) * SFM_EVENT_CAPACITY, SFM_TAG));
    if (!gEvents) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status = FltRegisterFilter(driver, &Registration, &gFilter);
    if (!NT_SUCCESS(status)) { ExFreePoolWithTag(gEvents, SFM_TAG); return status; }
    PSECURITY_DESCRIPTOR security = nullptr;
    status = FltBuildDefaultSecurityDescriptor(&security, FLT_PORT_ALL_ACCESS);
    if (NT_SUCCESS(status)) {
        UNICODE_STRING name = RTL_CONSTANT_STRING(SFM_PORT_NAME);
        OBJECT_ATTRIBUTES attributes;
        InitializeObjectAttributes(&attributes, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, nullptr, security);
        status = FltCreateCommunicationPort(gFilter, &gServerPort, &attributes, nullptr, Connect, Disconnect, Message, 1);
        FltFreeSecurityDescriptor(security);
    }
    if (NT_SUCCESS(status)) status = FltStartFiltering(gFilter);
    if (!NT_SUCCESS(status)) {
        if (gServerPort) FltCloseCommunicationPort(gServerPort);
        FltUnregisterFilter(gFilter); ExFreePoolWithTag(gEvents, SFM_TAG);
    }
    return status;
}
