
#include "pch.hpp"

#include "logger.hpp"
#include <mutex>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

std::mutex gLogLock;

BOOL LogMessage(LEVEL Level, LPCTSTR Format, ...)
{
	TCHAR Buffer[MAX_LOG_MESSAGE] = { 0 };
	va_list Args;

	va_start(Args, Format);
	StringCchVPrintf(Buffer, MAX_LOG_MESSAGE, Format, Args);
	va_end(Args);

	std::lock_guard<std::mutex> lock(gLogLock);

	LPCTSTR prefix = TEXT("[?]");
	switch (Level) {
	case L_DEBUG: prefix = TEXT("[DEBUG]"); break;
	case L_INFO:  prefix = TEXT("[INFO]");  break;
	case L_WARN:  prefix = TEXT("[WARN]");  break;
	case L_ERROR: prefix = TEXT("[ERROR]"); break;
	}

	TCHAR formatted[MAX_LOG_MESSAGE + 64] = { 0 };
	StringCchPrintf(formatted, _countof(formatted), TEXT("%s %s\n"), prefix, Buffer);

	// 1. Output to console if attached
	_fputts(formatted, (Level == L_ERROR || Level == L_WARN) ? stderr : stdout);
	fflush(stdout);
	fflush(stderr);

	// 2. Output to Windows debugger / DebugView
	OutputDebugString(formatted);

	// 3. Output to openprocmon.log next to the executable
	static TCHAR s_szLogPath[MAX_PATH] = { 0 };
	if (s_szLogPath[0] == 0)
	{
		GetModuleFileName(NULL, s_szLogPath, MAX_PATH);
		PathRemoveFileSpec(s_szLogPath);
		PathAppend(s_szLogPath, TEXT("openprocmon.log"));
	}

	FILE* fp = NULL;
	if (_tfopen_s(&fp, s_szLogPath, TEXT("a+, ccs=UTF-8")) == 0 && fp)
	{
		_fputts(formatted, fp);
		fclose(fp);
	}

	return TRUE;
}