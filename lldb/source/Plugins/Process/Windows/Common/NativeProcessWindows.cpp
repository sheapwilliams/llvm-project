//===-- NativeProcessWindows.cpp --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Host/windows/windows.h"
#include <psapi.h>

#include "NativeProcessWindows.h"
#include "NativeThreadWindows.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/HostNativeProcessBase.h"
#include "lldb/Host/HostProcess.h"
#include "lldb/Host/ProcessLaunchInfo.h"
#include "lldb/Host/windows/AutoHandle.h"
#include "lldb/Host/windows/HostThreadWindows.h"
#include "lldb/Host/windows/ProcessLauncherWindows.h"
#include "lldb/Target/MemoryRegionInfo.h"
#include "lldb/Target/Process.h"
#include "lldb/Utility/State.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/raw_ostream.h"

#include "DebuggerThread.h"
#include "ExceptionRecord.h"
#include "ProcessWindowsLog.h"

#include <tlhelp32.h>

#pragma warning(disable : 4005)
#include "winternl.h"
#include <ntstatus.h>

using namespace lldb;
using namespace lldb_private;
using namespace llvm;

namespace lldb_private {

NativeProcessWindows::NativeProcessWindows(ProcessLaunchInfo &launch_info,
                                           NativeDelegate &delegate,
                                           llvm::Error &E)
    : NativeProcessProtocol(LLDB_INVALID_PROCESS_ID,
                            launch_info.GetPTY().ReleaseMasterFileDescriptor(),
                            delegate),
      ProcessDebugger(), m_arch(launch_info.GetArchitecture()) {
  ErrorAsOutParameter EOut(&E);
  DebugDelegateSP delegate_sp(new NativeDebugDelegate(*this));
  E = LaunchProcess(launch_info, delegate_sp).ToError();
  if (E)
    return;

  SetID(GetDebuggedProcessId());
}

NativeProcessWindows::NativeProcessWindows(lldb::pid_t pid, int terminal_fd,
                                           NativeDelegate &delegate,
                                           llvm::Error &E)
    : NativeProcessProtocol(pid, terminal_fd, delegate), ProcessDebugger() {
  ErrorAsOutParameter EOut(&E);
  DebugDelegateSP delegate_sp(new NativeDebugDelegate(*this));
  ProcessAttachInfo attach_info;
  attach_info.SetProcessID(pid);
  E = AttachProcess(pid, attach_info, delegate_sp).ToError();
  if (E)
    return;

  SetID(GetDebuggedProcessId());

  ProcessInstanceInfo info;
  if (!Host::GetProcessInfo(pid, info)) {
    E = createStringError(inconvertibleErrorCode(),
                          "Cannot get process information");
    return;
  }
  m_arch = info.GetArchitecture();
}

Status NativeProcessWindows::Resume(const ResumeActionList &resume_actions) {
  Log *log = ProcessWindowsLog::GetLogIfAny(WINDOWS_LOG_PROCESS);
  Status error;
  llvm::sys::ScopedLock lock(m_mutex);

  StateType state = GetState();
  if (state == eStateStopped || state == eStateCrashed) {
    LLDB_LOG(log, "process {0} is in state {1}.  Resuming...",
             GetDebuggedProcessId(), state);
    LLDB_LOG(log, "resuming {0} threads.", m_threads.size());

    bool failed = false;
    for (uint32_t i = 0; i < m_threads.size(); ++i) {
      auto thread = static_cast<NativeThreadWindows *>(m_threads[i].get());
      const ResumeAction *const action =
          resume_actions.GetActionForThread(thread->GetID(), true);
      if (action == nullptr)
        continue;

      switch (action->state) {
      case eStateRunning:
      case eStateStepping: {
        Status result = thread->DoResume(action->state);
        if (result.Fail()) {
          failed = true;
          LLDB_LOG(log,
                   "Trying to resume thread at index {0}, but failed with "
                   "error {1}.",
                   i, result);
        }
        break;
      }
      case eStateSuspended:
      case eStateStopped:
        llvm_unreachable("Unexpected state");

      default:
        return Status(
            "NativeProcessWindows::%s (): unexpected state %s specified "
            "for pid %" PRIu64 ", tid %" PRIu64,
            __FUNCTION__, StateAsCString(action->state), GetID(),
            thread->GetID());
      }
    }

    if (failed) {
      error.SetErrorString("NativeProcessWindows::DoResume failed");
    } else {
      SetState(eStateRunning);
    }

    // Resume the debug loop.
    ExceptionRecordSP active_exception =
        m_session_data->m_debugger->GetActiveException().lock();
    if (active_exception) {
      // Resume the process and continue processing debug events.  Mask the
      // exception so that from the process's view, there is no indication that
      // anything happened.
      m_session_data->m_debugger->ContinueAsyncException(
          ExceptionResult::MaskException);
    }
  } else {
    LLDB_LOG(log, "error: process {0} is in state {1}.  Returning...",
             GetDebuggedProcessId(), GetState());
  }

  return error;
}

NativeThreadWindows *
NativeProcessWindows::GetThreadByID(lldb::tid_t thread_id) {
  return static_cast<NativeThreadWindows *>(
      NativeProcessProtocol::GetThreadByID(thread_id));
}

Status NativeProcessWindows::Halt() {
  bool caused_stop = false;
  StateType state = GetState();
  if (state != eStateStopped)
    return HaltProcess(caused_stop);
  return Status();
}

Status NativeProcessWindows::Detach() {
  Status error;
  Log *log = ProcessWindowsLog::GetLogIfAny(WINDOWS_LOG_PROCESS);
  StateType state = GetState();
  if (state != eStateExited && state != eStateDetached) {
    error = DetachProcess();
    if (error.Success())
      SetState(eStateDetached);
    else
      LLDB_LOG(log, "Detaching process error: {0}", error);
  } else {
    error.SetErrorStringWithFormat("error: process {0} in state = {1}, but "
                                   "cannot detach it in this state.",
                                   GetID(), state);
    LLDB_LOG(log, "error: {0}", error);
  }
  return error;
}

Status NativeProcessWindows::Signal(int signo) {
  Status error;
  error.SetErrorString("Windows does not support sending signals to processes");
  return error;
}

Status NativeProcessWindows::Interrupt() { return Halt(); }

Status NativeProcessWindows::Kill() {
  StateType state = GetState();
  return DestroyProcess(state);
}

Status NativeProcessWindows::IgnoreSignals(llvm::ArrayRef<int> signals) {
  return Status();
}

Status NativeProcessWindows::GetMemoryRegionInfo(lldb::addr_t load_addr,
                                                 MemoryRegionInfo &range_info) {
  return ProcessDebugger::GetMemoryRegionInfo(load_addr, range_info);
}

Status NativeProcessWindows::ReadMemory(lldb::addr_t addr, void *buf,
                                        size_t size, size_t &bytes_read) {
  return ProcessDebugger::ReadMemory(addr, buf, size, bytes_read);
}

Status NativeProcessWindows::WriteMemory(lldb::addr_t addr, const void *buf,
                                         size_t size, size_t &bytes_written) {
  return ProcessDebugger::WriteMemory(addr, buf, size, bytes_written);
}

Status NativeProcessWindows::AllocateMemory(size_t size, uint32_t permissions,
                                            lldb::addr_t &addr) {
  return ProcessDebugger::AllocateMemory(size, permissions, addr);
}

Status NativeProcessWindows::DeallocateMemory(lldb::addr_t addr) {
  return ProcessDebugger::DeallocateMemory(addr);
}

lldb::addr_t NativeProcessWindows::GetSharedLibraryInfoAddress() { return 0; }

bool NativeProcessWindows::IsAlive() const {
  StateType state = GetState();
  switch (state) {
  case eStateCrashed:
  case eStateDetached:
  case eStateExited:
  case eStateInvalid:
  case eStateUnloaded:
    return false;
  default:
    return true;
  }
}

void NativeProcessWindows::SetStopReasonForThread(NativeThreadWindows &thread,
                                                  lldb::StopReason reason,
                                                  std::string description) {
  SetCurrentThreadID(thread.GetID());

  ThreadStopInfo stop_info;
  stop_info.reason = reason;

  // No signal support on Windows but required to provide a 'valid' signum.
  if (reason == StopReason::eStopReasonException) {
    stop_info.details.exception.type = 0;
    stop_info.details.exception.data_count = 0;
  } else {
    stop_info.details.signal.signo = SIGTRAP;
  }

  thread.SetStopReason(stop_info, description);
}

void NativeProcessWindows::StopThread(lldb::tid_t thread_id,
                                      lldb::StopReason reason,
                                      std::string description) {
  NativeThreadWindows *thread = GetThreadByID(thread_id);
  if (!thread)
    return;

  for (uint32_t i = 0; i < m_threads.size(); ++i) {
    auto t = static_cast<NativeThreadWindows *>(m_threads[i].get());
    Status error = t->DoStop();
    if (error.Fail())
      exit(1);
  }
  SetStopReasonForThread(*thread, reason, description);
}

size_t NativeProcessWindows::UpdateThreads() { return m_threads.size(); }

llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
NativeProcessWindows::GetAuxvData() const {
  // Not available on this target.
  return llvm::errc::not_supported;
}

bool NativeProcessWindows::FindSoftwareBreakpoint(lldb::addr_t addr) {
  auto it = m_software_breakpoints.find(addr);
  if (it == m_software_breakpoints.end())
    return false;
  return true;
}

Status NativeProcessWindows::SetBreakpoint(lldb::addr_t addr, uint32_t size,
                                           bool hardware) {
  if (hardware)
    return SetHardwareBreakpoint(addr, size);
  return SetSoftwareBreakpoint(addr, size);
}

Status NativeProcessWindows::RemoveBreakpoint(lldb::addr_t addr,
                                              bool hardware) {
  if (hardware)
    return RemoveHardwareBreakpoint(addr);
  return RemoveSoftwareBreakpoint(addr);
}

Status NativeProcessWindows::CacheLoadedModules() {
  Status error;
  if (!m_loaded_modules.empty())
    return Status();

  // Retrieve loaded modules by a Target/Module free implemenation.
  AutoHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetID()));
  if (snapshot.IsValid()) {
    MODULEENTRY32W me;
    me.dwSize = sizeof(MODULEENTRY32W);
    if (Module32FirstW(snapshot.get(), &me)) {
      do {
        std::string path;
        if (!llvm::convertWideToUTF8(me.szExePath, path))
          continue;

        FileSpec file_spec(path);
        FileSystem::Instance().Resolve(file_spec);
        m_loaded_modules[file_spec] = (addr_t)me.modBaseAddr;
      } while (Module32Next(snapshot.get(), &me));
    }

    if (!m_loaded_modules.empty())
      return Status();
  }

  error.SetError(::GetLastError(), lldb::ErrorType::eErrorTypeWin32);
  return error;
}

Status NativeProcessWindows::GetLoadedModuleFileSpec(const char *module_path,
                                                     FileSpec &file_spec) {
  Status error = CacheLoadedModules();
  if (error.Fail())
    return error;

  FileSpec module_file_spec(module_path);
  FileSystem::Instance().Resolve(module_file_spec);
  for (auto &it : m_loaded_modules) {
    if (it.first == module_file_spec) {
      file_spec = it.first;
      return Status();
    }
  }
  return Status("Module (%s) not found in process %" PRIu64 "!",
                module_file_spec.GetCString(), GetID());
}

Status
NativeProcessWindows::GetFileLoadAddress(const llvm::StringRef &file_name,
                                         lldb::addr_t &load_addr) {
  Status error = CacheLoadedModules();
  if (error.Fail())
    return error;

  load_addr = LLDB_INVALID_ADDRESS;
  FileSpec file_spec(file_name);
  FileSystem::Instance().Resolve(file_spec);
  for (auto &it : m_loaded_modules) {
    if (it.first == file_spec) {
      load_addr = it.second;
      return Status();
    }
  }
  return Status("Can't get loaded address of file (%s) in process %" PRIu64 "!",
                file_spec.GetCString(), GetID());
}

void NativeProcessWindows::OnExitProcess(uint32_t exit_code) {
  Log *log = ProcessWindowsLog::GetLogIfAny(WINDOWS_LOG_PROCESS);
  LLDB_LOG(log, "Process {0} exited with code {1}", GetID(), exit_code);

  ProcessDebugger::OnExitProcess(exit_code);

  // No signal involved.  It is just an exit event.
  WaitStatus wait_status(WaitStatus::Exit, exit_code);
  SetExitStatus(wait_status, true);

  // Notify the native delegate.
  SetState(eStateExited, true);
}

void NativeProcessWindows::OnDebuggerConnected(lldb::addr_t image_base) {
  Log *log = ProcessWindowsLog::GetLogIfAny(WINDOWS_LOG_PROCESS);
  LLDB_LOG(log, "Debugger connected to process {0}. Image base = {1:x}",
           GetDebuggedProcessId(), image_base);

  // This is the earliest chance we can resolve the process ID and
  // architecutre if we don't know them yet.
  if (GetID() == LLDB_INVALID_PROCESS_ID)
    SetID(GetDebuggedProcessId());

  if (GetArchitecture().GetMachine() == llvm::Triple::UnknownArch) {
    ProcessInstanceInfo process_info;
    if (!Host::GetProcessInfo(GetDebuggedProcessId(), process_info)) {
      LLDB_LOG(log, "Cannot get process information during debugger connecting "
                    "to process");
      return;
    }
    SetArchitecture(process_info.GetArchitecture());
  }

  // The very first one shall always be the main thread.
  assert(m_threads.empty());
  m_threads.push_back(std::make_unique<NativeThreadWindows>(
      *this, m_session_data->m_debugger->GetMainThread()));
}

ExceptionResult
NativeProcessWindows::OnDebugException(bool first_chance,
                                       const ExceptionRecord &record) {
  Log *log = ProcessWindowsLog::GetLogIfAny(WINDOWS_LOG_EXCEPTION);
  llvm::sys::ScopedLock lock(m_mutex);

  // Let the debugger establish the internal status.
  ProcessDebugger::OnDebugException(first_chance, record);

  static bool initial_stop = false;
  if (!first_chance) {
    SetState(eStateStopped, false);
  }

  ExceptionResult result = ExceptionResult::SendToApplication;
  switch (record.GetExceptionCode()) {
  case STATUS_SINGLE_STEP:
  case STATUS_WX86_SINGLE_STEP:
    StopThread(record.GetThreadID(), StopReason::eStopReasonTrace);
    SetState(eStateStopped, true);

    // Continue the debugger.
    return ExceptionResult::MaskException;

  case STATUS_BREAKPOINT:
  case STATUS_WX86_BREAKPOINT:
    if (FindSoftwareBreakpoint(record.GetExceptionAddress())) {
      LLDB_LOG(log, "Hit non-loader breakpoint at address {0:x}.",
               record.GetExceptionAddress());

      StopThread(record.GetThreadID(), StopReason::eStopReasonBreakpoint);

      if (NativeThreadWindows *stop_thread =
              GetThreadByID(record.GetThreadID())) {
        auto &register_context = stop_thread->GetRegisterContext();
        // The current EIP is AFTER the BP opcode, which is one byte '0xCC'
        uint64_t pc = register_context.GetPC() - 1;
        register_context.SetPC(pc);
      }

      SetState(eStateStopped, true);
      return ExceptionResult::MaskException;
    }

    if (!initial_stop) {
      initial_stop = true;
      LLDB_LOG(log,
               "Hit loader breakpoint at address {0:x}, setting initial stop "
               "event.",
               record.GetExceptionAddress());

      // We are required to report the reason for the first stop after
      // launching or being attached.
      if (NativeThreadWindows *thread = GetThreadByID(record.GetThreadID()))
        SetStopReasonForThread(*thread, StopReason::eStopReasonBreakpoint);

      // Do not notify the native delegate (e.g. llgs) since at this moment
      // the program hasn't returned from Factory::Launch() and the delegate
      // might not have an valid native process to operate on.
      SetState(eStateStopped, false);

      // Hit the initial stop. Continue the application.
      return ExceptionResult::BreakInDebugger;
    }

    // Fall through
  default:
    LLDB_LOG(log,
             "Debugger thread reported exception {0:x} at address {1:x} "
             "(first_chance={2})",
             record.GetExceptionCode(), record.GetExceptionAddress(),
             first_chance);

    {
      std::string desc;
      llvm::raw_string_ostream desc_stream(desc);
      desc_stream << "Exception "
                  << llvm::format_hex(record.GetExceptionCode(), 8)
                  << " encountered at address "
                  << llvm::format_hex(record.GetExceptionAddress(), 8);
      StopThread(record.GetThreadID(), StopReason::eStopReasonException,
                 desc_stream.str().c_str());

      SetState(eStateStopped, true);
    }

    // For non-breakpoints, give the application a chance to handle the
    // exception first.
    if (first_chance)
      result = ExceptionResult::SendToApplication;
    else
      result = ExceptionResult::BreakInDebugger;
  }

  return result;
}

void NativeProcessWindows::OnCreateThread(const HostThread &new_thread) {
  llvm::sys::ScopedLock lock(m_mutex);
  m_threads.push_back(
      std::make_unique<NativeThreadWindows>(*this, new_thread));
}

void NativeProcessWindows::OnExitThread(lldb::tid_t thread_id,
                                        uint32_t exit_code) {
  llvm::sys::ScopedLock lock(m_mutex);
  NativeThreadWindows *thread = GetThreadByID(thread_id);
  if (!thread)
    return;

  for (auto t = m_threads.begin(); t != m_threads.end();) {
    if ((*t)->GetID() == thread_id) {
      t = m_threads.erase(t);
    } else {
      ++t;
    }
  }
}

void NativeProcessWindows::OnLoadDll(const ModuleSpec &module_spec,
                                     lldb::addr_t module_addr) {
  // Simply invalidate the cached loaded modules.
  if (!m_loaded_modules.empty())
    m_loaded_modules.clear();
}

void NativeProcessWindows::OnUnloadDll(lldb::addr_t module_addr) {
  if (!m_loaded_modules.empty())
    m_loaded_modules.clear();
}

llvm::Expected<std::unique_ptr<NativeProcessProtocol>>
NativeProcessWindows::Factory::Launch(
    ProcessLaunchInfo &launch_info,
    NativeProcessProtocol::NativeDelegate &native_delegate,
    MainLoop &mainloop) const {
  Error E = Error::success();
  auto process_up = std::unique_ptr<NativeProcessWindows>(
      new NativeProcessWindows(launch_info, native_delegate, E));
  if (E)
    return std::move(E);
  return std::move(process_up);
}

llvm::Expected<std::unique_ptr<NativeProcessProtocol>>
NativeProcessWindows::Factory::Attach(
    lldb::pid_t pid, NativeProcessProtocol::NativeDelegate &native_delegate,
    MainLoop &mainloop) const {
  Error E = Error::success();
  // Set pty master fd invalid since it is not available.
  auto process_up = std::unique_ptr<NativeProcessWindows>(
      new NativeProcessWindows(pid, -1, native_delegate, E));
  if (E)
    return std::move(E);
  return std::move(process_up);
}
} // namespace lldb_private
