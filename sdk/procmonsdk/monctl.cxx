
#include "pch.hpp"
#include "logger.hpp"
#include "eventmgr.hpp"
#include "monctl.hpp"

#include <assert.h>

#pragma comment(lib, "FltLib.lib")

CMonitorContoller::CMonitorContoller()
{

}

CMonitorContoller::~CMonitorContoller()
{
	DisConnect();
}


BOOL CMonitorContoller::Connect()
{
	// Stop() pauses collection but deliberately retains the port and workers.
	// Reuse that connection on restart: opening another port can fail because
	// the driver already has this client connected, and can lose the old handle.
	if (m_hPort && m_hPort != INVALID_HANDLE_VALUE) {
		return TRUE;
	}
	m_hPort = NULL;
	BOOL bOk = FALSE;
	ULONG Flag = 0;

	LogMessage(L_INFO, TEXT("CMonitorContoller::Connect() started"));

	static const LPCWSTR s_candidatePorts[] = {
		L"\\ProcessMonitor24Port",
		L"\\ProcessMonitor25Port",
		L"\\ProcessMonitor23Port",
		L"\\OpenProcessMonitor24Port"
	};

	// Phase 1: Check if any Procmon communication port is already active
	for (size_t p = 0; p < _countof(s_candidatePorts); ++p)
	{
		HANDLE candidatePort = NULL;
		HRESULT hResult = FilterConnectCommunicationPort(s_candidatePorts[p],
			0,
			&Flag,
			sizeof(ULONG),
			NULL,
			&candidatePort);

		LogMessage(L_INFO, TEXT("FilterConnectCommunicationPort('%s') -> 0x%08X"), s_candidatePorts[p], hResult);

		if (SUCCEEDED(hResult)) {
			m_hPort = candidatePort;
			LogMessage(L_INFO, TEXT("Connected to existing driver port '%s'"), s_candidatePorts[p]);
			bOk = TRUE;
			break;
		}
	}

	// Phase 2: If none is active, initialize and load the driver
	if (!bOk)
	{
		CDrvLoader& drvLoader = Singleton<CDrvLoader>::getInstance();
		if (!drvLoader.IsReady()) {
			LogMessage(L_INFO, TEXT("Initializing CDrvLoader with PROCMON24 / PROCMON25..."));
			TCHAR szSysDrivers[MAX_PATH] = { 0 };
			GetSystemDirectory(szSysDrivers, MAX_PATH);
			PathAppend(szSysDrivers, TEXT("drivers\\PROCMON25.SYS"));
			if (PathFileExists(szSysDrivers)) {
				drvLoader.Init(TEXT("PROCMON25"), TEXT("PROCMON25.SYS"));
			} else {
				drvLoader.Init(TEXT("PROCMON24"), TEXT("PROCMON24.SYS"));
			}
		}

		if (drvLoader.IsReady()) {
			LogMessage(L_INFO, TEXT("Calling drvLoader.Load()..."));
			if (drvLoader.Load()) {
				LogMessage(L_INFO, TEXT("drvLoader.Load() succeeded, retrying port connection..."));

				for (size_t p = 0; p < _countof(s_candidatePorts); ++p)
				{
					HANDLE candidatePort = NULL;
					HRESULT hResult = FilterConnectCommunicationPort(s_candidatePorts[p],
						0,
						&Flag,
						sizeof(ULONG),
						NULL,
						&candidatePort);

					LogMessage(L_INFO, TEXT("Post-load FilterConnectCommunicationPort('%s') -> 0x%08X"), s_candidatePorts[p], hResult);

					if (SUCCEEDED(hResult)) {
						m_hPort = candidatePort;
						LogMessage(L_INFO, TEXT("Successfully connected to driver port '%s'"), s_candidatePorts[p]);
						bOk = TRUE;
						break;
					}
				}
			} else {
				LogMessage(L_ERROR, TEXT("drvLoader.Load() failed!"));
			}
		} else {
			LogMessage(L_ERROR, TEXT("drvLoader.Init() failed to locate driver file!"));
		}
	}

	if (bOk){
		m_RecvThread.Init(m_hPort);
		LogMessage(L_INFO, TEXT("Driver port connected and worker thread initialized successfully."));
	} else {
		LogMessage(L_ERROR, TEXT("Could not connect to any Procmon communication port."));
	}
	
	return bOk;
}

VOID 
CMonitorContoller::DisConnect()
{
	if (m_hPort){
		CloseHandle(m_hPort);
		m_hPort = NULL;
	}
}

VOID 
CMonitorContoller::SetMonitor(
	IN BOOL bEnableProc, 
	IN BOOL bEnableFile, 
	IN BOOL bEnableReg
)
{
	m_dwControl = 0;
	if (bEnableProc){
		m_dwControl |= CTL_MONITOR_PROC_ON;
	}

	if (bEnableFile) {
		m_dwControl |= CTL_MONITOR_FILE_ON;
	}

	if (bEnableReg) {
		m_dwControl |= CTL_MONITOR_REG_ON;
	}
}

BOOL CMonitorContoller::DisableAll()
{
	return Control(CTL_MONITOR_ALL_CLOSE);
}

BOOL CMonitorContoller::Start()
{
	bool bRet;

	//
	// start processing thread
	//
	
	bRet = m_OptThread.Start();
	if (!bRet){
		return FALSE;
	}
	
	//
	// start event receive thread
	//
	
	bRet = m_RecvThread.Start();
	if (!bRet){
		m_OptThread.Stop();
		return FALSE;
	}
	
	return Control(m_dwControl);
}

BOOL CMonitorContoller::Stop()
{
	BOOL bRet;
	bRet = DisableAll();
	EVENTMGR().Clear();
	//PROCMGR().Clear();

	return bRet;
}

BOOL CMonitorContoller::Destory()
{
	DisableAll();
	m_OptThread.Stop();
	m_RecvThread.Stop();
	DisConnect();
	return TRUE;
}

BOOL
CMonitorContoller::Control()
{
	return Control(m_dwControl);
}

BOOL 
CMonitorContoller::Control(
	IN DWORD Flags
)
{
	HRESULT hResult;
	DWORD dwRetBytes;
	FLTMSG_CONTROL_FLAGS Controls;

	Controls.Head.CtlCode = 0;
	Controls.Flags = Flags;
	hResult = FilterSendMessage(m_hPort, &Controls, sizeof(Controls), NULL, 0, &dwRetBytes);
	if (hResult != S_OK) {
		LogMessage(L_ERROR, TEXT("Can not enable monitor"));
		return FALSE;
	}
	return TRUE;
}

BOOL CRecvThread::Init(HANDLE hPort)
{
	m_hPort = hPort;
	return TRUE;
}



void CRecvThread::Run()
{
	HRESULT hResult;
	ULONG MessageLength = MAX_PROCMON_MESSAGE_LEN + sizeof(PROCMON_MESSAGE_HEADER);
	PPROCMON_MESSAGE_HEADER pMessage = (PPROCMON_MESSAGE_HEADER)HeapAlloc(GetProcessHeap(), 0,
		MAX_PROCMON_MESSAGE_LEN + sizeof(PROCMON_MESSAGE_HEADER));
	OVERLAPPED Overlapped = { 0 };

	if (!pMessage) {
		return;
	}

	ZeroMemory(&Overlapped, sizeof(Overlapped));
	Overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (!Overlapped.hEvent) {
		HeapFree(GetProcessHeap(), 0, pMessage);
		return;
	}

	while (!IsStop())
	{
		hResult = FilterGetMessage(m_hPort,
			&pMessage->Header, MessageLength, &Overlapped);

		if (hResult == HRESULT_FROM_WIN32(ERROR_IO_PENDING)) {

			//if(WaitForSingleObject(Overlapped.hEvent, 500) == WAIT_TIMEOUT){
			//	continue;
			//}
			
			//
			// wait until data ready
			//
			
			BOOL bNeedBreak = FALSE;
			while (TRUE)
			{
				DWORD dwWaitResult = WaitForSingleObject(Overlapped.hEvent, 500);
				if (dwWaitResult == WAIT_TIMEOUT){
					if (IsStop()){
						bNeedBreak = TRUE;
						break;
					}else{
						
						//
						// Continue waiting
						//
						
					}
				}else if (dwWaitResult == WAIT_OBJECT_0){
					
					//
					// the data ready
					//
					
					break;
				}else{
					bNeedBreak = TRUE;
					break;
				}
			}

			if (bNeedBreak) {
				break;
			}


		}else if (hResult != S_OK) {

			//
			// TODO error
			//

			assert(FALSE);

		}

		//
		// here we receive a message block.
		// try to decode the block
		//

		PLOG_ENTRY pEntries = (PLOG_ENTRY)(pMessage + 1);
		
		//
		// pass to operator mgr
		//
		
		if(!Singleton<CEventMgr>::getInstance().ProcessMsgBlocks(pEntries, pMessage->Length)){
			LogMessage(L_WARN, TEXT("Failed to process msg blocks"));
		}

	}

	LogMessage(L_INFO, TEXT(".......recv thread exit....."));

	//
	// clean up
	//

	if (pMessage) {
		HeapFree(GetProcessHeap(), 0, pMessage);
	}
}

void COPtThread::Run()
{
	while (!IsStop())
	{
		
		//
		// If queue have no data, the function return false
		// for every 500ms. so here we have a opportunity to 
		// exit the loop
		//
		
		Singleton<CEventMgr>::getInstance().Process();
	}

	LogMessage(L_INFO, TEXT(".......processing thread exit....."));

}

