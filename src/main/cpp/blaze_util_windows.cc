// Copyright 2014 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <fcntl.h>
#include <stdarg.h>  // va_start, va_end, va_list

#ifndef COMPILER_MSVC
#include <errno.h>
#include <limits.h>
#include <sys/cygwin.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#endif  // COMPILER_MSVC

#include <windows.h>
#include <lmcons.h>          // UNLEN
#include <versionhelpers.h>  // IsWindows8OrGreater

#ifdef COMPILER_MSVC
#include <io.h>            // _open
#include <knownfolders.h>  // FOLDERID_Profile
#include <objbase.h>       // CoTaskMemFree
#include <shlobj.h>        // SHGetKnownFolderPath
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <thread>  // NOLINT (to slience Google-internal linter)
#include <type_traits>  // static_assert
#include <vector>

#include "src/main/cpp/blaze_util.h"
#include "src/main/cpp/blaze_util_platform.h"
#include "src/main/cpp/global_variables.h"
#include "src/main/cpp/startup_options.h"
#include "src/main/cpp/util/errors.h"
#include "src/main/cpp/util/exit_code.h"
#include "src/main/cpp/util/file.h"
#include "src/main/cpp/util/file_platform.h"
#include "src/main/cpp/util/md5.h"
#include "src/main/cpp/util/numbers.h"
#include "src/main/cpp/util/strings.h"
#include "src/main/native/windows/file.h"
#include "src/main/native/windows/util.h"

namespace blaze {

// Ensure we can safely cast (const) wchar_t* to LP(C)WSTR.
// This is true with MSVC but usually not with GCC.
static_assert(sizeof(wchar_t) == sizeof(WCHAR),
              "wchar_t and WCHAR should be the same size");

// When using widechar Win32 API functions the maximum path length is 32K.
// Add 4 characters for potential UNC prefix and a couple more for safety.
static const size_t kWindowsPathBufferSize = 0x8010;

using bazel::windows::AutoHandle;
using bazel::windows::CreateJunction;

// TODO(bazel-team): get rid of die/pdie, handle errors on the caller side.
// die/pdie are exit points in the code and they make it difficult to follow the
// control flow, plus it's not clear whether they call destructors on local
// variables in the call stack.
using blaze_util::die;
using blaze_util::pdie;

using std::string;
using std::unique_ptr;
using std::wstring;

SignalHandler SignalHandler::INSTANCE;

class WindowsClock {
 public:
  uint64_t GetMilliseconds() const;
  uint64_t GetProcessMilliseconds() const;

  static const WindowsClock INSTANCE;

 private:
  // Clock frequency per seconds.
  // It's safe to cache this because (from QueryPerformanceFrequency on MSDN):
  // "The frequency of the performance counter is fixed at system boot and is
  // consistent across all processors. Therefore, the frequency need only be
  // queried upon application initialization, and the result can be cached."
  const LARGE_INTEGER kFrequency;

  // Time (in milliseconds) at process start.
  const LARGE_INTEGER kStart;

  WindowsClock();

  static LARGE_INTEGER GetFrequency();
  static LARGE_INTEGER GetMillisecondsAsLargeInt(const LARGE_INTEGER& freq);
};

#ifdef COMPILER_MSVC

BOOL WINAPI ConsoleCtrlHandler(_In_ DWORD ctrlType) {
  static volatile int sigint_count = 0;
  switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
      if (++sigint_count >= 3) {
        SigPrintf(
            "\n%s caught third Ctrl+C handler signal; killed.\n\n",
            SignalHandler::Get().GetGlobals()->options->product_name.c_str());
        if (SignalHandler::Get().GetGlobals()->server_pid != -1) {
          KillServerProcess(
              SignalHandler::Get().GetGlobals()->server_pid,
              SignalHandler::Get().GetGlobals()->options->output_base);
        }
        _exit(1);
      }
      SigPrintf(
          "\n%s Ctrl+C handler; shutting down.\n\n",
          SignalHandler::Get().GetGlobals()->options->product_name.c_str());
      SignalHandler::Get().CancelServer();
      return TRUE;

    case CTRL_CLOSE_EVENT:
      SignalHandler::Get().CancelServer();
      return TRUE;
  }
  return false;
}

void SignalHandler::Install(GlobalVariables* globals,
                            SignalHandler::Callback cancel_server) {
  _globals = globals;
  _cancel_server = cancel_server;
  ::SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);
}

ATTRIBUTE_NORETURN void SignalHandler::PropagateSignalOrExit(int exit_code) {
  // We do not handle signals on Windows; always exit with exit_code.
  exit(exit_code);
}

#else  // not COMPILER_MSVC

// The number of the last received signal that should cause the client
// to shutdown.  This is saved so that the client's WTERMSIG can be set
// correctly.  (Currently only SIGPIPE uses this mechanism.)
static volatile sig_atomic_t signal_handler_received_signal = 0;

// Signal handler.
static void handler(int signum) {
  int saved_errno = errno;

  static volatile sig_atomic_t sigint_count = 0;

  switch (signum) {
    case SIGINT:
      if (++sigint_count >= 3) {
        SigPrintf(
            "\n%s caught third interrupt signal; killed.\n\n",
            SignalHandler::Get().GetGlobals()->options->product_name.c_str());
        if (SignalHandler::Get().GetGlobals()->server_pid != -1) {
          KillServerProcess(
              SignalHandler::Get().GetGlobals()->server_pid,
              SignalHandler::Get().GetGlobals()->options->output_base);
        }
        _exit(1);
      }
      SigPrintf(
          "\n%s caught interrupt signal; shutting down.\n\n",
          SignalHandler::Get().GetGlobals()->options->product_name.c_str());
      SignalHandler::Get().CancelServer();
      break;
    case SIGTERM:
      SigPrintf(
          "\n%s caught terminate signal; shutting down.\n\n",
          SignalHandler::Get().GetGlobals()->options->product_name.c_str());
      SignalHandler::Get().CancelServer();
      break;
    case SIGPIPE:
      signal_handler_received_signal = SIGPIPE;
      break;
    case SIGQUIT:
      SigPrintf("\nSending SIGQUIT to JVM process %d (see %s).\n\n",
                SignalHandler::Get().GetGlobals()->server_pid,
                SignalHandler::Get().GetGlobals()->jvm_log_file.c_str());
      kill(SignalHandler::Get().GetGlobals()->server_pid, SIGQUIT);
      break;
  }

  errno = saved_errno;
}

void SignalHandler::Install(GlobalVariables* globals,
                            SignalHandler::Callback cancel_server) {
  _globals = globals;
  _cancel_server = cancel_server;

  // Unblock all signals.
  sigset_t sigset;
  sigemptyset(&sigset);
  sigprocmask(SIG_SETMASK, &sigset, NULL);

  signal(SIGINT, handler);
  signal(SIGTERM, handler);
  signal(SIGPIPE, handler);
  signal(SIGQUIT, handler);
}

ATTRIBUTE_NORETURN void SignalHandler::PropagateSignalOrExit(int exit_code) {
  if (signal_handler_received_signal) {
    // Kill ourselves with the same signal, so that callers see the
    // right WTERMSIG value.
    signal(signal_handler_received_signal, SIG_DFL);
    raise(signal_handler_received_signal);
    exit(1);  // (in case raise didn't kill us for some reason)
  } else {
    exit(exit_code);
  }
}

#endif  // COMPILER_MSVC

// A signal-safe version of fprintf(stderr, ...).
//
// WARNING: any output from the blaze client may be interleaved
// with output from the blaze server.  In --curses mode,
// the Blaze server often erases the previous line of output.
// So, be sure to end each such message with TWO newlines,
// otherwise it may be erased by the next message from the
// Blaze server.
// Also, it's a good idea to start each message with a newline,
// in case the Blaze server has written a partial line.
void SigPrintf(const char *format, ...) {
#ifdef COMPILER_MSVC
  int stderr_fileno = _fileno(stderr);
#else  // not COMPILER_MSVC
  int stderr_fileno = STDERR_FILENO;
#endif
  char buf[1024];
  va_list ap;
  va_start(ap, format);
  int r = vsnprintf(buf, sizeof buf, format, ap);
  va_end(ap);
  if (write(stderr_fileno, buf, r) <= 0) {
    // We don't care, just placate the compiler.
  }
}

static void PrintErrorW(const wstring& op) {
  DWORD last_error = ::GetLastError();
  if (last_error == 0) {
    return;
  }

  WCHAR* message_buffer;
  FormatMessageW(
      /* dwFlags */ FORMAT_MESSAGE_ALLOCATE_BUFFER |
          FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      /* lpSource */ nullptr,
      /* dwMessageId */ last_error,
      /* dwLanguageId */ MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      /* lpBuffer */ message_buffer,
      /* nSize */ 0,
      /* Arguments */ nullptr);

  fwprintf(stderr, L"ERROR: %s: %s (%d)\n", op.c_str(), message_buffer,
           last_error);
  LocalFree(message_buffer);
}

void WarnFilesystemType(const string& output_base) {
}

string GetProcessIdAsString() {
  return ToString(GetCurrentProcessId());
}

string GetSelfPath() {
  WCHAR buffer[kWindowsPathBufferSize] = {0};
  if (!GetModuleFileNameW(0, buffer, kWindowsPathBufferSize)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "GetSelfPath: GetModuleFileNameW");
  }
  return string(blaze_util::WstringToCstring(buffer).get());
}

string GetOutputRoot() {
  for (const char* i : {"TMPDIR", "TEMPDIR", "TMP", "TEMP"}) {
    string tmpdir(GetEnv(i));
    if (!tmpdir.empty()) {
      return tmpdir;
    }
  }
#ifdef COMPILER_MSVC
  // GetTempPathW and GetEnvironmentVariableW only work properly when Bazel
  // runs under cmd.exe, not when it's run from msys.
  // The reason is that MSYS consumes all environment variables and sets its own
  // ones. The symptom of this is that GetEnvironmentVariableW returns nothing
  // for TEMP under MSYS, though it can retrieve WINDIR.

  WCHAR buffer[kWindowsPathBufferSize] = {0};
  if (!::GetTempPathW(kWindowsPathBufferSize, buffer)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "GetOutputRoot: GetTempPathW");
  }
  return string(blaze_util::WstringToCstring(buffer).get());
#else  // not COMPILER_MSVC
  return "/var/tmp";
#endif  // COMPILER_MSVC
}

string GetHomeDir() {
#ifdef COMPILER_MSVC
  PWSTR wpath;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, NULL,
                                       &wpath))) {
    string result = string(blaze_util::WstringToCstring(wpath).get());
    ::CoTaskMemFree(wpath);
    return result;
  }
#endif
  return GetEnv("HOME");  // only defined in MSYS/Cygwin
}

string FindSystemWideBlazerc() {
#ifdef COMPILER_MSVC
  // TODO(bazel-team): figure out a good path to return here.
  return "";
#else   // not COMPILER_MSVC
  string path = "/etc/bazel.bazelrc";
  if (blaze_util::CanReadFile(path)) {
    return path;
  }
  return "";
#endif  // COMPILER_MSVC
}

string GetJavaBinaryUnderJavabase() { return "bin/java.exe"; }

uint64_t GetMillisecondsMonotonic() {
  return WindowsClock::INSTANCE.GetMilliseconds();
}

uint64_t GetMillisecondsSinceProcessStart() {
  return WindowsClock::INSTANCE.GetProcessMilliseconds();
}

void SetScheduling(bool batch_cpu_scheduling, int io_nice_level) {
  // TODO(bazel-team): There should be a similar function on Windows.
}

string GetProcessCWD(int pid) {
#ifdef COMPILER_MSVC
  // TODO(bazel-team) 2016-11-18: decide whether we need this on Windows and
  // implement or delete.
  return "";
#else   // not COMPILER_MSVC
  char server_cwd[PATH_MAX] = {};
  if (readlink(
          ("/proc/" + ToString(pid) + "/cwd").c_str(),
          server_cwd, sizeof(server_cwd)) < 0) {
    return "";
  }

  return string(server_cwd);
#endif  // COMPILER_MSVC
}

bool IsSharedLibrary(const string &filename) {
  return blaze_util::ends_with(filename, ".dll");
}

string GetDefaultHostJavabase() {
  string javahome(GetEnv("JAVA_HOME"));
  if (javahome.empty()) {
    die(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
        "Error: JAVA_HOME not set.");
  }
  return javahome;
}

namespace {

// Max command line length is per CreateProcess documentation
// (https://msdn.microsoft.com/en-us/library/ms682425(VS.85).aspx)
//
// Quoting rules are described here:
// https://blogs.msdn.microsoft.com/twistylittlepassagesallalike/2011/04/23/everyone-quotes-command-line-arguments-the-wrong-way/

static const int MAX_CMDLINE_LENGTH = 32768;

struct CmdLine {
  char cmdline[MAX_CMDLINE_LENGTH];
};
static void CreateCommandLine(CmdLine* result, const string& exe,
                              const std::vector<string>& args_vector) {
  std::ostringstream cmdline;
  string short_exe;
  if (!blaze_util::AsShortWindowsPath(exe, &short_exe)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "CreateCommandLine: AsShortWindowsPath(%s)", exe.c_str());
  }
  bool first = true;
  for (const auto& s : args_vector) {
    if (first) {
      first = false;
      // Skip first argument, instead use quoted executable name.
      cmdline << '\"' << short_exe << '\"';
      continue;
    } else {
      cmdline << ' ';
    }

    bool has_space = s.find(" ") != string::npos;

    if (has_space) {
      cmdline << '\"';
    }

    std::string::const_iterator it = s.begin();
    while (it != s.end()) {
      char ch = *it++;
      switch (ch) {
        case '"':
          // Escape double quotes
          cmdline << "\\\"";
          break;

        case '\\':
          if (it == s.end()) {
            // Backslashes at the end of the string are quoted if we add quotes
            cmdline << (has_space ? "\\\\" : "\\");
          } else {
            // Backslashes everywhere else are quoted if they are followed by a
            // quote or a backslash
            cmdline << (*it == '"' || *it == '\\' ? "\\\\" : "\\");
          }
          break;

        default:
          cmdline << ch;
      }
    }

    if (has_space) {
      cmdline << '\"';
    }
  }

  string cmdline_str = cmdline.str();
  if (cmdline_str.size() >= MAX_CMDLINE_LENGTH) {
    pdie(blaze_exit_code::INTERNAL_ERROR, "Command line too long (%d > %d): %s",
         cmdline_str.size(), MAX_CMDLINE_LENGTH, cmdline_str.c_str());
  }

  // Copy command line into a mutable buffer.
  // CreateProcess is allowed to mutate its command line argument.
  strncpy(result->cmdline, cmdline_str.c_str(), MAX_CMDLINE_LENGTH - 1);
  result->cmdline[MAX_CMDLINE_LENGTH - 1] = 0;
}

}  // namespace

string GetJvmVersion(const string& java_exe) {
  HANDLE pipe_read, pipe_write;

  SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
  if (!::CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "GetJvmVersion: CreatePipe");
  }

  if (!SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(pipe_read);
    CloseHandle(pipe_write);
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "GetJvmVersion: SetHandleInformation");
  }

  PROCESS_INFORMATION processInfo = {0};
  STARTUPINFOA startupInfo = {0};
  startupInfo.hStdError = pipe_write;
  startupInfo.hStdOutput = pipe_write;
  startupInfo.dwFlags |= STARTF_USESTDHANDLES;

  string win_java_exe;
  if (!blaze_util::AsShortWindowsPath(java_exe, &win_java_exe)) {
    CloseHandle(pipe_read);
    CloseHandle(pipe_write);
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "GetJvmVersion: AsShortWindowsPath(%s)", java_exe.c_str());
  }
  win_java_exe = string("\"") + win_java_exe + "\" -version";

  char cmdline[MAX_CMDLINE_LENGTH];
  strncpy(cmdline, win_java_exe.c_str(), win_java_exe.size() + 1);
  BOOL ok = CreateProcessA(
      /* lpApplicationName */ NULL,
      /* lpCommandLine */ cmdline,
      /* lpProcessAttributes */ NULL,
      /* lpThreadAttributes */ NULL,
      /* bInheritHandles */ TRUE,
      /* dwCreationFlags */ 0,
      /* lpEnvironment */ NULL,
      /* lpCurrentDirectory */ NULL,
      /* lpStartupInfo */ &startupInfo,
      /* lpProcessInformation */ &processInfo);

  if (!ok) {
    CloseHandle(pipe_read);
    CloseHandle(pipe_write);
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "RunProgram: CreateProcess(%s)", cmdline);
  }

  CloseHandle(pipe_write);
  std::string result = "";
  DWORD bytes_read;
  CHAR buf[1024];

  for (;;) {
    ok = ::ReadFile(pipe_read, buf, 1023, &bytes_read, NULL);
    if (!ok || bytes_read == 0) {
      break;
    }
    buf[bytes_read] = 0;
    result = result + buf;
  }

  CloseHandle(pipe_read);
  CloseHandle(processInfo.hProcess);
  CloseHandle(processInfo.hThread);
  return ReadJvmVersion(result);
}

#ifndef COMPILER_MSVC
// If we pass DETACHED_PROCESS to CreateProcess(), cmd.exe appropriately
// returns the command prompt when the client terminates. msys2, however, in
// its infinite wisdom, waits until the *server* terminates and cannot be
// convinced otherwise.
//
// So, we first pretend to be a POSIX daemon so that msys2 knows about our
// intentions and *then* we call CreateProcess(). Life ain't easy.
static bool DaemonizeOnWindows() {
  if (fork() > 0) {
    // We are the original client process.
    return true;
  }

  if (fork() > 0) {
    // We are the child of the original client process. Terminate so that the
    // actual server is not a child process of the client.
    exit(0);
  }

  setsid();
  // Contrary to the POSIX version, we are not closing the three standard file
  // descriptors here. CreateProcess() will take care of that and it's useful
  // to see the error messages in ExecuteDaemon() on the console of the client.
  return false;
}
#endif  // not COMPILER_MSVC

static bool GetProcessStartupTime(HANDLE process, uint64_t* result) {
  FILETIME creation_time, dummy1, dummy2, dummy3;
  // GetProcessTimes cannot handle NULL arguments.
  if (process == INVALID_HANDLE_VALUE ||
      !::GetProcessTimes(process, &creation_time, &dummy1, &dummy2, &dummy3)) {
    return false;
  }
  *result = static_cast<uint64_t>(creation_time.dwHighDateTime) << 32 |
            creation_time.dwLowDateTime;
  return true;
}

static void WriteProcessStartupTime(const string& server_dir, HANDLE process) {
  uint64_t start_time = 0;
  if (!GetProcessStartupTime(process, &start_time)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "WriteProcessStartupTime(%s): GetProcessStartupTime",
         server_dir.c_str());
  }

  string start_time_file = blaze_util::JoinPath(server_dir, "server.starttime");
  if (!blaze_util::WriteFile(ToString(start_time), start_time_file)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "WriteProcessStartupTime(%s): WriteFile(%s)", server_dir.c_str(),
         start_time_file.c_str());
  }
}

static HANDLE CreateJvmOutputFile(const wstring& path,
                                  SECURITY_ATTRIBUTES* sa) {
  // If the previous server process was asked to be shut down (but not killed),
  // it takes a while for it to comply, so wait until the JVM output file that
  // it held open is closed. There seems to be no better way to wait for a file
  // to be closed on Windows.
  static const unsigned int timeout_sec = 60;
  for (unsigned int waited = 0; waited < timeout_sec; ++waited) {
    HANDLE handle = ::CreateFileW(
        /* lpFileName */ path.c_str(),
        /* dwDesiredAccess */ GENERIC_READ | GENERIC_WRITE,
        /* dwShareMode */ FILE_SHARE_READ,
        /* lpSecurityAttributes */ sa,
        /* dwCreationDisposition */ CREATE_ALWAYS,
        /* dwFlagsAndAttributes */ FILE_ATTRIBUTE_NORMAL,
        /* hTemplateFile */ NULL);
    if (handle != INVALID_HANDLE_VALUE) {
      return handle;
    }
    if (GetLastError() != ERROR_SHARING_VIOLATION &&
        GetLastError() != ERROR_LOCK_VIOLATION) {
      // Some other error occurred than the file being open; bail out.
      break;
    }

    // The file is still held open, the server is shutting down. There's a
    // chance that another process holds it open, we don't know; in that case
    // we just exit after the timeout expires.
    if (waited == 5 || waited == 10 || waited == 30) {
      fprintf(stderr,
              "Waiting for previous Bazel server's log file to close "
              "(waited %d seconds, waiting at most %d)\n",
              waited, timeout_sec);
    }
    Sleep(1000);
  }
  return INVALID_HANDLE_VALUE;
}

#ifdef COMPILER_MSVC

class ProcessHandleBlazeServerStartup : public BlazeServerStartup {
 public:
  ProcessHandleBlazeServerStartup(HANDLE _proc) : proc(_proc) {}

  bool IsStillAlive() override {
    FILETIME dummy1, exit_time, dummy2, dummy3;
    return GetProcessTimes(proc, &dummy1, &exit_time, &dummy2, &dummy3) &&
           exit_time.dwHighDateTime == 0 && exit_time.dwLowDateTime == 0;
  }

 private:
  AutoHandle proc;
};

#else  // COMPILER_MSVC

// Keeping an eye on the server process on Windows is not implemented yet.
// TODO(lberki): Implement this, because otherwise if we can't start up a server
// process, the client will hang until it times out.
class DummyBlazeServerStartup : public BlazeServerStartup {
 public:
  DummyBlazeServerStartup() {}
  virtual ~DummyBlazeServerStartup() {}
  virtual bool IsStillAlive() { return true; }
};

#endif  // COMPILER_MSVC

void ExecuteDaemon(const string& exe, const std::vector<string>& args_vector,
                   const string& daemon_output, const string& server_dir,
                   BlazeServerStartup** server_startup) {
#ifndef COMPILER_MSVC
  if (DaemonizeOnWindows()) {
    // We are the client process
    *server_startup = new DummyBlazeServerStartup();
    return;
  }
#endif  // not COMPILER_MSVC

  wstring wdaemon_output;
  if (!blaze_util::AsAbsoluteWindowsPath(daemon_output, &wdaemon_output)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "ExecuteDaemon(%s): AsAbsoluteWindowsPath(%s)", exe.c_str(),
         daemon_output.c_str());
  }

  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  // We redirect stdin to the NUL device, and redirect stdout and stderr to
  // `stdout_file` and `stderr_file` (opened below) by telling CreateProcess to
  // use these file handles, so they must be inheritable.
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;

  AutoHandle devnull(::CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
  if (!devnull.IsValid()) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "ExecuteDaemon(%s): CreateFileA(NUL)", exe.c_str());
  }

  AutoHandle stdout_file(CreateJvmOutputFile(wdaemon_output.c_str(), &sa));
  if (!stdout_file.IsValid()) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "ExecuteDaemon(%s): CreateJvmOutputFile(%ls)", exe.c_str(),
         wdaemon_output.c_str());
  }
  HANDLE stderr_handle;
  // We must duplicate the handle to stdout, otherwise "bazel clean --expunge"
  // won't work, because when it tries to close stdout then stderr, the former
  // will succeed but the latter will appear to be valid yet still fail to
  // close.
  if (!DuplicateHandle(
          /* hSourceProcessHandle */ GetCurrentProcess(),
          /* hSourceHandle */ stdout_file,
          /* hTargetProcessHandle */ GetCurrentProcess(),
          /* lpTargetHandle */ &stderr_handle,
          /* dwDesiredAccess */ 0,
          /* bInheritHandle */ TRUE,
          /* dwOptions */ DUPLICATE_SAME_ACCESS)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "ExecuteDaemon(%s): DuplicateHandle(%ls)", exe.c_str(),
         wdaemon_output.c_str());
  }
  AutoHandle stderr_file(stderr_handle);

  PROCESS_INFORMATION processInfo = {0};
  STARTUPINFOA startupInfo = {0};

  startupInfo.hStdInput = devnull;
  startupInfo.hStdError = stdout_file;
  startupInfo.hStdOutput = stderr_handle;
  startupInfo.dwFlags |= STARTF_USESTDHANDLES;
  CmdLine cmdline;
  CreateCommandLine(&cmdline, exe, args_vector);

  BOOL ok = CreateProcessA(
      /* lpApplicationName */ NULL,
      /* lpCommandLine */ cmdline.cmdline,
      /* lpProcessAttributes */ NULL,
      /* lpThreadAttributes */ NULL,
      /* bInheritHandles */ TRUE,
      /* dwCreationFlags */ DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
      /* lpEnvironment */ NULL,
      /* lpCurrentDirectory */ NULL,
      /* lpStartupInfo */ &startupInfo,
      /* lpProcessInformation */ &processInfo);

  if (!ok) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "ExecuteDaemon(%s): CreateProcess(%s)", exe.c_str(), cmdline.cmdline);
  }

  WriteProcessStartupTime(server_dir, processInfo.hProcess);

#ifdef COMPILER_MSVC
  // Pass ownership of processInfo.hProcess
  *server_startup = new ProcessHandleBlazeServerStartup(processInfo.hProcess);
#endif

  string pid_string = ToString(processInfo.dwProcessId);
  string pid_file = blaze_util::JoinPath(server_dir, kServerPidFile);
  if (!blaze_util::WriteFile(pid_string, pid_file)) {
    // Not a lot we can do if this fails
    fprintf(stderr, "Cannot write PID file %s\n", pid_file.c_str());
  }

  // Don't close processInfo.hProcess here, it's now owned by the
  // ProcessHandleBlazeServerStartup instance.
  CloseHandle(processInfo.hThread);

#ifndef COMPILER_MSVC
  exit(0);
#endif  // COMPILER_MSVC
}

void BatchWaiterThread(HANDLE java_handle) {
  WaitForSingleObject(java_handle, INFINITE);
}

#ifdef COMPILER_MSVC
  // TODO(bazel-team): implement signal handling.
#else  // not COMPILER_MSVC
static void MingwSignalHandler(int signum) {
  // Java process will be terminated because we set the job to terminate if its
  // handle is closed.
  //
  // Note that this is different how interruption is handled on Unix, where the
  // Java process sets up a signal handler for SIGINT itself. That cannot be
  // done on Windows without using native code, and it's better to have as
  // little JNI as possible. The most important part of the cleanup after
  // termination (killing all child processes) happens automatically on Windows
  // anyway, since we put the batch Java process in its own job which does not
  // allow breakaway processes.
  exit(blaze_exit_code::ExitCode::INTERRUPTED);
}
#endif  // COMPILER_MSVC

// Returns whether assigning the given process to a job failed because nested
// jobs are not available on the current system.
static bool IsFailureDueToNestedJobsNotSupported(HANDLE process) {
  BOOL is_in_job;
  if (!IsProcessInJob(process, NULL, &is_in_job)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "IsFailureDueToNestedJobsNotSupported: IsProcessInJob");
    return false;
  }

  if (!is_in_job) {
    // Not in a job.
    return false;
  }
  return !IsWindows8OrGreater();
}

// Run the given program in the current working directory, using the given
// argument vector, wait for it to finish, then exit ourselves with the exitcode
// of that program.
void ExecuteProgram(const string& exe, const std::vector<string>& args_vector) {
  CmdLine cmdline;
  CreateCommandLine(&cmdline, exe, args_vector);

  STARTUPINFOA startupInfo = {0};
  startupInfo.cb = sizeof(STARTUPINFOA);

  PROCESS_INFORMATION processInfo = {0};

  HANDLE job = CreateJobObject(NULL, NULL);
  if (job == NULL) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "ExecuteProgram(%s): CreateJobObject", exe.c_str());
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {0};
  job_info.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                               &job_info, sizeof(job_info))) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "ExecuteProgram(%s): SetInformationJobObject", exe.c_str());
  }

  BOOL success = CreateProcessA(
      /* lpApplicationName */ NULL,
      /* lpCommandLine */ cmdline.cmdline,
      /* lpProcessAttributes */ NULL,
      /* lpThreadAttributes */ NULL,
      /* bInheritHandles */ TRUE,
      /* dwCreationFlags */ CREATE_SUSPENDED,
      /* lpEnvironment */ NULL,
      /* lpCurrentDirectory */ NULL,
      /* lpStartupInfo */ &startupInfo,
      /* lpProcessInformation */ &processInfo);

  if (!success) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "ExecuteProgram(%s): CreateProcess(%s)", exe.c_str(), cmdline.cmdline);
  }

  // We will try to put the launched process into a Job object. This will make
  // Windows reliably kill all child processes that the process itself may
  // launch once the process exits. On Windows systems that don't support nested
  // jobs, this may fail if we are already running inside a job ourselves. In
  // this case, we'll continue anyway, because we assume that our parent is
  // handling process management for us.
  if (!AssignProcessToJobObject(job, processInfo.hProcess) &&
      !IsFailureDueToNestedJobsNotSupported(processInfo.hProcess)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "ExecuteProgram(%s): AssignProcessToJobObject", exe.c_str());
  }

  // Now that we potentially put the process into a new job object, we can start
  // running it.
  if (ResumeThread(processInfo.hThread) == -1) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "ExecuteProgram(%s): ResumeThread", exe.c_str());
  }

  // msys doesn't deliver signals while a Win32 call is pending so we need to
  // do the blocking call in another thread

#ifdef COMPILER_MSVC
  // TODO(bazel-team): implement signal handling.
#else  // not COMPILER_MSVC
  signal(SIGINT, MingwSignalHandler);
#endif  // COMPILER_MSVC
  std::thread batch_waiter_thread([=]() {
    BatchWaiterThread(processInfo.hProcess);
  });

  // The output base lock is held while waiting
  batch_waiter_thread.join();
  DWORD exit_code;
  GetExitCodeProcess(processInfo.hProcess, &exit_code);
  CloseHandle(processInfo.hProcess);
  CloseHandle(processInfo.hThread);
  exit(exit_code);
}

const char kListSeparator = ';';

string PathAsJvmFlag(const string& path) {
  string spath;
  if (!blaze_util::AsShortWindowsPath(path, &spath)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "PathAsJvmFlag(%s): AsShortWindowsPath", path.c_str());
  }
  // Convert backslashes to forward slashes, in order to avoid the JVM parsing
  // Windows paths as if they contained escaped characters.
  // See https://github.com/bazelbuild/bazel/issues/2576
  std::replace(spath.begin(), spath.end(), '\\', '/');
  return spath;
}

string ConvertPath(const string& path) {
#ifdef COMPILER_MSVC
  // The path may not be Windows-style and may not be normalized, so convert it.
  wstring wpath;
  if (!blaze_util::AsAbsoluteWindowsPath(path, &wpath)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "ConvertPath(%s): AsAbsoluteWindowsPath", path.c_str());
  }
  std::transform(wpath.begin(), wpath.end(), wpath.begin(), ::towlower);
  return string(blaze_util::WstringToCstring(
                    blaze_util::RemoveUncPrefixMaybe(wpath.c_str()))
                    .get());
#else  // not COMPILER_MSVC
  // If the path looks like %USERPROFILE%/foo/bar, don't convert.
  if (path.empty() || path[0] == '%') {
    // It's fine to convert to lower-case even if the path contains environment
    // variable names, since Windows can look them up case-insensitively.
    return blaze_util::AsLower(path);
  }
  char* wpath = static_cast<char*>(cygwin_create_path(
      CCP_POSIX_TO_WIN_A, static_cast<const void*>(path.c_str())));
  string result(wpath);
  free(wpath);
  return blaze_util::AsLower(result);
#endif  // COMPILER_MSVC
}

// Convert a Unix path list to Windows path list
string ConvertPathList(const string& path_list) {
#ifdef COMPILER_MSVC
  // In the MSVC version we use the actual %PATH% value which is separated by
  // ";" and contains Windows paths.
  return path_list;
#else   // not COMPILER_MSVC
  string w_list = "";
  int start = 0;
  int pos;
  while ((pos = path_list.find(":", start)) != string::npos) {
    w_list += ConvertPath(path_list.substr(start, pos - start)) + ";";
    start = pos + 1;
  }
  if (start < path_list.size()) {
    w_list += ConvertPath(path_list.substr(start));
  }
  return w_list;
#endif  // COMPILER_MSVC
}

bool SymlinkDirectories(const string &posix_target, const string &posix_name) {
  wstring name;
  wstring target;
  if (!blaze_util::AsAbsoluteWindowsPath(posix_name, &name)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "SymlinkDirectories(%s, %s): AsAbsoluteWindowsPath(%s)",
         posix_target.c_str(), posix_name.c_str(), posix_target.c_str());
    return false;
  }
  if (!blaze_util::AsAbsoluteWindowsPath(posix_target, &target)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "SymlinkDirectories(%s, %s): AsAbsoluteWindowsPath(%s)",
         posix_target.c_str(), posix_name.c_str(), posix_name.c_str());
    return false;
  }
  string error(CreateJunction(name, target));
  if (!error.empty()) {
    blaze_util::PrintError("SymlinkDirectories(%s, %s): CreateJunction: %s",
                           posix_target.c_str(), posix_name.c_str(),
                           error.c_str());
    return false;
  }
  return true;
}

bool CompareAbsolutePaths(const string& a, const string& b) {
  return ConvertPath(a) == ConvertPath(b);
}

#ifndef STILL_ACTIVE
#define STILL_ACTIVE (259)  // From MSDN about GetExitCodeProcess.
#endif

// On Windows (and Linux) we use a combination of PID and start time to identify
// the server process. That is supposed to be unique unless one can start more
// processes than there are PIDs available within a single jiffy.
bool VerifyServerProcess(int pid, const string& output_base) {
  AutoHandle process(
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process.IsValid()) {
    // Cannot find the server process. Can happen if the PID file is stale.
    return false;
  }

  DWORD exit_code = 0;
  uint64_t start_time = 0;
  if (!::GetExitCodeProcess(process, &exit_code) || exit_code != STILL_ACTIVE ||
      !GetProcessStartupTime(process, &start_time)) {
    // Process doesn't exist or died meantime, all is good. No stale server is
    // present.
    return false;
  }

  string recorded_start_time;
  bool file_present = blaze_util::ReadFile(
      blaze_util::JoinPath(output_base, "server/server.starttime"),
      &recorded_start_time);

  // If start time file got deleted, but PID file didn't, assume that this is an
  // old Bazel process that doesn't know how to write start time files yet.
  return !file_present || recorded_start_time == ToString(start_time);
}

bool KillServerProcess(int pid, const string& output_base) {
  AutoHandle process(::OpenProcess(
      PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  DWORD exitcode = 0;
  if (!process.IsValid() || !::GetExitCodeProcess(process, &exitcode) ||
      exitcode != STILL_ACTIVE) {
    // Cannot find the server process (can happen if the PID file is stale) or
    // it already exited.
    return false;
  }

  BOOL result = TerminateProcess(process, /*uExitCode*/ 0);
  if (!result || !AwaitServerProcessTermination(pid, output_base,
                                                kPostKillGracePeriodSeconds)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "Cannot terminate server process with PID %d, output_base=(%s)", pid,
         output_base.c_str());
  }
  return result;
}

void TrySleep(unsigned int milliseconds) {
  Sleep(milliseconds);
}

// Not supported.
void ExcludePathFromBackup(const string &path) {
}

string GetHashedBaseDir(const string& root, const string& hashable) {
  // Builds a shorter output base dir name for Windows.
  // This algorithm only uses 1/3 of the bits to get 8-char alphanumeric
  // file name.

  static const char* alphabet
      // Exactly 64 characters.
      = "abcdefghigklmnopqrstuvwxyzABCDEFGHIGKLMNOPQRSTUVWXYZ0123456789_-";

  // The length of the resulting filename (8 characters).
  static const int filename_length = blaze_util::Md5Digest::kDigestLength / 2;
  unsigned char buf[blaze_util::Md5Digest::kDigestLength];
  char coded_name[filename_length + 1];
  blaze_util::Md5Digest digest;
  digest.Update(hashable.data(), hashable.size());
  digest.Finish(buf);
  for (int i = 0; i < filename_length; i++) {
    coded_name[i] = alphabet[buf[i] & 0x3F];
  }
  coded_name[filename_length] = '\0';
  return blaze_util::JoinPath(root, string(coded_name));
}

void CreateSecureOutputRoot(const string& path) {
  // TODO(bazel-team): implement this properly, by mimicing whatever the POSIX
  // implementation does.
  const char* root = path.c_str();
  if (!blaze_util::MakeDirectories(path, 0755)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "MakeDirectories(%s) failed", root);
  }

#ifndef COMPILER_MSVC
  struct stat fileinfo = {};

  // The path already exists.
  // Check ownership and mode, and verify that it is a directory.

  if (lstat(root, &fileinfo) < 0) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR, "lstat('%s')", root);
  }

  if (fileinfo.st_uid != geteuid()) {
    die(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR, "'%s' is not owned by me",
        root);
  }

  // Ensure the permission mask is indeed 0755 (rwxr-xr-x).
  if ((fileinfo.st_mode & 022) != 0) {
    int new_mode = fileinfo.st_mode & (~022);
    if (chmod(root, new_mode) < 0) {
      die(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
          "'%s' has mode %o, chmod to %o failed", root,
          fileinfo.st_mode & 07777, new_mode);
    }
  }
#endif  // not COMPILER_MSVC

  if (!blaze_util::IsDirectory(path)) {
    die(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR, "'%s' is not a directory",
        root);
  }

  ExcludePathFromBackup(root);
}

string GetEnv(const string& name) {
  DWORD size = ::GetEnvironmentVariableA(name.c_str(), NULL, 0);
  if (size == 0) {
#ifdef COMPILER_MSVC
    return string();  // unset or empty envvar
#else  // not COMPILER_MSVC
    char* result = getenv(name.c_str());
    return result != NULL ? string(result) : string();
#endif  // COMPILER_MSVC
  }

  unique_ptr<char[]> value(new char[size]);
  ::GetEnvironmentVariableA(name.c_str(), value.get(), size);
  return string(value.get());
}

void SetEnv(const string& name, const string& value) {
  // _putenv_s both calls ::SetEnvionmentVariableA and updates environ(5).
  _putenv_s(name.c_str(), value.c_str());
}

void UnsetEnv(const string& name) { SetEnv(name, ""); }

bool WarnIfStartedFromDesktop() {
  // GetConsoleProcessList returns:
  //   0, if no console attached (Bazel runs as a subprocess)
  //   1, if Bazel was started by clicking on its icon
  //   2, if Bazel was started from the command line (even if its output is
  //      redirected)
  DWORD dummy[2] = {0};
  if (GetConsoleProcessList(dummy, 2) != 1) {
    return false;
  }
  printf(
      "Bazel is a command line tool.\n\n"
      "Try opening a console, such as the Windows Command Prompt (cmd.exe) "
      "or PowerShell, and running \"bazel help\".\n\n"
      "Press Enter to close this window...");
  ReadFile(GetStdHandle(STD_INPUT_HANDLE), dummy, 1, dummy, NULL);
  return true;
}

#ifndef ENABLE_PROCESSED_OUTPUT
// From MSDN about BOOL SetConsoleMode(HANDLE, DWORD).
#define ENABLE_PROCESSED_OUTPUT 0x0001
#endif  // not ENABLE_PROCESSED_OUTPUT

#ifndef ENABLE_WRAP_AT_EOL_OUTPUT
// From MSDN about BOOL SetConsoleMode(HANDLE, DWORD).
#define ENABLE_WRAP_AT_EOL_OUTPUT 0x0002
#endif  // not ENABLE_WRAP_AT_EOL_OUTPUT

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
// From MSDN about BOOL SetConsoleMode(HANDLE, DWORD).
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif  // not ENABLE_VIRTUAL_TERMINAL_PROCESSING

void SetupStdStreams() {
#ifdef COMPILER_MSVC
  static const DWORD stdhandles[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE,
                                     STD_ERROR_HANDLE};
  for (int i = 0; i <= 2; ++i) {
    HANDLE handle = ::GetStdHandle(stdhandles[i]);
    if (handle == INVALID_HANDLE_VALUE || handle == NULL) {
      // Ensure we have open fds to each std* stream. Otherwise we can end up
      // with bizarre things like stdout going to the lock file, etc.
      _open("NUL", (i == 0) ? _O_RDONLY : _O_WRONLY);
    }
    DWORD mode = 0;
    if (i > 0 && handle != INVALID_HANDLE_VALUE && handle != NULL &&
        ::GetConsoleMode(handle, &mode)) {
      DWORD newmode = mode | ENABLE_PROCESSED_OUTPUT |
                      ENABLE_WRAP_AT_EOL_OUTPUT |
                      ENABLE_VIRTUAL_TERMINAL_PROCESSING;
      if (mode != newmode) {
        // We don't care about the success of this. Worst that can happen if
        // this method fails is that the console won't understand control
        // characters like color change or carriage return.
        ::SetConsoleMode(handle, newmode);
      }
    }
  }
#else  // not COMPILER_MSVC
  // Set non-buffered output mode for stderr/stdout. The server already
  // line-buffers messages where it makes sense, so there's no need to do set
  // line-buffering here. On the other hand the server sometimes sends binary
  // output (when for example a query returns results as proto), in which case
  // we must not perform line buffering on the client side. So turn off
  // buffering here completely.
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  // Ensure we have three open fds.  Otherwise we can end up with
  // bizarre things like stdout going to the lock file, etc.
  if (fcntl(STDIN_FILENO, F_GETFL) == -1) open("/dev/null", O_RDONLY);
  if (fcntl(STDOUT_FILENO, F_GETFL) == -1) open("/dev/null", O_WRONLY);
  if (fcntl(STDERR_FILENO, F_GETFL) == -1) open("/dev/null", O_WRONLY);
#endif  // COMPILER_MSVC
}

LARGE_INTEGER WindowsClock::GetFrequency() {
  LARGE_INTEGER result;
  if (!QueryPerformanceFrequency(&result)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "WindowsClock::GetFrequency: QueryPerformanceFrequency");
  }

  // On ancient Windows versions (pre-XP) and specific hardware the result may
  // be 0. Since this is pre-XP, we don't handle that, just error out.
  if (result.QuadPart <= 0) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "WindowsClock::GetFrequency: QueryPerformanceFrequency returned "
         "invalid result (%llu)\n",
         result.QuadPart);
  }

  return result;
}

LARGE_INTEGER WindowsClock::GetMillisecondsAsLargeInt(
    const LARGE_INTEGER& freq) {
  LARGE_INTEGER counter;
  if (!QueryPerformanceCounter(&counter)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "WindowsClock::GetMillisecondsAsLargeInt: QueryPerformanceCounter");
  }

  LARGE_INTEGER result;
  result.QuadPart =
      // seconds
      (counter.QuadPart / freq.QuadPart) * 1000LL +
      // milliseconds
      (((counter.QuadPart % freq.QuadPart) * 1000LL) / freq.QuadPart);

  return result;
}

const WindowsClock WindowsClock::INSTANCE;

WindowsClock::WindowsClock()
    : kFrequency(GetFrequency()),
      kStart(GetMillisecondsAsLargeInt(kFrequency)) {}

uint64_t WindowsClock::GetMilliseconds() const {
  return GetMillisecondsAsLargeInt(kFrequency).QuadPart;
}

uint64_t WindowsClock::GetProcessMilliseconds() const {
  return GetMilliseconds() - kStart.QuadPart;
}

uint64_t AcquireLock(const string& output_base, bool batch_mode, bool block,
                     BlazeLock* blaze_lock) {
  string lockfile = blaze_util::JoinPath(output_base, "lock");
  wstring wlockfile;
  if (!blaze_util::AsAbsoluteWindowsPath(lockfile, &wlockfile)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "AcquireLock(%s): AsAbsoluteWindowsPath(%s)", output_base.c_str(),
         lockfile.c_str());
  }

  blaze_lock->handle = INVALID_HANDLE_VALUE;
  bool first_lock_attempt = true;
  uint64_t st = GetMillisecondsMonotonic();
  while (true) {
    blaze_lock->handle = ::CreateFileW(
        /* lpFileName */ wlockfile.c_str(),
        /* dwDesiredAccess */ GENERIC_READ | GENERIC_WRITE,
        /* dwShareMode */ FILE_SHARE_READ,
        /* lpSecurityAttributes */ NULL,
        /* dwCreationDisposition */ CREATE_ALWAYS,
        /* dwFlagsAndAttributes */ FILE_ATTRIBUTE_NORMAL,
        /* hTemplateFile */ NULL);
    if (blaze_lock->handle != INVALID_HANDLE_VALUE) {
      // We could open the file, so noone else holds a lock on it.
      break;
    }
    if (GetLastError() == ERROR_SHARING_VIOLATION) {
      // Someone else has the lock.
      if (!block) {
        die(blaze_exit_code::BAD_ARGV,
            "Another command is running. Exiting immediately.");
      }
      if (first_lock_attempt) {
        first_lock_attempt = false;
        fprintf(stderr,
                "Another command is running. Waiting for it to complete...");
        fflush(stderr);
      }
      Sleep(/* dwMilliseconds */ 200);
    } else {
      pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
           "AcquireLock(%s): CreateFileW(%ls)", lockfile.c_str(),
           wlockfile.c_str());
    }
  }
  uint64_t wait_time = GetMillisecondsMonotonic() - st;

  // We have the lock.
  OVERLAPPED overlapped = {0};
  if (!LockFileEx(
          /* hFile */ blaze_lock->handle,
          /* dwFlags */ LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
          /* dwReserved */ 0,
          /* nNumberOfBytesToLockLow */ 1,
          /* nNumberOfBytesToLockHigh */ 0,
          /* lpOverlapped */ &overlapped)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "AcquireLock(%s): LockFileEx(%ls)", lockfile.c_str(),
         wlockfile.c_str());
  }
  // On other platforms we write some info about this process into the lock file
  // such as the server PID. On Windows we don't do that because the file is
  // locked exclusively, meaning other processes may not open the file even for
  // reading.

  return wait_time;
}

void ReleaseLock(BlazeLock* blaze_lock) {
  OVERLAPPED overlapped = {0};
  UnlockFileEx(blaze_lock->handle, 0, 1, 0, &overlapped);
  CloseHandle(blaze_lock->handle);
}

#ifdef GetUserName
// By including <windows.h>, we have GetUserName defined either as
// GetUserNameA or GetUserNameW.
#undef GetUserName
#endif

string GetUserName() {
  WCHAR buffer[UNLEN + 1];
  DWORD len = UNLEN + 1;
  if (!::GetUserNameW(buffer, &len)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR, "GetUserNameW");
  }
  return string(blaze_util::WstringToCstring(buffer).get());
}

bool IsEmacsTerminal() {
  string emacs = GetEnv("EMACS");
  string inside_emacs = GetEnv("INSIDE_EMACS");
  // GNU Emacs <25.1 (and ~all non-GNU emacsen) set EMACS=t, but >=25.1 doesn't
  // do that and instead sets INSIDE_EMACS=<stuff> (where <stuff> can look like
  // e.g. "25.1.1,comint").  So we check both variables for maximum
  // compatibility.
  return emacs == "t" || !inside_emacs.empty();
}

// Returns true iff both stdout and stderr are connected to a
// terminal, and it can support color and cursor movement
// (this is computed heuristically based on the values of
// environment variables).
bool IsStandardTerminal() {
#ifdef COMPILER_MSVC
  for (DWORD i : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
    DWORD mode = 0;
    HANDLE handle = ::GetStdHandle(i);
    // handle may be invalid when std{out,err} is redirected
    if (handle == INVALID_HANDLE_VALUE || !::GetConsoleMode(handle, &mode) ||
        !(mode & ENABLE_PROCESSED_OUTPUT) ||
        !(mode & ENABLE_WRAP_AT_EOL_OUTPUT) ||
        !(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
      return false;
    }
  }
  return true;
#else  // not COMPILER_MSVC
  string term = GetEnv("TERM");
  if (term.empty() || term == "dumb" || term == "emacs" ||
      term == "xterm-mono" || term == "symbolics" || term == "9term" ||
      IsEmacsTerminal()) {
    return false;
  }
  return isatty(STDOUT_FILENO) && isatty(STDERR_FILENO);
#endif  // COMPILER_MSVC
}

// Returns the number of columns of the terminal to which stdout is
// connected, or $COLUMNS (default 80) if there is no such terminal.
int GetTerminalColumns() {
#ifndef COMPILER_MSVC
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1) {
    return ws.ws_col;
  }
#endif  // not COMPILER_MSVC

  string columns_env = GetEnv("COLUMNS");
  if (!columns_env.empty()) {
    char* endptr;
    int columns = blaze_util::strto32(columns_env.c_str(), &endptr, 10);
    if (*endptr == '\0') {  // $COLUMNS is a valid number
      return columns;
    }
  }

  HANDLE stdout_handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (stdout_handle != INVALID_HANDLE_VALUE) {
    // stdout_handle may be invalid when stdout is redirected.
    CONSOLE_SCREEN_BUFFER_INFO screen_info;
    if (GetConsoleScreenBufferInfo(stdout_handle, &screen_info)) {
      int width = 1 + screen_info.srWindow.Right - screen_info.srWindow.Left;
      if (width > 1) {
        return width;
      }
    }
  }

  return 80;  // default if not a terminal.
}

bool UnlimitResources() {
  return true;  // Nothing to do so assume success.
}

static const int MAX_KEY_LENGTH = 255;
// We do not care about registry values longer than MAX_PATH
static const int REG_VALUE_BUFFER_SIZE = MAX_PATH;

// Implements heuristics to discover msys2 installation.
static string GetMsysBash() {
  HKEY h_uninstall;

  // MSYS2 installer writes its registry into HKCU, although documentation
  // (https://msdn.microsoft.com/en-us/library/ms954376.aspx)
  // clearly states that it should go to HKLM.
  static const char* const key =
      "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
  if (RegOpenKeyExA(HKEY_CURRENT_USER,  // _In_     HKEY    hKey,
                    key,                // _In_opt_ LPCTSTR lpSubKey,
                    0,                  // _In_     DWORD   ulOptions,
                    KEY_ENUMERATE_SUB_KEYS |
                        KEY_QUERY_VALUE,  // _In_     REGSAM  samDesired,
                    &h_uninstall          // _Out_    PHKEY   phkResult
                    )) {
    debug_log("Cannot open HKCU\\%s", key);
    return string();
  }
  AutoHandle auto_uninstall(h_uninstall);

  // Since MSYS2 decided to generate a new product key for each installation,
  // we enumerate all keys under
  // HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall and find the first
  // with MSYS2 64bit display name.
  static const char* const msys_display_name = "MSYS2 64bit";
  DWORD n_subkeys;

  if (RegQueryInfoKey(h_uninstall,  // _In_        HKEY      hKey,
                      0,            // _Out_opt_   LPTSTR    lpClass,
                      0,            // _Inout_opt_ LPDWORD   lpcClass,
                      0,            // _Reserved_  LPDWORD   lpReserved,
                      &n_subkeys,   // _Out_opt_   LPDWORD   lpcSubKeys,
                      0,            // _Out_opt_   LPDWORD   lpcMaxSubKeyLen,
                      0,            // _Out_opt_   LPDWORD   lpcMaxClassLen,
                      0,            // _Out_opt_   LPDWORD   lpcValues,
                      0,            // _Out_opt_   LPDWORD   lpcMaxValueNameLen,
                      0,            // _Out_opt_   LPDWORD   lpcMaxValueLen,
                      0,  // _Out_opt_   LPDWORD   lpcbSecurityDescriptor,
                      0   // _Out_opt_   PFILETIME lpftLastWriteTime
                      )) {
    debug_log("Cannot query HKCU\\%s", key);
    return string();
  }

  for (DWORD key_index = 0; key_index < n_subkeys; key_index++) {
    char subkey_name[MAX_KEY_LENGTH];
    if (RegEnumKeyA(h_uninstall,         // _In_  HKEY   hKey,
                    key_index,           // _In_  DWORD  dwIndex,
                    subkey_name,         // _Out_ LPTSTR lpName,
                    sizeof(subkey_name)  // _In_  DWORD  cchName
                    )) {
      debug_log("Cannot get %d subkey of HKCU\\%s", key_index, key);
      continue;  // try next subkey
    }

    HKEY h_subkey;
    if (RegOpenKeyEx(h_uninstall,      // _In_     HKEY    hKey,
                     subkey_name,      // _In_opt_ LPCTSTR lpSubKey,
                     0,                // _In_     DWORD   ulOptions,
                     KEY_QUERY_VALUE,  // _In_     REGSAM  samDesired,
                     &h_subkey         // _Out_    PHKEY   phkResult
                     )) {
      debug_log("Failed to open subkey HKCU\\%s\\%s", key, subkey_name);
      continue;  // try next subkey
    }
    AutoHandle auto_subkey(h_subkey);

    BYTE value[REG_VALUE_BUFFER_SIZE];
    DWORD value_length = sizeof(value);
    DWORD value_type;

    if (RegQueryValueEx(h_subkey,       // _In_        HKEY    hKey,
                        "DisplayName",  // _In_opt_    LPCTSTR lpValueName,
                        0,              // _Reserved_  LPDWORD lpReserved,
                        &value_type,    // _Out_opt_   LPDWORD lpType,
                        value,          // _Out_opt_   LPBYTE  lpData,
                        &value_length   // _Inout_opt_ LPDWORD lpcbData
                        )) {
      debug_log("Failed to query DisplayName of HKCU\\%s\\%s", key,
                subkey_name);
      continue;  // try next subkey
    }

    if (value_type == REG_SZ &&
        0 == memcmp(msys_display_name, value, sizeof(msys_display_name))) {
      debug_log("Getting install location of HKCU\\%s\\%s", key, subkey_name);
      BYTE path[REG_VALUE_BUFFER_SIZE];
      DWORD path_length = sizeof(path);
      DWORD path_type;
      if (RegQueryValueEx(
              h_subkey,           // _In_        HKEY    hKey,
              "InstallLocation",  // _In_opt_    LPCTSTR lpValueName,
              0,                  // _Reserved_  LPDWORD lpReserved,
              &path_type,         // _Out_opt_   LPDWORD lpType,
              path,               // _Out_opt_   LPBYTE  lpData,
              &path_length        // _Inout_opt_ LPDWORD lpcbData
              )) {
        debug_log("Failed to query InstallLocation of HKCU\\%s\\%s", key,
                  subkey_name);
        continue;  // try next subkey
      }

      if (path_length == 0 || path_type != REG_SZ) {
        debug_log("Zero-length (%d) install location or wrong type (%d)",
                  path_length, path_type);
        continue;  // try next subkey
      }

      debug_log("Install location of HKCU\\%s\\%s is %s", key, subkey_name,
                path);
      string path_as_string(path, path + path_length - 1);
      string bash_exe = path_as_string + "\\usr\\bin\\bash.exe";
      if (!blaze_util::PathExists(bash_exe)) {
        debug_log("%s does not exist", bash_exe.c_str());
        continue;  // try next subkey
      }

      debug_log("Detected msys bash at %s", bash_exe.c_str());
      return bash_exe;
    }
  }
  return string();
}

// Implements heuristics to discover Git-on-Win installation.
static string GetBashFromGitOnWin() {
  HKEY h_GitOnWin_uninstall;

  // Well-known registry key for Git-on-Windows.
  static const char* const key =
      "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Git_is1";
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,    // _In_     HKEY    hKey,
                    key,                   // _In_opt_ LPCTSTR lpSubKey,
                    0,                     // _In_     DWORD   ulOptions,
                    KEY_QUERY_VALUE,       // _In_     REGSAM  samDesired,
                    &h_GitOnWin_uninstall  // _Out_    PHKEY   phkResult
                    )) {
    debug_log("Cannot open HKCU\\%s", key);
    return string();
  }
  AutoHandle auto_h_GitOnWin_uninstall(h_GitOnWin_uninstall);

  debug_log("Getting install location of HKLM\\%s", key);
  BYTE path[REG_VALUE_BUFFER_SIZE];
  DWORD path_length = sizeof(path);
  DWORD path_type;
  if (RegQueryValueEx(h_GitOnWin_uninstall,  // _In_        HKEY    hKey,
                      "InstallLocation",     // _In_opt_    LPCTSTR lpValueName,
                      0,                     // _Reserved_  LPDWORD lpReserved,
                      &path_type,            // _Out_opt_   LPDWORD lpType,
                      path,                  // _Out_opt_   LPBYTE  lpData,
                      &path_length           // _Inout_opt_ LPDWORD lpcbData
                      )) {
    debug_log("Failed to query InstallLocation of HKLM\\%s", key);
    return string();
  }

  if (path_length == 0 || path_type != REG_SZ) {
    debug_log("Zero-length (%d) install location or wrong type (%d)",
              path_length, path_type);
    return string();
  }

  debug_log("Install location of HKLM\\%s is %s", key, path);
  string path_as_string(path, path + path_length - 1);
  string bash_exe = path_as_string + "\\usr\\bin\\bash.exe";
  if (!blaze_util::PathExists(bash_exe)) {
    debug_log("%s does not exist", bash_exe.c_str());
    return string();
  }

  debug_log("Detected git-on-Windows bash at %s", bash_exe.c_str());
  return bash_exe;
}

static string GetBashFromPath() {
  char found[MAX_PATH];
  string path_list = blaze::GetEnv("PATH");

  // We do not fully replicate all the quirks of search in PATH.
  // There is no system function to do so, and that way lies madness.
  size_t start = 0;
  do {
    // This ignores possibly quoted semicolons in PATH etc.
    size_t end = path_list.find_first_of(";", start);
    string path = path_list.substr(
        start, end != string::npos ? end - start : string::npos);
    // Handle one typical way of quoting (where.exe does not handle this, but
    // CreateProcess does).
    if (path.size() > 1 && path[0] == '"' && path[path.size() - 1] == '"') {
      path = path.substr(1, path.size() - 2);
    }
    if (SearchPathA(path.c_str(),   // _In_opt_  LPCTSTR lpPath,
                    "bash.exe",     // _In_      LPCTSTR lpFileName,
                    0,              // LPCTSTR lpExtension,
                    sizeof(found),  // DWORD   nBufferLength,
                    found,          // _Out_     LPTSTR  lpBuffer,
                    0               // _Out_opt_ LPTSTR  *lpFilePart
                    )) {
      debug_log("bash.exe found on PATH: %s", found);
      return string(found);
    }
    if (end == string::npos) {
      break;
    }
    start = end + 1;
  } while (true);

  debug_log("bash.exe not found on PATH");
  return string();
}

static string LocateBash() {
  string msys_bash = GetMsysBash();
  if (!msys_bash.empty()) {
    return msys_bash;
  }

  string git_on_win_bash = GetBashFromGitOnWin();
  if (!git_on_win_bash.empty()) {
    return git_on_win_bash;
  }

  return GetBashFromPath();
}

void DetectBashOrDie() {
  if (!blaze::GetEnv("BAZEL_SH").empty()) return;

  uint64_t start = blaze::GetMillisecondsMonotonic();

  string bash = LocateBash();
  uint64_t end = blaze::GetMillisecondsMonotonic();
  debug_log("BAZEL_SH detection took %lu msec, found %s", end - start,
            bash.c_str());

  if (!bash.empty()) {
    // Set process environment variable.
    blaze::SetEnv("BAZEL_SH", bash);
  } else {
    printf(
        "Bazel on Windows requires bash.exe and other Unix tools, but we could "
        "not find them.\n"
        "If you do not have them installed, the easiest is to install MSYS2 "
        "from\n"
        "       http://repo.msys2.org/distrib/msys2-x86_64-latest.exe\n"
        "or git-on-Windows from\n"
        "       https://git-scm.com/download/win\n"
        "\n"
        "If you already have bash.exe installed but Bazel cannot find it,\n"
        "set BAZEL_SH environment variable to its location:\n"
        "       set BAZEL_SH=c:\\path\\to\\bash.exe\n");
    exit(1);
  }
}

}  // namespace blaze
