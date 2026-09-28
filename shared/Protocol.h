#pragma once

// This header is shared by the Win32 broker and the WDK minifilter.
// Only fixed-width Windows types cross the communication port. No pointers.
#ifndef _KERNEL_MODE
#include <Windows.h>
#endif

#define SFM_PORT_NAME L"\\SecureFileMonitorPort"
#define SFM_PROTOCOL_VERSION 1u
#define SFM_MAX_PATH 1024u
#define SFM_MAX_FILES 128u
#define SFM_EVENT_BATCH 24u
#define SFM_EVENT_CAPACITY 2048u
#define SFM_MAX_PENDING 16u
#define SFM_DECISION_SECONDS 20u

enum SfmOperation : ULONG { SfmRead = 1, SfmWrite = 2, SfmOpen = 4 };
enum SfmCommand : ULONG { SfmConfigure = 1, SfmPoll = 2 };
enum SfmDecision : ULONG { SfmDeny = 0, SfmAllow = 1 };
enum SfmReason : ULONG {
    SfmRule = 1, SfmUser = 2, SfmTimeout = 3, SfmDisconnected = 4,
    SfmOverload = 5, SfmUnsafeToPend = 6, SfmResourceFailure = 7,
    SfmStopping = 8, SfmInvalidReply = 9, SfmSessionRule = 10
};
enum SfmPhase : ULONG { SfmCompleted = 1, SfmBlocked = 2 };

#pragma pack(push, 8)
struct SfmCommandHeader { ULONG Version; ULONG Command; };
struct SfmConfiguration {
    SfmCommandHeader Header;
    ULONG Enabled;
    ULONG FileCount;
    WCHAR Paths[SFM_MAX_FILES][SFM_MAX_PATH];
};

struct SfmRequest {
    ULONG Version;
    ULONG Operation;
    ULONGLONG RequestId;
    LONGLONG TimeUtc;
    LONGLONG DeadlineUtc;
    ULONGLONG ProcessCreatedUtc;
    LONGLONG Offset;
    ULONG ProcessId;
    ULONG DesiredAccess;
    ULONG RequestedBytes;
    ULONG Reserved;
    WCHAR Path[SFM_MAX_PATH];
};

struct SfmReply { ULONG Decision; ULONG Reason; };
struct SfmEvent {
    SfmRequest Request;
    LONGLONG CompletedUtc;
    ULONGLONG TransferredBytes;
    LONG Status;
    ULONG Decision;
    ULONG Reason;
    ULONG Phase;
};

struct SfmPollResult {
    ULONG Version;
    ULONG Count;
    ULONGLONG DroppedEvents;
    ULONGLONG NameFailures;
    ULONGLONG BypassedOperations;
    ULONG PendingRequests;
    ULONG Enabled;
    SfmEvent Events[SFM_EVENT_BATCH];
};
#pragma pack(pop)

static_assert(sizeof(WCHAR) == 2, "Wire protocol needs UTF-16 WCHAR");
static_assert(sizeof(SfmRequest) == 2112, "Protocol request layout changed");
static_assert(sizeof(SfmReply) == 8, "Protocol reply layout changed");
static_assert(sizeof(SfmEvent) == 2144, "Protocol event layout changed");
static_assert(FIELD_OFFSET(SfmRequest, Path) == 64, "Protocol alignment changed");

