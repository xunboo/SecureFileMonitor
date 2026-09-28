
#include "pch.hpp"

#include "drvload.hpp"
#include "logger.hpp"
#include "utils.hpp"
#include <winternl.h>
#include <fltuser.h>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "FltLib.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

EXTERN_C
NTSYSAPI
NTSTATUS
NtLoadDriver(
	PUNICODE_STRING DriverServiceName
);

EXTERN_C
NTSYSAPI
NTSTATUS
NtUnloadDriver(
	PUNICODE_STRING DriverServiceName
);

#define REGISTRY_PATH_PREFIX		TEXT("System\\CurrentControlSet\\Services\\")
#define SERVICE_IMAGE_PATH_PREFIX	TEXT("\\??\\")
#define DRIVER_SERVICE_NAME_PREFIX	L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\"

CDrvLoader::CDrvLoader()
{

}

CDrvLoader::~CDrvLoader()
{

}

BOOL 
CDrvLoader::Init(
	IN const CString& strDriverName, 
	IN const CString& strDriverPath)
{
	CString resolvedPath = strDriverPath;
	CPath DriverPath(resolvedPath);

	if (DriverPath.IsRelative()){
		TCHAR szFullName[MAX_PATH] = { 0 };
		GetModuleFileName(NULL, szFullName, MAX_PATH);
		PathRemoveFileSpec(szFullName);
		PathAppend(szFullName, DriverPath);
		if (PathFileExists(szFullName)){
			DriverPath = szFullName;
		} else {
			// Check C:\Windows\System32\drivers\<strDriverPath>
			TCHAR szSysDrivers[MAX_PATH] = { 0 };
			GetSystemDirectory(szSysDrivers, MAX_PATH);
			PathAppend(szSysDrivers, TEXT("drivers"));
			PathAppend(szSysDrivers, strDriverPath);
			if (PathFileExists(szSysDrivers)){
				DriverPath = szSysDrivers;
			}
		}
	}

	if (!DriverPath.FileExists()){
		// Check C:\Windows\System32\drivers for PROCMON24.SYS, PROCMON25.SYS, or procmon.sys
		TCHAR szSysDrivers[MAX_PATH] = { 0 };
		GetSystemDirectory(szSysDrivers, MAX_PATH);
		PathAppend(szSysDrivers, TEXT("drivers"));
		PathAppend(szSysDrivers, TEXT("PROCMON24.SYS"));
		if (PathFileExists(szSysDrivers)){
			DriverPath = szSysDrivers;
		} else {
			PathRemoveFileSpec(szSysDrivers);
			PathAppend(szSysDrivers, TEXT("PROCMON25.SYS"));
			if (PathFileExists(szSysDrivers)){
				DriverPath = szSysDrivers;
			} else {
				PathRemoveFileSpec(szSysDrivers);
				PathAppend(szSysDrivers, TEXT("procmon.sys"));
				if (PathFileExists(szSysDrivers)){
					DriverPath = szSysDrivers;
				}
			}
		}
	}

	if (!DriverPath.FileExists()){
		LogMessage(L_WARN, TEXT("Driver file not exist: %s"), (LPCTSTR)DriverPath);
		return FALSE;
	}else{
		// Try enable driver load privilege
		UtilSetPriviledge(SE_LOAD_DRIVER_NAME, TRUE);
		m_strDriverPath = (LPCTSTR)DriverPath;
		if (PathFindFileName((LPCTSTR)DriverPath) && _tcsicmp(PathFindFileName((LPCTSTR)DriverPath), TEXT("PROCMON25.SYS")) == 0) {
			m_strDriverName = TEXT("PROCMON25");
		} else if (strDriverName.IsEmpty()) {
			m_strDriverName = TEXT("PROCMON24");
		} else {
			m_strDriverName = strDriverName;
		}
		LogMessage(L_INFO, TEXT("CDrvLoader::Init resolved driver '%s' to '%s'"), (LPCTSTR)m_strDriverName, (LPCTSTR)m_strDriverPath);
		return TRUE;
	}
	return FALSE;
}

#define STATUS_FLT_INSTANCE_ALTITUDE_COLLISION ((NTSTATUS)0xC01C0011L)
#define STATUS_IMAGE_ALREADY_LOADED ((NTSTATUS)0xC000010EL)
#define HRESULT_ALTITUDE_COLLISION ((HRESULT)0x801F0011L)

BOOL CDrvLoader::Load()
{
	if (!IsReady()) {
		LogMessage(L_ERROR, TEXT("CDrvLoader::Load() failed: loader not initialized (name='%s', path='%s')"),
			(LPCTSTR)m_strDriverName, (LPCTSTR)m_strDriverPath);
		return FALSE;
	}

	LogMessage(L_INFO, TEXT("CDrvLoader::Load() for '%s', path: '%s'"), (LPCTSTR)m_strDriverName, (LPCTSTR)m_strDriverPath);

	UtilSetPriviledge(SE_LOAD_DRIVER_NAME, TRUE);

	// Attempt to unload stale instances if any
	FilterUnload(CT2W(m_strDriverName));
	if (m_strDriverName.CompareNoCase(TEXT("PROCMON24")) == 0) {
		FilterUnload(L"PROCMON25");
	} else if (m_strDriverName.CompareNoCase(TEXT("PROCMON25")) == 0) {
		FilterUnload(L"PROCMON24");
	}

	static const LPCTSTR s_altitudes[] = {
		TEXT("385201"), // Primary alternate altitude: bypasses collision with 385200
		TEXT("385200"), // Standard Procmon altitude
		TEXT("385202"),
		TEXT("385205"),
		TEXT("385300")
	};

	HRESULT hr = E_FAIL;
	BOOL bLoaded = FALSE;

	for (size_t i = 0; i < _countof(s_altitudes); ++i)
	{
		LPCTSTR currentAltitude = s_altitudes[i];
		LogMessage(L_INFO, TEXT("Configuring service key with Altitude '%s'..."), currentAltitude);

		if (!CreateServiceKey(currentAltitude)) {
			LogMessage(L_ERROR, TEXT("CreateServiceKey() failed for altitude %s!"), currentAltitude);
			continue;
		}

		LogMessage(L_INFO, TEXT("Trying FilterLoad('%s')..."), (LPCTSTR)m_strDriverName);
		hr = FilterLoad(CT2W(m_strDriverName));
		LogMessage(L_INFO, TEXT("FilterLoad('%s') [Altitude %s] returned: 0x%08X"),
			(LPCTSTR)m_strDriverName, currentAltitude, hr);

		if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) || (DWORD)hr == 0x801F0012 /* FLT_IMAGE_ALREADY_LOADED */) {
			LogMessage(L_INFO, TEXT("FilterLoad succeeded with altitude %s!"), currentAltitude);
			bLoaded = TRUE;
			break;
		}

		if (hr == HRESULT_ALTITUDE_COLLISION) {
			LogMessage(L_WARN, TEXT("Altitude %s collided (0x801F0011), trying next altitude..."), currentAltitude);
			continue;
		}

		// Other error encountered; don't keep looping
		break;
	}

	if (bLoaded) {
		return TRUE;
	}

	// Try SCM StartService
	SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hSCM) {
		SC_HANDLE hSvc = OpenService(hSCM, m_strDriverName, SERVICE_START | SERVICE_QUERY_STATUS);
		if (hSvc) {
			if (StartService(hSvc, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
				LogMessage(L_INFO, TEXT("SCM StartService succeeded for '%s'"), (LPCTSTR)m_strDriverName);
				CloseServiceHandle(hSvc);
				CloseServiceHandle(hSCM);
				return TRUE;
			}
			CloseServiceHandle(hSvc);
		}
		CloseServiceHandle(hSCM);
	}

	CStringW strDriverSrvName;
	UNICODE_STRING UniDrvSrvName;
	NTSTATUS Status;

	strDriverSrvName = DRIVER_SERVICE_NAME_PREFIX;
	strDriverSrvName += CT2W(m_strDriverName);

	RtlInitUnicodeString(&UniDrvSrvName, strDriverSrvName);
	Status = NtLoadDriver(&UniDrvSrvName);
	LogMessage(L_INFO, TEXT("NtLoadDriver('%s') returned: 0x%08X"), (LPCWSTR)strDriverSrvName, Status);

	if (!NT_SUCCESS(Status) && Status != STATUS_IMAGE_ALREADY_LOADED){
		LogMessage(L_ERROR, TEXT("NtLoadDriver Failed code 0x%08X (FilterLoad: 0x%08X)"), Status, hr);
		return FALSE;
	}

	LogMessage(L_INFO, TEXT("NtLoadDriver succeeded!"));
	return TRUE;
}

BOOL CDrvLoader::UnLoad()
{
	CStringW strDrvSrvName;
	UNICODE_STRING UniDrvSrvName;
	NTSTATUS Status;

	strDrvSrvName = DRIVER_SERVICE_NAME_PREFIX;
	strDrvSrvName += m_strDriverName;

	RtlInitUnicodeString(&UniDrvSrvName, strDrvSrvName);
	Status = NtUnloadDriver(&UniDrvSrvName);
	if (!NT_SUCCESS(Status)) {
		LogMessage(L_ERROR, TEXT("NtUnloadDriver Failed code 0x%x"), Status);
		return FALSE;
	}

	return TRUE;
}

BOOL CDrvLoader::CreateServiceInstanceKey(HKEY hKey, LPCTSTR szAltitude)
{
	HKEY hKeySubIns = NULL;
	HKEY hKeyInstance = NULL;
	DWORD Data = 3;

	RegSetValueEx(hKey, TEXT("SupportedFeatures"), 0, REG_DWORD, (const BYTE*)&Data, sizeof(DWORD));
	
	if (RegCreateKeyEx(hKey, TEXT("Instances"), 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKeyInstance, NULL) == ERROR_SUCCESS)
	{
		CString strInstanceName = m_strDriverName + TEXT(" Instance");
		RegSetValueEx(hKeyInstance, TEXT("DefaultInstance"), 0, REG_SZ, (const BYTE*)strInstanceName.GetBuffer(),
			(DWORD)(((DWORD)strInstanceName.GetLength() + 1) * sizeof(TCHAR)));

		if (RegCreateKeyEx(hKeyInstance, strInstanceName.GetBuffer(), 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKeySubIns, NULL) == ERROR_SUCCESS)
		{
			RegSetValueEx(hKeySubIns, TEXT("Altitude"), 0, REG_SZ, (const BYTE*)szAltitude, 
				(DWORD)(((DWORD)_tcslen(szAltitude) + 1) * sizeof(TCHAR)));
			Data = 0;
			RegSetValueEx(hKeySubIns, TEXT("Flags"), 0, REG_DWORD, (const BYTE*)&Data, sizeof(DWORD));
			RegCloseKey(hKeySubIns);
		}
		RegCloseKey(hKeyInstance);
	}

	return TRUE;
}

BOOL CDrvLoader::CreateServiceKey(LPCTSTR szAltitude)
{
	CString strRegistryPath;
	HKEY hKey;
	LSTATUS dwErrorCode;
	DWORD dwDisposition;
	CString strServiceImagePath;
	DWORD dwImagPathSize;

	if (!IsReady()) {
		LogMessage(L_ERROR, TEXT("CreateServiceKey failed: driver not ready (name='%s', path='%s')"),
			(LPCTSTR)m_strDriverName, (LPCTSTR)m_strDriverPath);
		return FALSE;
	}

	// 1. Try registering via SCM so SCM database is aware of the service
	SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hSCM)
	{
		SC_HANDLE hSvc = CreateService(
			hSCM,
			m_strDriverName,
			m_strDriverName,
			SERVICE_ALL_ACCESS,
			SERVICE_FILE_SYSTEM_DRIVER,
			SERVICE_DEMAND_START,
			SERVICE_ERROR_NORMAL,
			m_strDriverPath,
			NULL,
			NULL,
			TEXT("FltMgr\0"),
			NULL,
			NULL);
		if (hSvc)
		{
			LogMessage(L_INFO, TEXT("SCM CreateService succeeded for '%s'"), (LPCTSTR)m_strDriverName);
			CloseServiceHandle(hSvc);
		}
		else
		{
			DWORD err = GetLastError();
			if (err == ERROR_SERVICE_EXISTS) {
				LogMessage(L_INFO, TEXT("SCM service '%s' already exists"), (LPCTSTR)m_strDriverName);
			} else {
				LogMessage(L_WARN, TEXT("SCM CreateService note (err=%u)"), err);
			}
		}
		CloseServiceHandle(hSCM);
	}

	// 2. Format registry path and ensure all Minifilter keys exist
	strRegistryPath = REGISTRY_PATH_PREFIX;
	strRegistryPath += m_strDriverName;

	strServiceImagePath = SERVICE_IMAGE_PATH_PREFIX;
	strServiceImagePath += m_strDriverPath;

	dwErrorCode = RegCreateKeyEx(HKEY_LOCAL_MACHINE,
		strRegistryPath.GetBuffer(),
		0,
		NULL,
		0,
		KEY_ALL_ACCESS,
		NULL,
		&hKey,
		&dwDisposition);
	if (ERROR_SUCCESS != dwErrorCode) {
		LogMessage(L_ERROR, TEXT("RegCreateKeyEx '%s' failed: %u"), (LPCTSTR)strRegistryPath, dwErrorCode);
		return FALSE;
	}

	dwImagPathSize = (DWORD)(((DWORD)strServiceImagePath.GetLength() + 1) * sizeof(TCHAR));
	dwErrorCode = RegSetValueEx(hKey,
		TEXT("ImagePath"),
		0,
		REG_EXPAND_SZ,
		(const BYTE*)strServiceImagePath.GetBuffer(),
		dwImagPathSize);
	if (ERROR_SUCCESS != dwErrorCode) {
		RegCloseKey(hKey);
		return FALSE;
	}

	// Set type: 2 (SERVICE_FILE_SYSTEM_DRIVER for minifilters)
	DWORD dwServiceType = 2;
	RegSetValueEx(hKey, TEXT("Type"), 0, REG_DWORD, (const BYTE*)&dwServiceType, sizeof(dwServiceType));

	DWORD dwServiceErrorControl = 1;
	RegSetValueEx(hKey, TEXT("ErrorControl"), 0, REG_DWORD, (const BYTE*)&dwServiceErrorControl, sizeof(DWORD));

	DWORD dwServiceStart = 3;
	RegSetValueEx(hKey, TEXT("Start"), 0, REG_DWORD, (const BYTE*)&dwServiceStart, sizeof(dwServiceStart));

	// Depend on Filter Manager
	LPCTSTR szDepend = TEXT("FltMgr\0");
	RegSetValueEx(hKey,
		TEXT("DependOnService"),
		0,
		REG_MULTI_SZ,
		(const BYTE*)szDepend,
		8 * sizeof(TCHAR));

	CreateServiceInstanceKey(hKey, szAltitude);

	RegCloseKey(hKey);
	return TRUE;
}

VOID CDrvLoader::DeleteServiceKey()
{
	CString strRegistryPath;
	
	if (!IsReady()){
		return;
	}

	// Format service registry path
	strRegistryPath = REGISTRY_PATH_PREFIX;
	strRegistryPath += m_strDriverName;

	SHDeleteKey(HKEY_LOCAL_MACHINE, strRegistryPath);
}

BOOL CDrvLoader::IsReady()
{
	if (m_strDriverName.IsEmpty() || m_strDriverPath.IsEmpty()) {
		return FALSE;
	}
	return TRUE;
}

