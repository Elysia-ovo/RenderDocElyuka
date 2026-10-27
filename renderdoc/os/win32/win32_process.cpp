/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

// must be separate so that it's included first and not sorted by clang-format
#include <windows.h>

#include <Psapi.h>
#include <tchar.h>
#include <tlhelp32.h>
#include "common/formatting.h"
#include "core/core.h"
#include "os/os_specific.h"
#include "strings/string_utils.h"

#include <string>

static rdcarray<EnvironmentModification> &GetEnvModifications()
{
  static rdcarray<EnvironmentModification> envCallbacks;
  return envCallbacks;
}

struct InsensitiveComparison
{
  bool operator()(const rdcstr &a, const rdcstr &b) const { return strlower(a) < strlower(b); }
};

typedef std::map<rdcstr, rdcstr, InsensitiveComparison> EnvMap;

static EnvMap EnvStringToEnvMap(const wchar_t *envstring)
{
  EnvMap ret;

  const wchar_t *e = envstring;

  while(*e)
  {
    const wchar_t *equals = wcschr(e, L'=');

    rdcstr name = StringFormat::Wide2UTF8(rdcwstr(e, equals - e));
    rdcstr value = StringFormat::Wide2UTF8(equals + 1);

    ret[name] = value;

    // jump to \0 and past it
    e += wcslen(e) + 1;
  }

  return ret;
}

void Process::RegisterEnvironmentModification(const EnvironmentModification &modif)
{
  GetEnvModifications().push_back(modif);
}

static void ApplyEnvModifications(EnvMap &envValues,
                                  const rdcarray<EnvironmentModification> &modifications,
                                  bool setToSystem)
{
  for(size_t i = 0; i < modifications.size(); i++)
  {
    const EnvironmentModification &m = modifications[i];

    rdcstr value;

    auto it = envValues.find(m.name);
    if(it != envValues.end())
      value = it->second;

    switch(m.mod)
    {
      case EnvMod::Set: value = m.value.c_str(); break;
      case EnvMod::Append:
      {
        if(!value.empty())
        {
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            value += ";";
          else if(m.sep == EnvSep::Colon)
            value += ":";
        }
        value += m.value.c_str();
        break;
      }
      case EnvMod::Prepend:
      {
        if(!value.empty())
        {
          rdcstr prep = m.value;
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            prep += ";";
          else if(m.sep == EnvSep::Colon)
            prep += ":";
          value = prep + value;
        }
        else
        {
          value = m.value.c_str();
        }
        break;
      }
    }

    envValues[m.name] = value;

    if(setToSystem)
      SetEnvironmentVariableW(StringFormat::UTF82Wide(m.name).c_str(),
                              StringFormat::UTF82Wide(value).c_str());
  }
}

// on windows we apply environment changes here, after process initialisation
// but before any real work (in RenderDoc::Initialise) so that we support
// injecting the dll into processes we didn't launch (ie didn't control the
// starting environment for), or even the application loading the dll itself
// without any interaction with our replay app.
void Process::ApplyEnvironmentModification()
{
  // turn environment string to a UTF-8 map
  LPWCH envStrings = GetEnvironmentStringsW();
  EnvMap envValues = EnvStringToEnvMap(envStrings);
  FreeEnvironmentStringsW(envStrings);
  rdcarray<EnvironmentModification> &modifications = GetEnvModifications();

  ApplyEnvModifications(envValues, modifications, true);

  // these have been applied to the current process
  modifications.clear();
}

rdcstr Process::GetEnvVariable(const rdcstr &name)
{
  DWORD len = GetEnvironmentVariableA(name.c_str(), NULL, 0);
  if(len == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
    return rdcstr();

  rdcstr ret;
  ret.resize(len + 1);

  GetEnvironmentVariableA(name.c_str(), ret.data(), len);
  ret.trim();
  return ret;
}

uint64_t Process::GetMemoryUsage()
{
  HANDLE proc = GetCurrentProcess();

  if(proc == NULL)
  {
    RDCERR("Couldn't open process: %d", GetLastError());
    return 0;
  }

  PROCESS_MEMORY_COUNTERS memInfo = {};

  uint64_t ret = 0;

  if(GetProcessMemoryInfo(proc, &memInfo, sizeof(memInfo)))
  {
    ret = memInfo.WorkingSetSize;
  }
  else
  {
    RDCERR("Couldn't get process memory info: %d", GetLastError());
  }

  return ret;
}

// helpers for various shims and dlls etc, not part of the public API
extern "C" __declspec(dllexport) void __cdecl INTERNAL_GetTargetControlIdent(uint32_t *ident)
{
  if(ident)
    *ident = RenderDoc::Inst().GetTargetControlIdent();
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureOptions(CaptureOptions *opts)
{
  if(opts)
    RenderDoc::Inst().SetCaptureOptions(*opts);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureFile(const char *capfile)
{
  if(capfile)
    RenderDoc::Inst().SetCaptureFileTemplate(capfile);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetDebugLogFile(const char *logfile)
{
  RENDERDOC_SetDebugLogFile(logfile ? logfile : rdcstr());
}

static EnvironmentModification tempEnvMod;

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModName(const char *name)
{
  if(name)
    tempEnvMod.name = name;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModValue(const char *value)
{
  if(value)
    tempEnvMod.value = value;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvSep(EnvSep *sep)
{
  if(sep)
    tempEnvMod.sep = *sep;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvMod(EnvMod *mod)
{
  if(mod)
  {
    tempEnvMod.mod = *mod;
    Process::RegisterEnvironmentModification(tempEnvMod);
  }
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_ApplyEnvMods(void *ignored)
{
  Process::ApplyEnvironmentModification();
}

enum class ContextInjectionResult
{
  Succeeded,
  SafeToFallback,
  FailedAfterExecution,
};

enum class RemoteInvokeResult
{
  Succeeded,
  FailedSafe,
  FailedUnsafe,
};

struct RemoteCallState
{
  uintptr_t result;
  uint32_t completed;
};

// Executes a one-argument function on the primary thread of a process that RenderDoc itself created
// with CREATE_SUSPENDED. We deliberately don't enumerate or hijack a thread in an already-running
// process: there is no reliable way to know whether such a thread owns the loader lock, is
// unwinding, or is otherwise at a safe call boundary.
//
// The trampoline writes its return value and completion flag, then parks in a small loop. Only
// after observing completion do we suspend the thread, verify that it is parked, and restore its
// complete original context. The thread is left suspended, preserving the CREATE_SUSPENDED contract
// for the rest of the injection sequence.
static ContextInjectionResult InjectViaSetThreadContext(HANDLE hProcess, HANDLE hThread,
                                                        void *remoteFunc, void *remoteArg,
                                                        uintptr_t *callResult)
{
  if(hThread == NULL)
    return ContextInjectionResult::SafeToFallback;

  CONTEXT origCtx;
  RDCEraseEl(origCtx);
  origCtx.ContextFlags = CONTEXT_ALL;

  if(!GetThreadContext(hThread, &origCtx))
  {
    RDCERR("Couldn't get suspended primary thread context: %u", GetLastError());
    return ContextInjectionResult::SafeToFallback;
  }

  void *stateMem = VirtualAllocEx(hProcess, NULL, sizeof(RemoteCallState), MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
  if(stateMem == NULL)
  {
    RDCERR("Couldn't allocate remote call state: %u", GetLastError());
    return ContextInjectionResult::SafeToFallback;
  }

#if defined(_M_X64) || defined(__x86_64__)
  static_assert(offsetof(RemoteCallState, completed) == 8,
                "Unexpected x64 remote call state layout");

  unsigned char shellcode[] = {
      0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,    // mov rcx, arg
      0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,    // mov rax, func
      0xFF, 0xD0,                                                    // call rax
      0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,    // mov rdx, state
      0x48, 0x89, 0x02,                                              // mov [rdx], rax
      0xC7, 0x42, 0x08, 0x01, 0x00, 0x00, 0x00,                      // mov [rdx+8], 1
      0xF3, 0x90,                                                    // pause
      0xEB, 0xFC,                                                    // jmp pause
  };

  const size_t loopOffset = 42;
  uintptr_t arg = (uintptr_t)remoteArg;
  uintptr_t func = (uintptr_t)remoteFunc;
  uintptr_t stateAddr = (uintptr_t)stateMem;
  memcpy(shellcode + 2, &arg, sizeof(arg));
  memcpy(shellcode + 12, &func, sizeof(func));
  memcpy(shellcode + 24, &stateAddr, sizeof(stateAddr));
#else
  static_assert(offsetof(RemoteCallState, completed) == 4,
                "Unexpected x86 remote call state layout");

  unsigned char shellcode[] = {
      0x68, 0x00, 0x00, 0x00, 0x00,                // push arg
      0xB8, 0x00, 0x00, 0x00, 0x00,                // mov eax, func
      0xFF, 0xD0,                                  // call eax
      0xBA, 0x00, 0x00, 0x00, 0x00,                // mov edx, state
      0x89, 0x02,                                  // mov [edx], eax
      0xC7, 0x42, 0x04, 0x01, 0x00, 0x00, 0x00,    // mov [edx+4], 1
      0xF3, 0x90,                                  // pause
      0xEB, 0xFC,                                  // jmp pause
  };

  const size_t loopOffset = 26;
  uintptr_t arg = (uintptr_t)remoteArg;
  uintptr_t func = (uintptr_t)remoteFunc;
  uintptr_t stateAddr = (uintptr_t)stateMem;
  memcpy(shellcode + 1, &arg, sizeof(arg));
  memcpy(shellcode + 6, &func, sizeof(func));
  memcpy(shellcode + 13, &stateAddr, sizeof(stateAddr));
#endif

  void *shellcodeMem =
      VirtualAllocEx(hProcess, NULL, sizeof(shellcode), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if(shellcodeMem == NULL)
  {
    RDCERR("Couldn't allocate remote trampoline: %u", GetLastError());
    VirtualFreeEx(hProcess, stateMem, 0, MEM_RELEASE);
    return ContextInjectionResult::SafeToFallback;
  }

  SIZE_T written = 0;
  if(!WriteProcessMemory(hProcess, shellcodeMem, shellcode, sizeof(shellcode), &written) ||
     written != sizeof(shellcode))
  {
    RDCERR("Couldn't write complete remote trampoline: %u", GetLastError());
    VirtualFreeEx(hProcess, shellcodeMem, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, stateMem, 0, MEM_RELEASE);
    return ContextInjectionResult::SafeToFallback;
  }

  DWORD oldProtect = 0;
  if(!VirtualProtectEx(hProcess, shellcodeMem, sizeof(shellcode), PAGE_EXECUTE_READ, &oldProtect) ||
     !FlushInstructionCache(hProcess, shellcodeMem, sizeof(shellcode)))
  {
    RDCERR("Couldn't make remote trampoline executable: %u", GetLastError());
    VirtualFreeEx(hProcess, shellcodeMem, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, stateMem, 0, MEM_RELEASE);
    return ContextInjectionResult::SafeToFallback;
  }

  CONTEXT callCtx = origCtx;

#if defined(_M_X64) || defined(__x86_64__)
  callCtx.Rip = (DWORD64)shellcodeMem;

  // Present a valid Windows x64 call site. The pre-call RSP is 16-byte aligned and the 32 bytes
  // above it are reserved as the mandatory shadow store. The original RSP is restored afterwards.
  callCtx.Rsp = (origCtx.Rsp & ~DWORD64(0xF)) - 0x40;
#else
  callCtx.Eip = (DWORD)(uintptr_t)shellcodeMem;
  callCtx.Esp = (origCtx.Esp & ~DWORD(0xF)) - 0x40;
#endif

  if(!SetThreadContext(hThread, &callCtx))
  {
    RDCERR("Couldn't redirect suspended primary thread: %u", GetLastError());
    VirtualFreeEx(hProcess, shellcodeMem, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, stateMem, 0, MEM_RELEASE);
    return ContextInjectionResult::SafeToFallback;
  }

  // The known primary thread must have exactly the one suspension applied by CREATE_SUSPENDED.
  // If there is another suspension, restore both the context and the count without executing code.
  DWORD previousSuspendCount = ResumeThread(hThread);
  if(previousSuspendCount != 1)
  {
    RDCERR("Unexpected primary thread suspend count %u", previousSuspendCount);

    if(previousSuspendCount == 0)
      return ContextInjectionResult::FailedAfterExecution;

    if(previousSuspendCount == (DWORD)-1)
    {
      if(SetThreadContext(hThread, &origCtx))
      {
        VirtualFreeEx(hProcess, shellcodeMem, 0, MEM_RELEASE);
        VirtualFreeEx(hProcess, stateMem, 0, MEM_RELEASE);
        return ContextInjectionResult::SafeToFallback;
      }

      return ContextInjectionResult::FailedAfterExecution;
    }

    bool restored = SetThreadContext(hThread, &origCtx) != FALSE;
    SuspendThread(hThread);

    if(restored)
    {
      VirtualFreeEx(hProcess, shellcodeMem, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, stateMem, 0, MEM_RELEASE);
      return ContextInjectionResult::SafeToFallback;
    }

    return ContextInjectionResult::FailedAfterExecution;
  }

  // Once resumed, never fall back to another remote call. We wait just as the old
  // CreateRemoteThread path did, but without repeatedly suspending a thread inside LoadLibrary.
  RemoteCallState state = {};
  uint32_t waitIterations = 0;

  for(;;)
  {
    SIZE_T read = 0;
    if(ReadProcessMemory(hProcess, stateMem, &state, sizeof(state), &read) &&
       read == sizeof(state) && state.completed == 1)
      break;

    if(WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0)
    {
      RDCERR("Target exited while executing the remote trampoline");
      return ContextInjectionResult::FailedAfterExecution;
    }

    Sleep(1);

    waitIterations++;
    if(waitIterations % 30000 == 0)
      RDCWARN("Still waiting for the remote trampoline to complete");
  }

  const uintptr_t loopBegin = (uintptr_t)shellcodeMem + loopOffset;
  const uintptr_t loopEnd = (uintptr_t)shellcodeMem + sizeof(shellcode);

  // The completion store immediately precedes the parking loop. Suspend only after observing it,
  // and retry if the thread was caught in the few instructions between the store and the loop.
  for(;;)
  {
    if(SuspendThread(hThread) == (DWORD)-1)
    {
      if(WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0)
        return ContextInjectionResult::FailedAfterExecution;

      Sleep(1);
      continue;
    }

    CONTEXT parkedCtx;
    RDCEraseEl(parkedCtx);
    parkedCtx.ContextFlags = CONTEXT_CONTROL;

    if(!GetThreadContext(hThread, &parkedCtx))
    {
      ResumeThread(hThread);
      Sleep(1);
      continue;
    }

#if defined(_M_X64) || defined(__x86_64__)
    uintptr_t ip = (uintptr_t)parkedCtx.Rip;
#else
    uintptr_t ip = (uintptr_t)parkedCtx.Eip;
#endif

    if(ip < loopBegin || ip >= loopEnd)
    {
      ResumeThread(hThread);
      Sleep(0);
      continue;
    }

    if(!SetThreadContext(hThread, &origCtx))
    {
      RDCERR("Couldn't restore primary thread context: %u", GetLastError());
      // Leave the thread suspended and the trampoline allocated. Resuming or freeing either here
      // would execute invalid state.
      return ContextInjectionResult::FailedAfterExecution;
    }

    if(callResult)
      *callResult = state.result;

    VirtualFreeEx(hProcess, shellcodeMem, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, stateMem, 0, MEM_RELEASE);

    // Deliberately do not resume. This recreates the one CREATE_SUSPENDED count that was borrowed
    // to run the trampoline, so subsequent setup calls and the caller see the original state.
    return ContextInjectionResult::Succeeded;
  }
}

static RemoteInvokeResult InvokeRemoteFunction(HANDLE hProcess, HANDLE suspendedPrimaryThread,
                                               void *remoteFunc, void *remoteArg,
                                               uintptr_t *callResult = NULL)
{
  if(remoteFunc == NULL)
  {
    RDCERR("Cannot invoke a null remote function");
    return RemoteInvokeResult::FailedSafe;
  }

  if(suspendedPrimaryThread != NULL)
  {
    ContextInjectionResult contextResult = InjectViaSetThreadContext(
        hProcess, suspendedPrimaryThread, remoteFunc, remoteArg, callResult);

    if(contextResult == ContextInjectionResult::Succeeded)
      return RemoteInvokeResult::Succeeded;

    if(contextResult == ContextInjectionResult::FailedAfterExecution)
      return RemoteInvokeResult::FailedUnsafe;

    RDCWARN("Context injection could not start safely; using the remote-thread compatibility path");
  }

  HANDLE hThread = CreateRemoteThread(hProcess, NULL, 1024 * 1024U,
                                      (LPTHREAD_START_ROUTINE)remoteFunc, remoteArg, 0, NULL);
  if(hThread == NULL)
  {
    RDCERR("Couldn't create compatibility remote thread: %u", GetLastError());
    return RemoteInvokeResult::FailedSafe;
  }

  DWORD waitResult = WaitForSingleObject(hThread, INFINITE);
  CloseHandle(hThread);

  if(waitResult != WAIT_OBJECT_0)
  {
    RDCERR("Error waiting for compatibility remote thread: %u", GetLastError());
    return RemoteInvokeResult::FailedUnsafe;
  }

  return RemoteInvokeResult::Succeeded;
}

static RemoteInvokeResult InjectDLL(HANDLE hProcess, HANDLE suspendedPrimaryThread, rdcwstr libName)
{
  wchar_t dllPath[MAX_PATH + 1] = {0};

  if(libName.length() > MAX_PATH || wcscpy_s(dllPath, libName.c_str()) != 0)
  {
    RDCERR("DLL path is too long to inject: '%ls'", libName.c_str());
    return RemoteInvokeResult::FailedSafe;
  }

  static HMODULE kernel32 = GetModuleHandleA("kernel32.dll");

  if(kernel32 == NULL)
  {
    RDCERR("Couldn't get handle for kernel32.dll");
    return RemoteInvokeResult::FailedSafe;
  }

  void *remoteMem =
      VirtualAllocEx(hProcess, NULL, sizeof(dllPath), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if(remoteMem)
  {
    SIZE_T written = 0;
    BOOL success =
        WriteProcessMemory(hProcess, remoteMem, (void *)dllPath, sizeof(dllPath), &written);
    if(success && written == sizeof(dllPath))
    {
      void *loadLibAddr = (void *)GetProcAddress(kernel32, "LoadLibraryW");
      uintptr_t module = 0;
      RemoteInvokeResult result =
          InvokeRemoteFunction(hProcess, suspendedPrimaryThread, loadLibAddr, remoteMem, &module);

      if(result != RemoteInvokeResult::FailedUnsafe)
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);

      return result;
    }
    else
    {
      RDCERR("Couldn't write remote memory %p with dllPath '%ls': %u", remoteMem, dllPath,
             GetLastError());
    }

    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
  }
  else
  {
    RDCERR("Couldn't allocate remote memory for DLL '%ls': %u", libName.c_str(), GetLastError());
  }

  return RemoteInvokeResult::FailedSafe;
}

uintptr_t FindRemoteDLL(DWORD pid, rdcstr libName)
{
  HANDLE hModuleSnap = INVALID_HANDLE_VALUE;

  rdcwstr wlibName = StringFormat::UTF82Wide(strlower(libName));

  // up to 10 retries
  for(int i = 0; i < 10; i++)
  {
    hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);

    if(hModuleSnap == INVALID_HANDLE_VALUE)
    {
      DWORD err = GetLastError();

      RDCWARN("CreateToolhelp32Snapshot(%u) -> 0x%08x", pid, err);

      // retry if error is ERROR_BAD_LENGTH
      if(err == ERROR_BAD_LENGTH)
        continue;
    }

    // didn't retry, or succeeded
    break;
  }

  if(hModuleSnap == INVALID_HANDLE_VALUE)
  {
    RDCERR("Couldn't create toolhelp dump of modules in process %u", pid);
    return 0;
  }

  MODULEENTRY32 me32;
  RDCEraseEl(me32);
  me32.dwSize = sizeof(MODULEENTRY32);

  BOOL success = Module32First(hModuleSnap, &me32);

  if(success == FALSE)
  {
    DWORD err = GetLastError();

    RDCERR("Couldn't get first module in process %u: 0x%08x", pid, err);
    CloseHandle(hModuleSnap);
    return 0;
  }

  uintptr_t ret = 0;

  int numModules = 0;

  do
  {
    wchar_t modnameLower[MAX_MODULE_NAME32 + 1];
    RDCEraseEl(modnameLower);
    wcsncpy_s(modnameLower, me32.szModule, MAX_MODULE_NAME32);

    wchar_t *wc = &modnameLower[0];
    while(*wc)
    {
      *wc = towlower(*wc);
      wc++;
    }

    numModules++;

    if(wcsstr(modnameLower, wlibName.c_str()) == modnameLower)
    {
      ret = (uintptr_t)me32.modBaseAddr;
    }
  } while(ret == 0 && Module32Next(hModuleSnap, &me32));

  if(ret == 0)
  {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);

    DWORD exitCode = 0;

    if(h)
      GetExitCodeProcess(h, &exitCode);

    if(h == NULL || exitCode != STILL_ACTIVE)
    {
      RDCERR(
          "Error injecting into remote process with PID %u which is no longer available.\n"
          "Possibly the process has crashed during early startup, or is missing DLLs to run?",
          pid);
    }
    else
    {
      RDCERR("Couldn't find module '%s' among %d modules", libName.c_str(), numModules);
    }

    if(h)
      CloseHandle(h);
  }

  CloseHandle(hModuleSnap);

  return ret;
}

static RemoteInvokeResult InjectFunctionCall(HANDLE hProcess, HANDLE suspendedPrimaryThread,
                                             uintptr_t renderdoc_remote, const char *funcName,
                                             void *data, const size_t dataLen)
{
  if(funcName == NULL || data == NULL || dataLen == 0 || renderdoc_remote == 0)
  {
    RDCERR("Invalid function call injection attempt");
    return RemoteInvokeResult::FailedSafe;
  }

  RDCDEBUG("Injecting call to %s", funcName);

  HMODULE renderdoc_local = GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll");

  if(renderdoc_local == NULL)
  {
    RDCERR("Couldn't get the local %s.dll module", STRINGIZE(RDOC_BASE_NAME));
    return RemoteInvokeResult::FailedSafe;
  }

  uintptr_t func_local = (uintptr_t)GetProcAddress(renderdoc_local, funcName);

  if(func_local == 0)
  {
    RDCERR("Couldn't find local export %s", funcName);
    return RemoteInvokeResult::FailedSafe;
  }

  // we've found SetCaptureOptions in our local instance of the module, now calculate the offset and
  // so get the function
  // in the remote module (which might be loaded at a different base address
  uintptr_t func_remote = func_local + renderdoc_remote - (uintptr_t)renderdoc_local;

  void *remoteMem = VirtualAllocEx(hProcess, NULL, dataLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if(remoteMem == NULL)
  {
    RDCERR("Couldn't allocate remote parameter memory for %s: %u", funcName, GetLastError());
    return RemoteInvokeResult::FailedSafe;
  }

  SIZE_T numWritten = 0;
  if(!WriteProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten) || numWritten != dataLen)
  {
    RDCERR("Couldn't write complete remote parameters for %s: %u", funcName, GetLastError());
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return RemoteInvokeResult::FailedSafe;
  }

  RemoteInvokeResult result =
      InvokeRemoteFunction(hProcess, suspendedPrimaryThread, (void *)func_remote, remoteMem);

  if(result == RemoteInvokeResult::Succeeded)
  {
    SIZE_T numRead = 0;
    if(!ReadProcessMemory(hProcess, remoteMem, data, dataLen, &numRead) || numRead != dataLen)
    {
      RDCERR("Couldn't read complete remote parameters for %s: %u", funcName, GetLastError());
      result = RemoteInvokeResult::FailedSafe;
    }
  }

  if(result != RemoteInvokeResult::FailedUnsafe)
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);

  return result;
}

static PROCESS_INFORMATION RunProcess(const rdcstr &app, const rdcstr &workingDir,
                                      const rdcstr &cmdLine,
                                      const rdcarray<EnvironmentModification> &env, bool internal,
                                      HANDLE *phChildStdOutput_Rd, HANDLE *phChildStdError_Rd)
{
  PROCESS_INFORMATION pi;
  STARTUPINFO si;
  SECURITY_ATTRIBUTES pSec;
  SECURITY_ATTRIBUTES tSec;

  RDCEraseEl(pi);
  RDCEraseEl(si);
  RDCEraseEl(pSec);
  RDCEraseEl(tSec);

  si.cb = sizeof(si);

  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  rdcwstr workdir = L"";

  if(!workingDir.empty())
    workdir = StringFormat::UTF82Wide(workingDir);
  else
    workdir = StringFormat::UTF82Wide(get_dirname(app));

  wchar_t *paramsAlloc = NULL;

  rdcwstr wapp = StringFormat::UTF82Wide(app);

  // CreateProcessW can modify the params, need space.
  size_t len = wapp.length() + 10;

  rdcwstr wcmd = L"";

  if(!cmdLine.empty())
  {
    wcmd = StringFormat::UTF82Wide(cmdLine);
    len += wcmd.length();
  }

  paramsAlloc = new wchar_t[len];

  RDCEraseMem(paramsAlloc, len * sizeof(wchar_t));

  wcscpy_s(paramsAlloc, len, L"\"");
  wcscat_s(paramsAlloc, len, wapp.c_str());
  wcscat_s(paramsAlloc, len, L"\"");

  if(!cmdLine.empty())
  {
    wcscat_s(paramsAlloc, len, L" ");
    wcscat_s(paramsAlloc, len, wcmd.c_str());
  }

  bool inheritHandles = false;

  HANDLE hChildStdOutput_Wr = 0, hChildStdError_Wr = 0;
  if(phChildStdOutput_Rd)
  {
    RDCASSERT(phChildStdError_Rd);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if(!CreatePipe(phChildStdOutput_Rd, &hChildStdOutput_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdOutput_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    if(!CreatePipe(phChildStdError_Rd, &hChildStdError_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdError_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hChildStdOutput_Wr;
    si.hStdError = hChildStdError_Wr;

    // Need to inherit handles in CreateProcess for ReadFile to read stdout
    inheritHandles = true;
  }

  // if it's a utility launch, hide the command prompt window from showing
  if(phChildStdOutput_Rd || internal)
    si.dwFlags |= STARTF_USESHOWWINDOW;

  if(!internal)
    RDCLOG("Running process %s", app.c_str());

  // turn environment string to a UTF-8 map
  std::wstring envString;

  if(!env.empty())
  {
    LPWCH envStrings = GetEnvironmentStringsW();
    EnvMap envValues = EnvStringToEnvMap(envStrings);
    FreeEnvironmentStringsW(envStrings);

    ApplyEnvModifications(envValues, env, false);

    for(auto it = envValues.begin(); it != envValues.end(); ++it)
    {
      envString += StringFormat::UTF82Wide(it->first).c_str();
      envString += L"=";
      envString += StringFormat::UTF82Wide(it->second).c_str();
      envString.push_back(0);
    }
  }

  BOOL retValue = CreateProcessW(
      NULL, paramsAlloc, &pSec, &tSec, inheritHandles, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
      envString.empty() ? NULL : (void *)envString.data(), workdir.c_str(), &si, &pi);

  DWORD err = GetLastError();

  if(phChildStdOutput_Rd)
  {
    CloseHandle(hChildStdOutput_Wr);
    CloseHandle(hChildStdError_Wr);
  }

  SAFE_DELETE_ARRAY(paramsAlloc);

  if(!retValue)
  {
    if(!internal)
      RDCWARN("Process %s could not be loaded (error %d).", app.c_str(), err);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    RDCEraseEl(pi);
  }

  return pi;
}

static rdcpair<RDResult, uint32_t> InjectIntoProcessInternal(
    uint32_t pid, const rdcarray<EnvironmentModification> &env, const rdcstr &capturefile,
    const CaptureOptions &opts, bool waitForExit, HANDLE suspendedPrimaryThread)
{
  rdcwstr wcapturefile = StringFormat::UTF82Wide(capturefile);

  HANDLE hProcess =
      OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                      PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                  FALSE, pid);

  if(opts.delayForDebugger > 0)
  {
    RDCDEBUG("Waiting for debugger attach to %lu", pid);
    uint32_t timeout = 0;

    BOOL debuggerAttached = FALSE;

    while(!debuggerAttached)
    {
      CheckRemoteDebuggerPresent(hProcess, &debuggerAttached);

      Sleep(10);
      timeout += 10;

      if(timeout > opts.delayForDebugger * 1000)
        break;
    }

    if(debuggerAttached)
      RDCDEBUG("Debugger attach detected after %.2f s", float(timeout) / 1000.0f);
    else
      RDCDEBUG("Timed out waiting for debugger, gave up after %u s", opts.delayForDebugger);
  }

  RDCLOG("Injecting renderdoc into process %lu", pid);

  wchar_t renderdocPath[MAX_PATH] = {0};
  GetModuleFileNameW(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), &renderdocPath[0],
                                      MAX_PATH - 1);

  wchar_t renderdocPathLower[MAX_PATH] = {0};
  memcpy(renderdocPathLower, renderdocPath, MAX_PATH * sizeof(wchar_t));
  for(size_t i = 0; i < MAX_PATH && renderdocPathLower[i]; i++)
  {
    // lowercase
    if(renderdocPathLower[i] >= 'A' && renderdocPathLower[i] <= 'Z')
      renderdocPathLower[i] = 'a' + char(renderdocPathLower[i] - 'A');

    // normalise paths
    if(renderdocPathLower[i] == '/')
      renderdocPathLower[i] = '\\';
  }

  BOOL isWow64 = FALSE;
  BOOL success = IsWow64Process(hProcess, &isWow64);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of process, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  bool capalt = false;

#if DISABLED(RDOC_X64)
  BOOL selfWow64 = FALSE;

  HANDLE hSelfProcess = GetCurrentProcess();

  // check to see if we're a WoW64 process
  success = IsWow64Process(hSelfProcess, &selfWow64);

  CloseHandle(hSelfProcess);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of self, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  // we know we're 32-bit, so if the target process is not wow64
  // and we are, it's 64-bit. If we're both not wow64 then we're
  // running on 32-bit windows, and if we're both wow64 then we're
  // both 32-bit on 64-bit windows.
  //
  // We don't support capturing 64-bit programs from a 32-bit install
  // because it's pointless - a 64-bit install will work for all in
  // that case. But we do want to handle the case of:
  // 64-bit renderdoc -> 32-bit program (via 32-bit renderdoccmd)
  //    -> 64-bit program (going back to 64-bit renderdoccmd).
  // so we try to see if we're an x86 invoked renderdoccmd in an
  // otherwise 64-bit install, and 'promote' back to 64-bit.
  if(selfWow64 && !isWow64)
  {
    wchar_t *slash = wcsrchr(renderdocPath, L'\\');

    if(slash && slash > renderdocPath + 4)
    {
      slash -= 4;

      if(slash && !wcsncmp(slash, L"\\x86", 4))
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    if(!capalt)
    {
      const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
      if(!devLocation)
        devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if we couldn't promote, then bail out.
    if(!capalt)
    {
      RDCDEBUG("Running from %ls", renderdocPathLower);

      CloseHandle(hProcess);
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                       "Can't capture 64-bit program with 32-bit build of RenderDoc. Please run a "
                       "64-bit build of RenderDoc");
      return {result, 0};
    }
  }
#else
  // farm off to alternate bitness renderdoccmd.exe

  // if the target process is 'wow64' that means it's 32-bit.
  capalt = (isWow64 == TRUE);
#endif

  if(capalt)
  {
#if ENABLED(RDOC_X64)
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\x64\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\Win32\\Development\\rendertestcmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\x64\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

        wcscat_s(renderdocPath, L"\\Win32\\Release\\rendertestcmd.exe");
      }
    }

    if(!devLocation)
    {
      // look in a subfolder for x86.

      // remove the filename from the path
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\x86\\rendertestcmd.exe");
    }
#else
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\x64\\Development\\rendertestcmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

        wcscat_s(renderdocPath, L"\\x64\\Release\\rendertestcmd.exe");
      }
    }

    if(!devLocation)
    {
      // look upwards on 32-bit to find the parent renderdoccmd.
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      // remove the filename
      if(slash)
        *slash = 0;

      // remove the \\x86
      slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\rendertestcmd.exe");
    }
#endif

    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    SECURITY_ATTRIBUTES pSec;
    SECURITY_ATTRIBUTES tSec;

    RDCEraseEl(pi);
    RDCEraseEl(si);
    RDCEraseEl(pSec);
    RDCEraseEl(tSec);

    // hide the console window
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    pSec.nLength = sizeof(pSec);
    tSec.nLength = sizeof(tSec);

    // If this is a cross-bitness launch, pass the already-suspended target primary thread to the
    // alternate-bitness helper. Restrict inheritance to this single duplicate so the helper does
    // not accidentally receive unrelated handles from the UI or captured process.
    HANDLE inheritedPrimaryThread = NULL;
    STARTUPINFOEXW extendedSi;
    RDCEraseEl(extendedSi);
    unsigned char *attributeStorage = NULL;
    bool attributeListInitialised = false;
    LPSTARTUPINFOW startupInfo = &si;
    DWORD helperCreationFlags = CREATE_NEW_CONSOLE | CREATE_SUSPENDED;
    BOOL inheritHandles = FALSE;

    if(suspendedPrimaryThread != NULL)
    {
      if(!DuplicateHandle(GetCurrentProcess(), suspendedPrimaryThread, GetCurrentProcess(),
                          &inheritedPrimaryThread, 0, TRUE, DUPLICATE_SAME_ACCESS))
      {
        RDCWARN("Couldn't duplicate suspended primary thread for alternate-bitness helper: %u",
                GetLastError());
      }
      else
      {
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &attributeBytes);

        if(attributeBytes > 0)
        {
          attributeStorage = new unsigned char[attributeBytes];
          extendedSi.StartupInfo = si;
          extendedSi.StartupInfo.cb = sizeof(extendedSi);
          extendedSi.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)(void *)attributeStorage;

          attributeListInitialised = InitializeProcThreadAttributeList(
                                         extendedSi.lpAttributeList, 1, 0, &attributeBytes) != FALSE;

          if(attributeListInitialised &&
             UpdateProcThreadAttribute(extendedSi.lpAttributeList, 0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inheritedPrimaryThread,
                                       sizeof(inheritedPrimaryThread), NULL, NULL))
          {
            startupInfo = &extendedSi.StartupInfo;
            helperCreationFlags |= EXTENDED_STARTUPINFO_PRESENT;
            inheritHandles = TRUE;
          }
          else
          {
            RDCWARN("Couldn't restrict handle inheritance for alternate-bitness helper: %u",
                    GetLastError());

            if(attributeListInitialised)
              DeleteProcThreadAttributeList(extendedSi.lpAttributeList);

            SAFE_DELETE_ARRAY(attributeStorage);
            extendedSi.lpAttributeList = NULL;
            attributeListInitialised = false;
            CloseHandle(inheritedPrimaryThread);
            inheritedPrimaryThread = NULL;
          }
        }
        else
        {
          RDCWARN("Couldn't size handle inheritance list for alternate-bitness helper: %u",
                  GetLastError());
          CloseHandle(inheritedPrimaryThread);
          inheritedPrimaryThread = NULL;
        }
      }
    }

    // serialise to string with two chars per byte
    rdcstr optstr = opts.EncodeAsString();

    wchar_t *paramsAlloc = new wchar_t[2048];

    rdcstr debugLogfile = RDCGETLOGFILE();
    rdcwstr wdebugLogfile = StringFormat::UTF82Wide(debugLogfile);

    _snwprintf_s(
        paramsAlloc, 2047, 2047,
        L"\"%ls\" capaltbit --pid=%u --thread-handle=%llu --capfile=\"%ls\" --debuglog=\"%ls\" "
        L"--capopts=\"%hs\"",
        renderdocPath, pid, (unsigned long long)(uintptr_t)inheritedPrimaryThread,
        wcapturefile.c_str(), wdebugLogfile.c_str(), optstr.c_str());

    RDCDEBUG("params %ls", paramsAlloc);

    paramsAlloc[2047] = 0;

    wchar_t *commandLine = paramsAlloc;

    std::wstring cmdWithEnv;

    if(!env.empty())
    {
      cmdWithEnv = paramsAlloc;

      for(const EnvironmentModification &e : env)
      {
        rdcstr name = e.name.trimmed();
        rdcstr value = e.value;

        if(name == "")
          break;

        cmdWithEnv += L" +env-";
        switch(e.mod)
        {
          case EnvMod::Set: cmdWithEnv += L"replace"; break;
          case EnvMod::Append: cmdWithEnv += L"append"; break;
          case EnvMod::Prepend: cmdWithEnv += L"prepend"; break;
        }

        if(e.mod != EnvMod::Set)
        {
          switch(e.sep)
          {
            case EnvSep::Platform: cmdWithEnv += L"-platform"; break;
            case EnvSep::SemiColon: cmdWithEnv += L"-semicolon"; break;
            case EnvSep::Colon: cmdWithEnv += L"-colon"; break;
            case EnvSep::NoSep: break;
          }
        }

        cmdWithEnv += L" ";

        // escape the parameters
        for(size_t it = 0; it < name.size(); it++)
        {
          if(name[it] == '"')
          {
            name.insert(it, '\\');
            it++;
          }
        }

        for(size_t it = 0; it < value.size(); it++)
        {
          if(value[it] == '"')
          {
            value.insert(it, '\\');
            it++;
          }
        }

        if(name.back() == '\\')
          name += "\\";

        if(value.back() == '\\')
          value += "\\";

        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(name).c_str()) + L"\" ";
        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(value).c_str()) + L"\" ";
      }

      commandLine = (wchar_t *)cmdWithEnv.c_str();
    }

    BOOL retValue = CreateProcessW(NULL, commandLine, &pSec, &tSec, inheritHandles,
                                   helperCreationFlags, NULL, NULL, startupInfo, &pi);

    if(attributeListInitialised)
      DeleteProcThreadAttributeList(extendedSi.lpAttributeList);

    SAFE_DELETE_ARRAY(attributeStorage);

    if(inheritedPrimaryThread)
      CloseHandle(inheritedPrimaryThread);

    SAFE_DELETE_ARRAY(paramsAlloc);

    if(!retValue)
    {
      RDResult result;
#if RENDERDOC_OFFICIAL_BUILD
      SET_ERROR_RESULT(result, ResultCode::InternalError,
                       "Can't run 32-bit renderdoccmd to capture 32-bit program.");
#else
      SET_ERROR_RESULT(
          result, ResultCode::InternalError,
          "Can't run 32-bit renderdoccmd to capture 32-bit program."
          "If this is a locally built RenderDoc you must build both 32-bit and 64-bit versions.");
#endif
      CloseHandle(hProcess);
      return {result, 0};
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hThread, INFINITE);
    CloseHandle(pi.hThread);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    if(waitForExit)
      WaitForSingleObject(hProcess, INFINITE);

    CloseHandle(hProcess);

    if(exitCode == 0)
    {
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::UnknownError,
                       "Encountered error while launching target 32-bit program.");
      return {result, 0};
    }

    if(exitCode < RenderDoc_FirstTargetControlPort)
    {
      ResultCode code = (ResultCode)exitCode;

      RDResult result;
      SET_ERROR_RESULT(result, code, "32-bit renderdoccmd returned '%s'", ToStr(code).c_str());
      return {code, 0};
    }

    return {ResultCode::Succeeded, (uint32_t)exitCode};
  }

  const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);
  rdcpair<RDResult, uint32_t> result = {ResultCode::Succeeded, 0};

  RemoteInvokeResult dllResult = InjectDLL(hProcess, suspendedPrimaryThread, renderdocPath);

  if(dllResult != RemoteInvokeResult::Succeeded)
  {
    SET_ERROR_RESULT(result.first, ResultCode::InjectionFailed,
                     "Failed to execute the remote LoadLibraryW call for %s.dll.", rdoc_dll);
  }
  else
  {
    uintptr_t loc = FindRemoteDLL(pid, STRINGIZE(RDOC_BASE_NAME) ".dll");

    if(loc == 0)
    {
      SET_ERROR_RESULT(
          result.first, ResultCode::InjectionFailed,
          "Failed to inject %s.dll into process. Check that the process did not crash or exit "
          "early in initialisation, e.g. if the working directory is incorrectly set.",
          rdoc_dll);
    }
    else
    {
      auto invoke = [&](const char *funcName, void *data, size_t dataLen) {
        RemoteInvokeResult invokeResult =
            InjectFunctionCall(hProcess, suspendedPrimaryThread, loc, funcName, data, dataLen);

        if(invokeResult == RemoteInvokeResult::Succeeded)
          return true;

        SET_ERROR_RESULT(result.first, ResultCode::InjectionFailed,
                         "Remote initialisation call '%s' failed.", funcName);
        return false;
      };

      bool callsSucceeded = true;

      // safe to cast away const as these functions don't modify the parameters
      if(!capturefile.empty())
        callsSucceeded =
            invoke("INTERNAL_SetCaptureFile", (void *)capturefile.c_str(), capturefile.size() + 1);

      rdcstr debugLogfile = RDCGETLOGFILE();

      if(callsSucceeded)
        callsSucceeded = invoke("INTERNAL_SetDebugLogFile", (void *)debugLogfile.c_str(),
                                debugLogfile.size() + 1);

      if(callsSucceeded)
        callsSucceeded =
            invoke("INTERNAL_SetCaptureOptions", (CaptureOptions *)&opts, sizeof(CaptureOptions));

      if(callsSucceeded)
        callsSucceeded =
            invoke("INTERNAL_GetTargetControlIdent", &result.second, sizeof(result.second));

      if(callsSucceeded && !env.empty())
      {
        for(const EnvironmentModification &e : env)
        {
          rdcstr name = e.name.trimmed();
          rdcstr value = e.value;
          EnvMod mod = e.mod;
          EnvSep sep = e.sep;

          if(name == "")
            break;

          callsSucceeded = invoke("INTERNAL_EnvModName", (void *)name.c_str(), name.size() + 1) &&
                           invoke("INTERNAL_EnvModValue", (void *)value.c_str(), value.size() + 1) &&
                           invoke("INTERNAL_EnvSep", &sep, sizeof(sep)) &&
                           invoke("INTERNAL_EnvMod", &mod, sizeof(mod));

          if(!callsSucceeded)
            break;
        }

        if(callsSucceeded)
        {
          // parameter is unused
          void *dummy = NULL;
          callsSucceeded = invoke("INTERNAL_ApplyEnvMods", &dummy, sizeof(dummy));
        }
      }
    }
  }

  if(waitForExit)
    WaitForSingleObject(hProcess, INFINITE);

  CloseHandle(hProcess);

  return result;
}

rdcpair<RDResult, uint32_t> Process::InjectIntoProcess(uint32_t pid,
                                                       const rdcarray<EnvironmentModification> &env,
                                                       const rdcstr &capturefile,
                                                       const CaptureOptions &opts, bool waitForExit)
{
  // Attaching to an arbitrary running process retains the compatibility path. Thread-context
  // injection is only used when LaunchAndInjectIntoProcess can provide the known suspended primary
  // thread that RenderDoc owns.
  return InjectIntoProcessInternal(pid, env, capturefile, opts, waitForExit, NULL);
}

rdcpair<RDResult, uint32_t> Process::InjectIntoSuspendedProcess(
    uint32_t pid, HANDLE suspendedPrimaryThread, const rdcarray<EnvironmentModification> &env,
    const rdcstr &capturefile, const CaptureOptions &opts, bool waitForExit)
{
  return InjectIntoProcessInternal(pid, env, capturefile, opts, waitForExit, suspendedPrimaryThread);
}

uint32_t Process::LaunchProcess(const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
                                bool internal, ProcessResult *result)
{
  HANDLE hChildStdOutput_Rd = NULL, hChildStdError_Rd = NULL;

  rdcstr appPath = app;
  size_t len = appPath.length();
  rdcstr ext;
  if(len > 4)
    ext = strlower(appPath.substr(len - 4));
  if(ext != ".exe")
    appPath += ".exe";

  PROCESS_INFORMATION pi =
      RunProcess(appPath, workingDir, cmdLine, {}, internal, result ? &hChildStdOutput_Rd : NULL,
                 result ? &hChildStdError_Rd : NULL);

  if(pi.dwProcessId == 0)
  {
    if(!internal)
      RDCWARN("Couldn't launch process '%s'", appPath.c_str());

    if(hChildStdError_Rd != NULL)
      CloseHandle(hChildStdError_Rd);
    if(hChildStdOutput_Rd != NULL)
      CloseHandle(hChildStdOutput_Rd);

    return 0;
  }

  if(!internal)
    RDCLOG("Launched process '%s' with '%s'", appPath.c_str(), cmdLine.c_str());

  ResumeThread(pi.hThread);

  if(result)
  {
    result->strStdout = "";
    result->strStderror = "";

    char chBuf[4096];
    DWORD dwOutputRead, dwErrorRead;
    BOOL success = FALSE;
    rdcstr s;
    for(;;)
    {
      success = ReadFile(hChildStdOutput_Rd, chBuf, sizeof(chBuf), &dwOutputRead, NULL);
      s = rdcstr(chBuf, dwOutputRead);
      result->strStdout += s;

      if(!success && !dwOutputRead)
        break;
    }

    for(;;)
    {
      success = ReadFile(hChildStdError_Rd, chBuf, sizeof(chBuf), &dwErrorRead, NULL);
      s = rdcstr(chBuf, dwErrorRead);
      result->strStderror += s;

      if(!success && !dwErrorRead)
        break;
    }

    CloseHandle(hChildStdOutput_Rd);
    CloseHandle(hChildStdError_Rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, (LPDWORD)&result->retCode);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  return pi.dwProcessId;
}

uint32_t Process::LaunchScript(const rdcstr &script, const rdcstr &workingDir,
                               const rdcstr &argList, bool internal, ProcessResult *result)
{
  // Change parameters to invoke command interpreter
  rdcstr args = "/C " + script + " " + argList;

  return LaunchProcess("cmd.exe", workingDir, args, internal, result);
}

rdcpair<RDResult, uint32_t> Process::LaunchAndInjectIntoProcess(
    const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
    const rdcarray<EnvironmentModification> &env, const rdcstr &capturefile,
    const CaptureOptions &opts, bool waitForExit)
{
  void *func =
      GetProcAddress(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), "INTERNAL_SetCaptureFile");

  if(func == NULL)
  {
    const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InternalError,
                     "Can't find required export function in %s.dll - corrupted/missing file?",
                     rdoc_dll);
    return {result, 0};
  }

  if(get_basename(app) == "explorer.exe" || get_basename(app) == "dllhost.exe")
  {
    RDResult result;
    SET_ERROR_RESULT(
        result, ResultCode::InjectionFailed,
        "For safety reasons RenderDoc does not support capturing executables with a "
        "reserved system filename such as '%s'. Please rename your executable to capture.",
        get_basename(app).c_str());
    return {result, 0};
  }

  PROCESS_INFORMATION pi = RunProcess(app, workingDir, cmdLine, env, false, NULL, NULL);

  if(pi.dwProcessId == 0)
  {
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InjectionFailed, "Failed to launch process.");
    return {result, 0};
  }

  rdcpair<RDResult, uint32_t> ret =
      InjectIntoProcessInternal(pi.dwProcessId, {}, capturefile, opts, false, pi.hThread);

  CloseHandle(pi.hProcess);
  ResumeThread(pi.hThread);
  ResumeThread(pi.hThread);

  if(ret.second == 0 || ret.first != ResultCode::Succeeded)
  {
    CloseHandle(pi.hThread);
    return ret;
  }

  if(waitForExit)
    WaitForSingleObject(pi.hThread, INFINITE);

  CloseHandle(pi.hThread);

  return ret;
}

bool Process::CanGlobalHook()
{
  // all we need is admin rights and it's the caller's responsibility to ensure that.
  return true;
}

// to simplify the below code, rather than splitting by 32-bit/64-bit we split by native and Wow32.
// This means that for 32-bit code (whether it's on 32-bit OS or not) we just have native, and the
// Wow32 stuff is empty/unused. For 64-bit we use both. Thus the native registry key is always the
// same path regardless of the bitness we're running as and we don't have to move things around or
// have conditionals all over

struct GlobalHookData
{
  struct
  {
    HANDLE pipe = NULL;
    DWORD appinitEnabled = 0;
    rdcwstr appinitDLLs;
  } dataNative, dataWow32;

  int32_t finished = 0;
  Threading::ThreadHandle pipeThread = 0;
};

// utility function to close the registry keys, print an error, and quit
static RDResult HandleRegError(HKEY keyNative, HKEY keyWow32, LSTATUS ret, const char *msg)
{
  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  RDCLOG("Error with AppInit registry keys - %s (%d)", msg, ret);

  RETURN_ERROR_RESULT(ResultCode::InjectionFailed,
                      "Error updating registry to enable global hook.\n"
                      "Check that RenderDoc is correctly running as administrator.");
}

#define REG_CHECK(msg)                                    \
  if(ret != ERROR_SUCCESS)                                \
  {                                                       \
    return HandleRegError(keyNative, keyWow32, ret, msg); \
  }

// function to backup the previous settings for AppInit, then enable it and write our own paths.
RDResult BackupAndChangeRegistry(GlobalHookData &hookdata, const rdcstr &shimpathWow32,
                                 const rdcstr &shimpathNative)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;

  // AppInit_DLLs requires short paths, but short paths can be disabled globally or on a per-volume
  // level. If short paths are disabled we'll get the long path back, we *always* expect the path to
  // get shorter because the shim filename is bigger than 8.3.

  DWORD nativeShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), NULL,
                                            (DWORD)shimpathNative.length());
  if(nativeShortSize == (DWORD)shimpathNative.length() + 1)
  {
    RETURN_ERROR_RESULT(
        ResultCode::FileIOFailed,
        "RenderDoc is installed on a volume or system that has short paths disabled.\n"
        "For the global hook, short paths must be enabled where RenderDoc is installed.");
  }

  if(!shimpathWow32.empty())
  {
    DWORD wow32ShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), NULL,
                                             (DWORD)shimpathWow32.length());

    if(wow32ShortSize == (DWORD)shimpathWow32.length() + 1)
    {
      RETURN_ERROR_RESULT(
          ResultCode::FileIOFailed,
          "RenderDoc is installed on a volume or system that has short paths disabled.\n"
          "For the global hook, short paths must be enabled where RenderDoc is installed.");
    }
  }

  // open the native key
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

  // if we are doing Wow32, open that key as well
  if(!shimpathWow32.empty())
  {
    ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
                          0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

    REG_CHECK("Could not open AppInit key");
  }

  const DWORD one = 1;

  // fetch the previous data for LoadAppInit_DLLs and AppInit_DLLs
  DWORD sz = 4;
  ret = RegGetValueA(keyNative, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                     (void *)&hookdata.dataNative.appinitEnabled, &sz);
  REG_CHECK("Could not fetch LoadAppInit_DLLs");

  sz = 0;
  ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
  if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
  {
    hookdata.dataNative.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
    ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                       hookdata.dataNative.appinitDLLs.data(), &sz);
  }
  REG_CHECK("Could not fetch AppInit_DLLs");

  // set DWORD:1 for LoadAppInit_DLLs and convert our path to a short path then set it
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  rdcwstr shortpath(shimpathNative.size());
  GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), shortpath.data(),
                    (DWORD)shortpath.length());

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                       DWORD(shortpath.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we're doing Wow32, repeat the process for those keys
  if(keyWow32)
  {
    sz = 4;
    ret = RegGetValueA(keyWow32, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                       (void *)&hookdata.dataWow32.appinitEnabled, &sz);
    REG_CHECK("Could not fetch LoadAppInit_DLLs");

    sz = 0;
    ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
    if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
    {
      hookdata.dataWow32.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
      ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                         hookdata.dataWow32.appinitDLLs.data(), &sz);
    }
    REG_CHECK("Could not fetch AppInit_DLLs");

    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    shortpath = rdcwstr(shimpathWow32.size());
    GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), shortpath.data(),
                      (DWORD)shortpath.length());

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                         DWORD(shortpath.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }

  std::wstring backup;

  // write a .reg file that contains the previous settings, so that if all else fails the user can
  // manually insert it back into the registry to restore everything.
  backup += L"Windows Registry Editor Version 5.00\n";
  backup += L"\n";
  backup += L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows]\n";
  backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
  backup += (hookdata.dataNative.appinitEnabled ? L"1\n" : L"0\n");
  backup += L"\"AppInit_DLLs\"=\"";
  // we append with the C string so we don't add trailing NULLs into the text.
  backup += hookdata.dataNative.appinitDLLs.c_str();
  backup += L"\"\n";
  if(keyWow32)
  {
    backup += L"\n";
    backup +=
        L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\"
        L"Windows NT\\CurrentVersion\\Windows]\n";
    backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
    backup += (hookdata.dataWow32.appinitEnabled ? L"1\n" : L"0\n");
    backup += L"\"AppInit_DLLs\"=\"";
    backup += hookdata.dataWow32.appinitDLLs.c_str();
    backup += L"\"\n";
  }

  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  keyNative = keyWow32 = NULL;

  // write it to disk but don't fail if we can't, just print it to the log and keep going.
  wchar_t reg_backup[MAX_PATH];
  GetTempPathW(MAX_PATH, reg_backup);
  wcscat_s(reg_backup, L"RenderDoc_RestoreGlobalHook.reg");

  FILE *f = NULL;
  _wfopen_s(&f, reg_backup, L"w");
  if(f)
  {
    fputws(backup.c_str(), f);
    fclose(f);
  }
  else
  {
    RDCERR("Error opening registry backup file %ls", reg_backup);
    RDCERR("Backup registry data is:\n\n%ls\n\n", backup.c_str());
  }

  return RDResult();
}

// switch error-handling to print-and-continue, as we can't really do anything about it at this
// point and we want to continue restoring in case only one thing failed.
#undef REG_CHECK
#define REG_CHECK(msg)                                                      \
  if(ret != ERROR_SUCCESS)                                                  \
  {                                                                         \
    HandleRegError(keyNative, keyWow32, ret, "Could not open AppInit key"); \
  }

void RestoreRegistry(const GlobalHookData &hookdata)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

#if ENABLED(RDOC_X64)
  ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                        "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0,
                        NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

  REG_CHECK("Could not open AppInit key");
#endif

  // set the native values back to where they were
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD,
                       (const BYTE *)&hookdata.dataNative.appinitEnabled,
                       sizeof(hookdata.dataNative.appinitEnabled));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ,
                       (const BYTE *)hookdata.dataNative.appinitDLLs.c_str(),
                       DWORD(hookdata.dataNative.appinitDLLs.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we opened it, restore the Wow32 values as well
  if(keyWow32)
  {
    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD,
                         (const BYTE *)&hookdata.dataWow32.appinitEnabled,
                         sizeof(hookdata.dataWow32.appinitEnabled));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ,
                         (const BYTE *)hookdata.dataWow32.appinitDLLs.c_str(),
                         DWORD(hookdata.dataWow32.appinitDLLs.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }
}

static GlobalHookData *globalHook = NULL;

// a thread we run in the background just to keep the pipes open and wait until we're ready to stop
// the global hook.
static void GlobalHookThread()
{
  Threading::SetCurrentThreadName("GlobalHookThread");

  // keep looping doing an atomic compare-exchange to check that finished is still 0
  while(Atomic::CmpExch32(&globalHook->finished, 0, 0) == 0)
  {
    // wake every quarter of a second to test again
    Threading::Sleep(250);
  }

  char exitData[32] = "exit";

  // write some data into the pipe and close it. The data is (currently) unimportant, just that it
  // causes the blocking read on the other end to succeed and close the program.
  DWORD dummy = 0;
  if(globalHook->dataNative.pipe)
  {
    WriteFile(globalHook->dataNative.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataNative.pipe);
  }

  if(globalHook->dataWow32.pipe)
  {
    WriteFile(globalHook->dataWow32.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataWow32.pipe);
  }
}

RDResult Process::StartGlobalHook(const rdcstr &pathmatch, const rdcstr &capturefile,
                                  const CaptureOptions &opts)
{
  if(pathmatch.empty())
  {
    RETURN_ERROR_RESULT(ResultCode::InvalidParameter,
                        "Invalid global hook parameter, empty path to match");
  }

  rdcstr renderdocPath;
  FileIO::GetLibraryFilename(renderdocPath);

  renderdocPath = get_dirname(renderdocPath);

  // the native renderdoccmd.exe is always next to the dll. Wow32 will be somewhere else
  rdcstr cmdpathNative = renderdocPath + "\\rendertestcmd.exe";
  rdcstr cmdpathWow32;

  rdcstr shimpathNative = renderdocPath;
  rdcstr shimpathWow32;

#if ENABLED(RDOC_X64)

  // native shim is just renderdocshim64.dll
  shimpathNative = renderdocPath + "\\rendertestshim64.dll";

  // if it looks like we're in the development environment, look for the alternate bitness in the
  // corresponding folder
  int devLocation = renderdocPath.find("\\x64\\Development");
  if(devLocation >= 0)
  {
    renderdocPath.erase(devLocation, ~0U);

    shimpathWow32 = renderdocPath + "\\Win32\\Development\\rendertestshim32.dll";
    cmdpathWow32 = renderdocPath + "\\Win32\\Development\\rendertestcmd.exe";
  }
  else
  {
    devLocation = renderdocPath.find("\\x64\\Release");

    if(devLocation >= 0)
    {
      renderdocPath.erase(devLocation, ~0U);

      shimpathWow32 = renderdocPath + "\\Win32\\Release\\rendertestshim32.dll";
      cmdpathWow32 = renderdocPath + "\\Win32\\Release\\rendertestcmd.exe";
    }
  }

  // if we're not in the dev environment, assume it's under a x86\ subfolder
  if(devLocation < 0)
  {
    shimpathWow32 = renderdocPath + "\\x86\\rendertestshim32.dll";
    cmdpathWow32 = renderdocPath + "\\x86\\rendertestcmd.exe";
  }

#else

  // nothing fancy to do here for 32-bit, just point the shim next to our dll.
  shimpathNative = renderdocPath + "\\rendertestshim32.dll";

#endif

  GlobalHookData hookdata;

  // try to backup and change the registry settings to start loading our shim dlls. If that fails,
  // we bail out immediately
  RDResult regStatus = BackupAndChangeRegistry(hookdata, shimpathWow32, shimpathNative);
  if(regStatus != ResultCode::Succeeded)
    return regStatus;

  PROCESS_INFORMATION pi = {0};
  STARTUPINFO si = {0};
  SECURITY_ATTRIBUTES pSec = {0};
  SECURITY_ATTRIBUTES tSec = {0};
  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  si.cb = sizeof(si);

  // serialise to string with two chars per byte
  rdcstr optstr = opts.EncodeAsString();
  rdcstr debugLogfile = RDCGETLOGFILE();

  rdcstr params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathNative.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  rdcwstr paramsAlloc = StringFormat::UTF82Wide(params);

  // we'll be setting stdin
  si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;

  // hide the console window
  si.wShowWindow = SW_HIDE;

  // this is the end of the pipe that the child will inherit and use as stdin
  HANDLE childEnd = NULL;

  DWORD err;

  // create a pipe with the writing end for us, and the reading end as the child process's stdin
  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataNative.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 32-bit stdin pipe (err %u)",
                          err);
    }

    // we don't want the child process to inherit our end
    res = SetHandleInformation(hookdata.dataNative.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 32-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  // launch the process
  BOOL retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE,
                                 NULL, NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore, the child has it
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch renderdoccmd from '%s' (err %u)",
                        cmdpathNative.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  RDCEraseEl(pi);

// repeat the process for the Wow32 renderdoccmd
#if ENABLED(RDOC_X64)
  params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathWow32.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  paramsAlloc = StringFormat::UTF82Wide(params);

  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataWow32.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 64-bit stdin pipe (err %u)",
                          err);
    }

    res = SetHandleInformation(hookdata.dataWow32.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 64-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE, NULL,
                            NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    CloseHandle(hookdata.dataWow32.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch renderdoccmd from '%s' (err %u)",
                        cmdpathWow32.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
#endif

  // set static global pointer with our data, and launch the thread
  globalHook = new GlobalHookData;
  *globalHook = hookdata;

  globalHook->pipeThread = Threading::CreateThread(&GlobalHookThread);

  return RDResult();
}

bool Process::IsGlobalHookActive()
{
  return globalHook != NULL;
}
void Process::StopGlobalHook()
{
  if(!globalHook)
    return;

  // set the finished flag and join to the thread so it closes the pipes (and so the child
  // processes)
  Atomic::Inc32(&globalHook->finished);

  Threading::JoinThread(globalHook->pipeThread);
  Threading::CloseThread(globalHook->pipeThread);

  // restore the registry settings from before we started
  RestoreRegistry(*globalHook);

  delete globalHook;
  globalHook = NULL;
}

bool Process::IsModuleLoaded(const rdcstr &module)
{
  return GetModuleHandleA(module.c_str()) != NULL;
}

void *Process::LoadModule(const rdcstr &module)
{
  HMODULE mod = GetModuleHandleA(module.c_str());
  if(mod != NULL)
    return mod;

  return LoadLibraryA(module.c_str());
}

void *Process::GetFunctionAddress(void *module, const rdcstr &function)
{
  if(module == NULL)
    return NULL;

  return (void *)GetProcAddress((HMODULE)module, function.c_str());
}

uint32_t Process::GetCurrentPID()
{
  return (uint32_t)GetCurrentProcessId();
}

void Process::Shutdown()
{
  // nothing to do
}
