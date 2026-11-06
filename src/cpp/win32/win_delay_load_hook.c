








#ifdef _WIN32

#ifndef DELAYIMP_INSECURE_WRITABLE_HOOKS
#define DELAYIMP_INSECURE_WRITABLE_HOOKS
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <Shlwapi.h>
#include <delayimp.h>
#include <string.h>

#ifndef _tcsicmp
#ifdef _UNICODE
#define _tcsicmp _wcsicmp
#else
#define _tcsicmp stricmp
#endif
#endif

static FARPROC WINAPI load_exe_hook(unsigned int event, DelayLoadInfo* info) {
  if (event != dliNotePreLoadLibrary) return NULL;

  if (_stricmp(info->szDll, "iojs.exe") != 0 &&
      _stricmp(info->szDll, "node.exe") != 0 &&
      _stricmp(info->szDll, "electron.exe") != 0 &&
      _stricmp(info->szDll, "node.dll") != 0)
    return NULL;

  
  HMODULE processModule = GetModuleHandle(NULL);

  
  TCHAR processPath[_MAX_PATH];
  GetModuleFileName(processModule, processPath, _MAX_PATH);

  
  LPTSTR processName = PathFindFileName(processPath);

  
  
  if (_tcsicmp(processName, TEXT("node.exe")) == 0 ||
      _tcsicmp(processName, TEXT("iojs.exe")) == 0 ||
      _tcsicmp(processName, TEXT("electron.exe")) == 0) {
    return (FARPROC) processModule;
  }

  
  
  PathRemoveFileSpec(processPath);
  PathAppend(processPath, TEXT("node.dll"));

  HMODULE nodeDllModule = GetModuleHandle(processPath);
  if(nodeDllModule != NULL) {
    
    
    return (FARPROC) nodeDllModule;
  }

  
  
  return (FARPROC) processModule;
}

PfnDliHook __pfnDliNotifyHook2 = load_exe_hook;

#endif 
