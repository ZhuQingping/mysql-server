/* Copyright (c) 2001, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   Without limiting anything contained in the foregoing, this file,
   which is part of C Driver for MySQL (Connector/C), is also subject to the
   Universal FOSS Exception, version 1.0, a copy of which can be found at
   http://oss.oracle.com/licenses/universal-foss-exception.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file mysys/stacktrace.cc
*/

#include "my_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <cstdint>
#include <string_view>
#if defined(__linux__) || defined(__sun) || defined(__FreeBSD__)
#include <sys/syscall.h>
#ifdef HAVE_EXT_BACKTRACE
#include <backtrace/stacktrace.hpp>
#include "absl/debugging/internal/demangle.h"
#endif
#endif
#include <time.h>

#include <algorithm>
#include <cinttypes>

#include "my_inttypes.h"
#include "my_macros.h"
#include "my_stacktrace.h"
#include "template_utils.h"

#ifndef _WIN32
#include <signal.h>

#include "my_thread.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STACKTRACE

#ifdef __linux__
#include <ctype.h> /* isprint */
#endif

#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#ifdef __FreeBSD__
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

#ifdef __linux__
/* __bss_start doesn't seem to work on FreeBSD and doesn't exist on OSX/Solaris.
 */
static const char *heap_start;
extern char *__bss_start;
#endif /* __linux */

#ifdef __linux__
static constexpr uint64_t NULLPTR_BOUND = 4095;
constexpr uint32_t MAX_LINE_SIZE = 256;
constexpr uint32_t MAX_CHUNK_SIZE = 4096;
constexpr uint32_t HEX_PER_WORD = 17;
#endif

static inline bool ptr_sane(const char *p [[maybe_unused]],
                            const char *heap_end [[maybe_unused]]) {
#ifdef __linux__
  return p && p >= heap_start && p <= heap_end;
#else
  return true;
#endif
}

void my_init_stacktrace() {
#ifdef __linux__
  heap_start = (char *)&__bss_start;
#endif /* __linux__ */

#ifdef HAVE_EXT_BACKTRACE
  my_warmup_ext_backtrace();
#endif
}

#ifdef __linux__

static void print_buffer(char *buffer, size_t count) {
  const char s[] = " ";
  for (; count && *buffer; --count) {
    my_write_stderr(isprint(*buffer) ? buffer : s, 1);
    ++buffer;
  }
}

/**
  Access the pages of this process through /proc/self/task/<tid>/mem
  in order to safely print the contents of a memory address range.

  @param  addr      The address at the start of the memory region.
  @param  max_len   The length of the memory region.

  @return Zero on success.
*/
static int safe_print_str(const char *addr, int max_len) {
  int fd;
  pid_t tid;
  off_t offset;
  ssize_t nbytes = 0;
  size_t total, count;
  char buf[MAX_LINE_SIZE];

  tid = (pid_t)syscall(SYS_gettid);

  sprintf(buf, "/proc/self/task/%d/mem", tid);

  if ((fd = open(buf, O_RDONLY)) < 0) return -1;

  static_assert(sizeof(off_t) >= sizeof(intptr),
                "off_t needs to be able to hold a pointer.");

  total = max_len;
  offset = (intptr)addr;

  /* Read up to the maximum number of bytes. */
  while (total) {
    count = std::min(sizeof(buf), total);

    if ((nbytes = pread(fd, buf, count, offset)) < 0) {
      /* Just in case... */
      if (errno == EINTR)
        continue;
      else
        break;
    }

    /* Advance offset into memory. */
    total -= nbytes;
    offset += nbytes;
    addr += nbytes;

    /* Output the printable characters. */
    print_buffer(buf, nbytes);

    /* Break if less than requested... */
    if ((count - nbytes)) break;
  }

  /* Output a new line if something was printed. */
  if (total != (size_t)max_len) my_safe_printf_stderr("%s", "\n");

  if (nbytes == -1) my_safe_printf_stderr("Can't read from address %p\n", addr);

  close(fd);

  return 0;
}

#endif /* __linux __ */

void my_safe_puts_stderr(const char *val, size_t max_len) {
  const char *heap_end = nullptr;
#ifdef __linux__
  if (!safe_print_str(val, max_len)) return;

  /* Only needed by the linux version of ptr_sane() */
  heap_end = static_cast<const char *>(sbrk(0));
#endif

  if (!ptr_sane(val, heap_end)) {
    my_safe_printf_stderr("%s", "is an invalid pointer\n");
    return;
  }

  for (; max_len && ptr_sane(val, heap_end) && *val; --max_len)
    my_write_stderr((val++), 1);
  my_safe_printf_stderr("%s", "\n");
}

#ifdef HAVE_EXT_BACKTRACE
struct cookie_t {
  int index;
};

/**
  loading symbols is slow, we can warm it at startup
 */
static void *preload_symbols(void *) {
  cookie_t cookie{.index = 0};
  auto dummy_print_callback = [](void *, uintptr_t, const char *, int,
                                 const char *) { return 0; };
  auto dummy_error_callback = [](void *, const char *, int) {};
  stacktrace::full(1, dummy_print_callback, dummy_error_callback, &cookie);
  return nullptr;
}

/**
 warm up the symbol, speedup extracting file name and lineno
*/
void my_warmup_ext_backtrace() {
  my_thread_handle thread_id;
  my_thread_attr_t thr_attr;
  my_thread_attr_init(&thr_attr);
  my_thread_attr_setdetachstate(&thr_attr, MY_THREAD_CREATE_DETACHED);
  /* Automatically release resources when the thread exits */
  my_thread_create(&thread_id, &thr_attr, preload_symbols, nullptr);

  my_thread_attr_destroy(&thr_attr);
}

/**
 Demangle a mangled C++ symbol name.
 This function is async-signal-safe (unlike abi::__cxa_demangle).

 @param mangled_name The mangled symbol name (starting with "_Z")
 @param buf Buffer to store the demangled name
 @param buf_size Size of the output buffer

 @return true if demangling succeeded, false otherwise
*/
static bool my_demangle_safe(const char *mangled_name, char *buf,
                             size_t buf_size) {
  return absl::debugging_internal::Demangle(mangled_name, buf, buf_size);
}
static size_t my_write_stderr(const std::string_view &sv) {
  return my_write_stderr(std::data(sv), std::size(sv));
}

void my_print_stacktrace(const uchar *stack_bottom, ulong thread_stack) {
  my_safe_printf_stderr("stack_bottom = %p thread_stack 0x%lx\n", stack_bottom,
                        thread_stack);

  auto print_callback = [](void *cookie, uintptr_t pc, const char *filename,
                           int lineno, const char *function) {
    auto idx = static_cast<cookie_t *>(cookie)->index++;
    my_safe_printf_stderr(" #%d 0x%lx ", idx, (unsigned long)pc);
    if (function == nullptr) {
      my_write_stderr("<unknown>");
    } else {
#ifdef HAVE_ABI_CXA_DEMANGLE
      static constexpr size_t kDemangleBufferSize = 256;
      char demangled_buf[kDemangleBufferSize];
      bool ok =
          my_demangle_safe(function, demangled_buf, sizeof(demangled_buf));
      my_write_stderr(ok ? demangled_buf : function);
#else
      my_write_stderr(function);
#endif
    }
    if (filename != nullptr) {
      std::string_view filename_sv{filename};
      constexpr std::string_view parent_dir{"../"};
      if (auto pos = filename_sv.rfind(parent_dir);
          pos != std::string_view::npos) {
        filename_sv.remove_prefix(pos + std::size(parent_dir));
      }
      constexpr std::string_view mysql_dir{"/mysql-server/"};
      constexpr std::string_view dstore_dir{"/dstore/"};
      if (auto pos = filename_sv.find(mysql_dir);
          pos != std::string_view::npos) {
        filename_sv.remove_prefix(pos);
      } else if (pos = filename_sv.find(dstore_dir);
                 pos != std::string_view::npos) {
        filename_sv.remove_prefix(pos);
      }
      my_write_stderr(" at ");
      my_write_stderr(filename_sv);
      my_safe_printf_stderr(":%d", lineno);
    }
    my_write_stderr("\n");

    return 0;
  };
  auto error_callback = [](void *, const char *msg, int errnum) {
    my_safe_printf_stderr("libbacktrace: %s", msg);
    if (errnum > 0) my_safe_printf_stderr(": %d", errnum);
    my_safe_printf_stderr("\n");
  };
  cookie_t cookie{.index = 0};
  stacktrace::full(1, print_callback, error_callback, &cookie);
}
#elif defined(HAVE_BACKTRACE)

#ifdef HAVE_ABI_CXA_DEMANGLE

#include <cxxabi.h>

static char *my_demangle(const char *mangled_name, int *status) {
  return abi::__cxa_demangle(mangled_name, nullptr, nullptr, status);
}

static bool my_demangle_symbol(char *line) {
  char *demangled = nullptr;
#ifdef __APPLE__  // OS X formatting of stacktraces is different from Linux
  char *begin = strstr(line, "_Z");
  char *end = begin ? strchr(begin, ' ') : NULL;

  if (begin && end) {
    begin[-1] = '\0';
    *end = '\0';
    int status;
    demangled = my_demangle(begin, &status);
    if (!demangled || status) {
      demangled = NULL;
      begin[-1] = '_';
      *end = ' ';
    }
  }
  if (demangled) my_safe_printf_stderr("%s %s %s\n", line, demangled, end + 1);
#else             // !__APPLE__
  char *begin = strchr(line, '(');
  char *end = begin ? strchr(begin, '+') : nullptr;

  if (begin && end) {
    *begin++ = *end++ = '\0';
    int status;
    demangled = my_demangle(begin, &status);
    if (!demangled || status) {
      demangled = nullptr;
      begin[-1] = '(';
      end[-1] = '+';
    }
  }
  if (demangled) my_safe_printf_stderr("%s(%s+%s\n", line, demangled, end);
#endif
  bool ret = (demangled == nullptr);
  free(demangled);
  return (ret);
}

// If it does not start with "_Z" it is a C function, and demangling fails.
// Print the original line, with modifications done by my_demangle_symbol().
static void my_demangle_symbols(char **addrs, int n) {
  for (int i = 0; i < n; i++) {
    if (my_demangle_symbol(addrs[i]))  // demangling failed
      my_safe_printf_stderr("%s\n", addrs[i]);
  }
}

#endif /* HAVE_ABI_CXA_DEMANGLE */

void my_print_stacktrace(const uchar *stack_bottom, ulong thread_stack) {
#if defined(__FreeBSD__)
  static char procname_buffer[2048];
  unw_cursor_t cursor;
  unw_context_t uc;
  unw_word_t ip;

  unw_getcontext(&uc);
  unw_init_local(&cursor, &uc);
  unw_word_t offp;
  while (unw_step(&cursor) > 0) {
    unw_get_reg(&cursor, UNW_REG_IP, &ip);
    unw_get_proc_name(&cursor, procname_buffer, sizeof(procname_buffer), &offp);
    int status;
    char *demangled = my_demangle(procname_buffer, &status);
    my_safe_printf_stderr("[0x%lx] %s+0x%lx\n", ip,
                          demangled ? demangled : procname_buffer, offp);
    if (demangled) free(demangled);
  }
#endif
  void *addrs[128];
  char **strings = nullptr;
  int n = backtrace(addrs, array_elements(addrs));
  my_safe_printf_stderr("stack_bottom = %p thread_stack 0x%lx\n", stack_bottom,
                        thread_stack);
#ifdef HAVE_ABI_CXA_DEMANGLE
  if ((strings = backtrace_symbols(addrs, n))) {
    my_demangle_symbols(strings, n);
    free(strings);
  }
#endif
  if (!strings) {
    backtrace_symbols_fd(addrs, n, fileno(stderr));
  }
}
#endif /* HAVE_EXT_BACKTRACE || HAVE_BACKTRACE */
#endif /* HAVE_STACKTRACE */

/* Produce a core for the thread */
void my_write_core(int sig) {
  signal(sig, SIG_DFL);
  pthread_kill(my_thread_self(), sig);
#if defined(P_MYID)
  /* On Solaris, the above kill is not enough */
  sigsend(P_PID, P_MYID, sig);
#endif
}

#else /* _WIN32*/

#include <dbghelp.h>
#include <tlhelp32.h>
#if _MSC_VER
#pragma comment(lib, "dbghelp")
#endif

static thread_local EXCEPTION_POINTERS *exception_ptrs;

#define MODULE64_SIZE_WINXP 576
#define STACKWALK_MAX_FRAMES 64

void my_init_stacktrace() {}

void my_set_exception_pointers(EXCEPTION_POINTERS *ep) { exception_ptrs = ep; }

/*
  Appends directory to symbol path.
*/
static void add_to_symbol_path(char *path, size_t path_buffer_size, char *dir,
                               size_t dir_buffer_size) {
  strcat_s(dir, dir_buffer_size, ";");
  if (!strstr(path, dir)) {
    strcat_s(path, path_buffer_size, dir);
  }
}

/*
  Get symbol path - semicolon-separated list of directories to search for debug
  symbols. We expect PDB in the same directory as corresponding exe or dll,
  so the path is build from directories of the loaded modules. If environment
  variable _NT_SYMBOL_PATH is set, it's value appended to the symbol search path
*/
static void get_symbol_path(char *path, size_t size) {
  HANDLE hSnap;
  char *envvar;
  char *p;
#ifndef NDEBUG
  static char pdb_debug_dir[MAX_PATH + 7];
#endif

  path[0] = '\0';

#ifndef NDEBUG
  /*
    Add "debug" subdirectory of the application directory, sometimes PDB will
    placed here by installation.
  */
  GetModuleFileName(NULL, pdb_debug_dir, MAX_PATH);
  p = strrchr(pdb_debug_dir, '\\');
  if (p) {
    *p = 0;
    strcat_s(pdb_debug_dir, sizeof(pdb_debug_dir), "\\debug;");
    add_to_symbol_path(path, size, pdb_debug_dir, sizeof(pdb_debug_dir));
  }
#endif

  /*
    Enumerate all modules, and add their directories to the path.
    Avoid duplicate entries.
  */
  hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (hSnap != INVALID_HANDLE_VALUE) {
    BOOL ret;
    MODULEENTRY32 mod;
    mod.dwSize = sizeof(MODULEENTRY32);
    for (ret = Module32First(hSnap, &mod); ret;
         ret = Module32Next(hSnap, &mod)) {
      char *module_dir = mod.szExePath;
      p = strrchr(module_dir, '\\');
      if (!p) {
        /*
          Path separator was not found. Not known to happen, if ever happens,
          will indicate current directory.
        */
        module_dir[0] = '.';
        module_dir[1] = '\0';
      } else {
        *p = '\0';
      }
      add_to_symbol_path(path, size, module_dir, sizeof(mod.szExePath));
    }
    CloseHandle(hSnap);
  }

  /* Add _NT_SYMBOL_PATH, if present. */
  envvar = getenv("_NT_SYMBOL_PATH");
  if (envvar) {
    strcat_s(path, size, envvar);
  }
}

#define MAX_SYMBOL_PATH 32768

/* Platform SDK in VS2003 does not have definition for SYMOPT_NO_PROMPTS*/
#ifndef SYMOPT_NO_PROMPTS
#define SYMOPT_NO_PROMPTS 0
#endif

void my_print_stacktrace(const uchar * /* stack_bottom */,
                         ulong /* thread_stack */) {
  HANDLE hProcess = GetCurrentProcess();
  HANDLE hThread = GetCurrentThread();
  static IMAGEHLP_MODULE64 module{};
  module.SizeOfStruct = sizeof(module);

  static IMAGEHLP_SYMBOL64_PACKAGE package;
  DWORD64 addr;
  DWORD machine;
  int i;
  CONTEXT context;
  STACKFRAME64 frame{};
  static char symbol_path[MAX_SYMBOL_PATH];

  if (exception_ptrs) {
    /* Copy context, as stackwalking on original will unwind the stack */
    context = *(exception_ptrs->ContextRecord);
  } else {
    /*
      We are to print stack outside the signal handler, let's just capture the
      current context.
    */
    RtlCaptureContext(&context);
  }

  /*Initialize symbols.*/
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_NO_PROMPTS | SYMOPT_DEFERRED_LOADS |
                SYMOPT_DEBUG);
  get_symbol_path(symbol_path, sizeof(symbol_path));
  SymInitialize(hProcess, symbol_path, true);

  /*Prepare stackframe for the first StackWalk64 call*/
  frame.AddrFrame.Mode = frame.AddrPC.Mode = frame.AddrStack.Mode =
      AddrModeFlat;
#if (defined _M_IX86)
  machine = IMAGE_FILE_MACHINE_I386;
  frame.AddrFrame.Offset = context.Ebp;
  frame.AddrPC.Offset = context.Eip;
  frame.AddrStack.Offset = context.Esp;
#elif (defined _M_X64)
  machine = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrPC.Offset = context.Rip;
  frame.AddrStack.Offset = context.Rsp;
#else
  /*There is currently no need to support IA64*/
  /* Warning C4068: unknown pragma 'error' */
#pragma error("unsupported architecture")
#endif

  package.sym.SizeOfStruct = sizeof(package.sym);
  package.sym.MaxNameLength = sizeof(package.name);

  /*Walk the stack, output useful information*/
  for (i = 0; i < STACKWALK_MAX_FRAMES; i++) {
    DWORD64 function_offset = 0;
    DWORD line_offset = 0;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);

    BOOL have_module = false;
    BOOL have_symbol = false;
    BOOL have_source = false;

    if (!StackWalk64(machine, hProcess, hThread, &frame, &context, 0, 0, 0, 0))
      break;
    addr = frame.AddrPC.Offset;

    have_module = SymGetModuleInfo64(hProcess, addr, &module);
    have_symbol =
        SymGetSymFromAddr64(hProcess, addr, &function_offset, &(package.sym));
    have_source = SymGetLineFromAddr64(hProcess, addr, &line_offset, &line);

    my_safe_printf_stderr("%llx    ", addr);
    if (have_module) {
      char *base_image_name = strrchr(module.ImageName, '\\');
      if (base_image_name)
        base_image_name++;
      else
        base_image_name = module.ImageName;
      my_safe_printf_stderr("%s!", base_image_name);
    }
    if (have_symbol)
      my_safe_printf_stderr("%s()", package.sym.Name);

    else if (have_module)
      my_safe_printf_stderr("%s", "???");

    if (have_source) {
      char *base_file_name = strrchr(line.FileName, '\\');
      if (base_file_name)
        base_file_name++;
      else
        base_file_name = line.FileName;
      my_safe_printf_stderr("[%s:%lu]", base_file_name, line.LineNumber);
    }
    my_safe_printf_stderr("%s", "\n");
  }
}

/*
  Write dump. The dump is created in current directory,
  file name is constructed from executable name plus
  ".dmp" extension
*/
void my_write_core(int /* sig */) {
  char path[MAX_PATH];
  // See comment below for clarification about size of dump_fname
  char dump_fname[MAX_PATH + 1 + 10 + 4 + 1] = "core.dmp";

  if (!exception_ptrs) return;

  if (GetModuleFileName(NULL, path, sizeof(path))) {
    char module_name[MAX_PATH];
    _splitpath(path, NULL, NULL, module_name, NULL);
    // max length of a value being placed to dump_fname is
    // MAX_PATH + 1 byte for '.' + up to 10 bytes for string
    // representation of DWORD value + 4 bytes for .dmp suffix +
    // 1 byte for termitated \0. Such size of output buffer guarantees
    // that there is enough space to place a result of string formatting
    // performed by snprintf().
    snprintf(dump_fname, sizeof(dump_fname), "%s.%lu.dmp", module_name,
             GetCurrentProcessId());
  }
  my_create_minidump(dump_fname, 0, 0);
}

/** Create a minidump.
  @param name    path of minidump file.
  @param process HANDLE to process. (0 for own process).
  @param pid     Process id.
*/

void my_create_minidump(const char *name, HANDLE process, DWORD pid) {
  char path[MAX_PATH];
  MINIDUMP_EXCEPTION_INFORMATION info;
  PMINIDUMP_EXCEPTION_INFORMATION info_ptr = NULL;
  HANDLE hFile;

  if (process == 0) {
    /* Does not need to CloseHandle() for the below. */
    process = GetCurrentProcess();
    pid = GetCurrentProcessId();
    info.ExceptionPointers = exception_ptrs;
    info.ClientPointers = false;
    info.ThreadId = GetCurrentThreadId();
    info_ptr = &info;
  }

  hFile = CreateFile(name, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, 0);
  if (hFile) {
    MINIDUMP_TYPE mdt =
        (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo |
                        MiniDumpWithProcessThreadData);
    /* Create minidump, use info only if same process. */
    if (MiniDumpWriteDump(process, pid, hFile, mdt, info_ptr, 0, 0)) {
      my_safe_printf_stderr("Minidump written to %s\n",
                            _fullpath(path, name, sizeof(path)) ? path : name);
    } else {
      my_safe_printf_stderr("MiniDumpWriteDump() failed, last error %lu\n",
                            GetLastError());
    }
    CloseHandle(hFile);
  } else {
    my_safe_printf_stderr("CreateFile(%s) failed, last error %lu\n", name,
                          GetLastError());
  }
}

void my_safe_puts_stderr(const char *val, size_t len) {
  __try {
    my_write_stderr(val, len);
    my_safe_printf_stderr("%s", "\n");
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    my_safe_printf_stderr("%s", "is an invalid string pointer\n");
  }
}
#endif /* _WIN32 */

#ifdef _WIN32
size_t my_write_stderr(const void *buf, size_t count) {
  DWORD bytes_written;
  SetFilePointer(GetStdHandle(STD_ERROR_HANDLE), 0, NULL, FILE_END);
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, (DWORD)count, &bytes_written,
            NULL);
  return bytes_written;
}
#else
size_t my_write_stderr(const void *buf, size_t count) {
  return (size_t)write(STDERR_FILENO, buf, count);
}
#endif

static const char digits[] = "0123456789abcdef";

char *my_safe_utoa(int base, ulonglong val, char *buf) {
  *buf-- = 0;
  do {
    *buf-- = digits[val % base];
  } while ((val /= base) != 0);
  return buf + 1;
}

char *my_safe_itoa(int base, longlong val, char *buf) {
  char *orig_buf = buf;
  const bool is_neg = (val < 0);
  *buf-- = 0;

  if (is_neg) val = -val;
  if (is_neg && base == 16) {
    int ix;
    val -= 1;
    for (ix = 0; ix < 16; ++ix) buf[-ix] = '0';
  }

  do {
    *buf-- = digits[val % base];
  } while ((val /= base) != 0);

  if (is_neg && base == 10) *buf-- = '-';

  if (is_neg && base == 16) {
    int ix;
    buf = orig_buf - 1;
    for (ix = 0; ix < 16; ++ix, --buf) {
      switch (*buf) {
        case '0':
          *buf = 'f';
          break;
        case '1':
          *buf = 'e';
          break;
        case '2':
          *buf = 'd';
          break;
        case '3':
          *buf = 'c';
          break;
        case '4':
          *buf = 'b';
          break;
        case '5':
          *buf = 'a';
          break;
        case '6':
          *buf = '9';
          break;
        case '7':
          *buf = '8';
          break;
        case '8':
          *buf = '7';
          break;
        case '9':
          *buf = '6';
          break;
        case 'a':
          *buf = '5';
          break;
        case 'b':
          *buf = '4';
          break;
        case 'c':
          *buf = '3';
          break;
        case 'd':
          *buf = '2';
          break;
        case 'e':
          *buf = '1';
          break;
        case 'f':
          *buf = '0';
          break;
      }
    }
  }
  return buf + 1;
}

static const char *check_longlong(const char *fmt, bool *have_longlong) {
  *have_longlong = false;
  if (*fmt == 'l') {
    fmt++;
    if (*fmt != 'l')
      *have_longlong = (sizeof(long) == sizeof(longlong));
    else {
      fmt++;
      *have_longlong = true;
    }
  }
  return fmt;
}

static size_t my_safe_vsnprintf(char *to, size_t size, const char *format,
                                va_list ap) {
  char *start = to;
  char *end = start + size - 1;
  for (; *format; ++format) {
    bool have_longlong = false;
    if (*format != '%') {
      if (to == end) /* end of buffer */
        break;
      *to++ = *format; /* copy ordinary char */
      continue;
    }
    ++format; /* skip '%' */

    format = check_longlong(format, &have_longlong);

    switch (*format) {
      case 'd':
      case 'i':
      case 'u':
      case 'x':
      case 'p': {
        longlong ival = 0;
        ulonglong uval = 0;
        if (*format == 'p')
          have_longlong = (sizeof(void *) == sizeof(longlong));
        if (have_longlong) {
          if (*format == 'u')
            uval = va_arg(ap, ulonglong);
          else
            ival = va_arg(ap, longlong);
        } else {
          if (*format == 'u')
            uval = va_arg(ap, unsigned int);
          else
            ival = va_arg(ap, int);
        }

        {
          char buff[22];
          const int base = (*format == 'x' || *format == 'p') ? 16 : 10;
          char *val_as_str =
              (*format == 'u')
                  ? my_safe_utoa(base, uval, &buff[sizeof(buff) - 1])
                  : my_safe_itoa(base, ival, &buff[sizeof(buff) - 1]);

          /*
            Strip off "ffffffff" if we have 'x' format without 'll'
            Similarly for 'p' format on 32bit systems.
          */
          if (base == 16 && !have_longlong && ival < 0) val_as_str += 8;

          while (*val_as_str && to < end) *to++ = *val_as_str++;
          continue;
        }
      }
      case 's': {
        const char *val = va_arg(ap, char *);
        if (!val) val = "(null)";
        while (*val && to < end) *to++ = *val++;
        continue;
      }
    }
  }
  *to = 0;
  return to - start;
}

size_t my_safe_snprintf(char *to, size_t n, const char *fmt, ...) {
  size_t result;
  va_list args;
  va_start(args, fmt);
  result = my_safe_vsnprintf(to, n, fmt, args);
  va_end(args);
  return result;
}

size_t my_safe_printf_stderr(const char *fmt, ...) {
  char to[512];
  size_t result;
  va_list args;
  va_start(args, fmt);
  result = my_safe_vsnprintf(to, sizeof(to), fmt, args);
  va_end(args);
  my_write_stderr(to, result);
  return result;
}

void my_safe_print_system_time() {
  char hrs_buf[3] = "00";
  char mins_buf[3] = "00";
  char secs_buf[3] = "00";
  int base = 10;
#ifdef _WIN32
  SYSTEMTIME utc_time;
  long hrs, mins, secs;
  GetSystemTime(&utc_time);
  hrs = utc_time.wHour;
  mins = utc_time.wMinute;
  secs = utc_time.wSecond;
#else
  /* Using time() instead of my_time() to avoid looping */
  const time_t curr_time = time(nullptr);
  /* Calculate time of day */
  const long tmins = curr_time / 60;
  const long thrs = tmins / 60;
  const long hrs = thrs % 24;
  const long mins = tmins % 60;
  const long secs = curr_time % 60;
#endif

  my_safe_itoa(base, hrs, &hrs_buf[2]);
  my_safe_itoa(base, mins, &mins_buf[2]);
  my_safe_itoa(base, secs, &secs_buf[2]);

  my_safe_printf_stderr("---------- %s:%s:%s UTC - ", hrs_buf, mins_buf,
                        secs_buf);
}

/* ============================================================
 * FFIC: Enhanced Crash Diagnostics
 * All functions below are new additions appended to this file.
 * No existing code was modified.
 *
 * Async-signal-safe contract: only write(2), read(2), open(2),
 * close(2), uname(2), and dladdr(3) are used.  No heap allocation.
 * Supported on Linux x86_64 and AArch64.
 * ============================================================ */
#if defined(__linux__) && !defined(_WIN32)
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <ucontext.h>

/* Print a formatted section separator: "\n=== Title ===\n" */
static void ffic_section(const char *title) {
  my_safe_printf_stderr("\n=== %s ===\n", title);
}

/* Format a 64-bit value as exactly 16 lowercase hex digits into buf[17]. */
static void ffic_fmt_hex64(char *buf, uint64_t v) {
  static const char kH[] = "0123456789abcdef";
  for (int i = 15; i >= 0; --i, v >>= 4) buf[i] = kH[v & 0xf];
  buf[16] = '\0';
}

/* Format an 8-bit value as exactly 2 lowercase hex digits into buf[3]. */
static void ffic_fmt_hex8(char *buf, unsigned char v) {
  static const char kH[] = "0123456789abcdef";
  buf[0] = kH[v >> 4];
  buf[1] = kH[v & 0xf];
  buf[2] = '\0';
}

#if defined(__x86_64__) || defined(__aarch64__)

/**
 * Print a full CPU register dump from the ucontext captured at crash time.
 *
 * Each register is printed as a 16-digit hex value.  A one-line explanation
 * of the most important registers is included so that engineers unfamiliar
 * with the ABI can understand what they are looking at.
 *
 * @param uc  Pointer to ucontext_t from the SA_SIGINFO handler,
 *                      or nullptr (in which case this function is a no-op).
 */
void ffic_print_registers(const ucontext_t *uc) {
  if (!uc) return;
  char h0[HEX_PER_WORD], h1[HEX_PER_WORD], h2[HEX_PER_WORD], h3[HEX_PER_WORD];

#if defined(__x86_64__)
  ffic_section("CPU Registers (x86_64)");

  /* Brief legend so non-OS-experts can orient themselves. */
  my_safe_printf_stderr(
      "  Explanation: Registers are the CPU's fastest storage locations.\n"
      "  At crash time they capture what the CPU was operating on.\n");
  my_safe_printf_stderr(
      "  RIP = faulting instruction  RSP = stack pointer"
      "  RBP = frame pointer\n");
  my_safe_printf_stderr(
      "  RDI = 1st arg / 'this'  RSI = 2nd arg"
      "  RAX = return value / syscall#\n");

  const greg_t *gr = uc->uc_mcontext.gregs;

  /* Control registers */
  ffic_fmt_hex64(h0, (uint64_t)gr[REG_RIP]);
  ffic_fmt_hex64(h1, (uint64_t)gr[REG_RSP]);
  ffic_fmt_hex64(h2, (uint64_t)gr[REG_RBP]);
  ffic_fmt_hex64(h3, (uint64_t)gr[REG_EFL]);
  my_safe_printf_stderr("  RIP = 0x%s  RSP = 0x%s\n", h0, h1);
  my_safe_printf_stderr("  RBP = 0x%s  EFL = 0x%s\n", h2, h3);

  /* CPU trap / fault codes */
  ffic_fmt_hex64(h0, (uint64_t)gr[REG_TRAPNO]);
  ffic_fmt_hex64(h1, (uint64_t)gr[REG_ERR]);
  my_safe_printf_stderr("  TRAPNO = 0x%s  ERR = 0x%s\n", h0, h1);
  uint64_t trapno = (uint64_t)gr[REG_TRAPNO];
  if (trapno == 0x0e)
    my_safe_printf_stderr(
        "  [TRAPNO=0x0e: page fault; ERR bits: bit0=protection-violation,"
        " bit1=write-access, bit2=user-mode]\n");
  else if (trapno == 0x0d)
    my_safe_printf_stderr(
        "  [TRAPNO=0x0d: general protection fault"
        " -- invalid memory access or privilege violation]\n");
  else if (trapno == 0x00)
    my_safe_printf_stderr("  [TRAPNO=0x00: divide-by-zero]\n");

  /* General-purpose registers -- two per line for readability */
  my_safe_printf_stderr("  General-purpose registers:\n");
  ffic_fmt_hex64(h0, (uint64_t)gr[REG_RAX]);
  ffic_fmt_hex64(h1, (uint64_t)gr[REG_RBX]);
  my_safe_printf_stderr("  RAX = 0x%s  RBX = 0x%s\n", h0, h1);
  ffic_fmt_hex64(h0, (uint64_t)gr[REG_RCX]);
  ffic_fmt_hex64(h1, (uint64_t)gr[REG_RDX]);
  my_safe_printf_stderr("  RCX = 0x%s  RDX = 0x%s\n", h0, h1);
  ffic_fmt_hex64(h0, (uint64_t)gr[REG_RSI]);
  ffic_fmt_hex64(h1, (uint64_t)gr[REG_RDI]);
  my_safe_printf_stderr("  RSI = 0x%s  RDI = 0x%s\n", h0, h1);
  ffic_fmt_hex64(h0, (uint64_t)gr[REG_R8]);
  ffic_fmt_hex64(h1, (uint64_t)gr[REG_R9]);
  my_safe_printf_stderr("  R8  = 0x%s  R9  = 0x%s\n", h0, h1);
  ffic_fmt_hex64(h0, (uint64_t)gr[REG_R10]);
  ffic_fmt_hex64(h1, (uint64_t)gr[REG_R11]);
  my_safe_printf_stderr("  R10 = 0x%s  R11 = 0x%s\n", h0, h1);
  ffic_fmt_hex64(h0, (uint64_t)gr[REG_R12]);
  ffic_fmt_hex64(h1, (uint64_t)gr[REG_R13]);
  my_safe_printf_stderr("  R12 = 0x%s  R13 = 0x%s\n", h0, h1);
  ffic_fmt_hex64(h0, (uint64_t)gr[REG_R14]);
  ffic_fmt_hex64(h1, (uint64_t)gr[REG_R15]);
  my_safe_printf_stderr("  R14 = 0x%s  R15 = 0x%s\n", h0, h1);

#elif defined(__aarch64__)
  ffic_section("CPU Registers (AArch64)");

  my_safe_printf_stderr(
      "  Explanation: Registers are the CPU's fastest storage locations.\n"
      "  At crash time they capture what the CPU was operating on.\n");
  my_safe_printf_stderr("  PC = faulting instruction  SP = stack pointer\n");
  my_safe_printf_stderr(
      "  x29(FP) = frame pointer  x30(LR) = return address\n");
  my_safe_printf_stderr(
      "  x0 = 1st arg / 'this' / return value  x1 = 2nd arg\n");

  const mcontext_t *mc = &uc->uc_mcontext;

  /* Special registers first */
  ffic_fmt_hex64(h0, (uint64_t)mc->pc);
  ffic_fmt_hex64(h1, (uint64_t)mc->sp);
  ffic_fmt_hex64(h2, (uint64_t)mc->regs[30]); /* LR */
  ffic_fmt_hex64(h3, (uint64_t)mc->pstate);
  my_safe_printf_stderr("  PC      = 0x%s  SP     = 0x%s\n", h0, h1);
  my_safe_printf_stderr("  LR(x30) = 0x%s  PSTATE = 0x%s\n", h2, h3);

  /* x0-x29 printed two per line */
  my_safe_printf_stderr("  General-purpose registers (x0-x29):\n");
  for (int i = 0; i < 30; i += 2) {
    char ha[HEX_PER_WORD], hb[HEX_PER_WORD];
    ffic_fmt_hex64(ha, (uint64_t)mc->regs[i]);
    ffic_fmt_hex64(hb, (uint64_t)mc->regs[i + 1]);
    my_safe_printf_stderr("  x%-2d = 0x%s  x%-2d = 0x%s\n", i, ha, i + 1, hb);
  }
#endif /* __x86_64__ / __aarch64__ */
}

/**
 * Print human-readable diagnostic hints derived from register values.
 *
 * This section is aimed at engineers who are not CPU/OS experts.
 * Each hint explains what a suspicious register value means and what
 * action to take.
 *
 * @param uc  Pointer to ucontext_t, or nullptr.
 */
void ffic_print_register_hints(const ucontext_t *uc) {
  if (!uc) return;
  char hbuf[HEX_PER_WORD];

  ffic_section("Register Diagnostic Hints");
  my_safe_printf_stderr(
      "  This section translates key register values into actionable clues.\n"
      "  Even without OS expertise, these hints point to the root cause.\n");

#if defined(__x86_64__)
  const greg_t *gr = uc->uc_mcontext.gregs;
  uint64_t rip = (uint64_t)gr[REG_RIP];
  uint64_t rsp = (uint64_t)gr[REG_RSP];
  uint64_t rbp = (uint64_t)gr[REG_RBP];
  uint64_t rdi = (uint64_t)gr[REG_RDI];
  uint64_t err = (uint64_t)gr[REG_ERR];

  /* Faulting instruction address -- always useful */
  ffic_fmt_hex64(hbuf, rip);
  my_safe_printf_stderr(
      "\n  [RIP = 0x%s] -- Faulting instruction address.\n"
      "    To locate in source: addr2line -e mysqld -f -C -p 0x%s\n",
      hbuf, hbuf);

  /* Null or near-null first argument / 'this' pointer */
  if (rdi <= NULLPTR_BOUND) {
    ffic_fmt_hex64(hbuf, rdi);
    my_safe_printf_stderr(
        "\n  [RDI = 0x%s] -- 1st argument / 'this' is NULL or near-NULL.\n"
        "    Strongest indicator of a null-pointer dereference.\n"
        "    A method was called on a NULL 'this', or NULL was passed\n"
        "    as the first argument to the faulting function.\n",
        hbuf);
  }

  /* Stack pointer misalignment */
  if (rsp % 16 != 0) {
    ffic_fmt_hex64(hbuf, rsp);
    my_safe_printf_stderr(
        "\n  [RSP = 0x%s] -- WARNING: stack pointer is NOT 16-byte aligned.\n"
        "    Indicates stack corruption or an ABI violation\n"
        "    (e.g. mismatched calling convention or corrupted return "
        "address).\n",
        hbuf);
  }

  /* Frame pointer below stack pointer */
  if (rbp != 0 && rbp < rsp) {
    ffic_fmt_hex64(hbuf, rbp);
    my_safe_printf_stderr(
        "\n  [RBP = 0x%s] -- WARNING: frame pointer is below stack pointer.\n"
        "    The stack frame is likely corrupted; the backtrace above\n"
        "    may show wrong function names or be incomplete.\n",
        hbuf);
  }

  /* Write-access page fault */
  if ((uint64_t)gr[REG_TRAPNO] == 0x0e && (err & 0x2)) {
    my_safe_printf_stderr(
        "\n  [ERR bit1=1] -- The fault was a WRITE to an invalid address.\n"
        "    Look for an assignment or store operation in the faulting"
        " function.\n");
  }

#elif defined(__aarch64__)
  const mcontext_t *mc = &uc->uc_mcontext;
  uint64_t pc = (uint64_t)mc->pc;
  uint64_t sp = (uint64_t)mc->sp;
  uint64_t fp = (uint64_t)mc->regs[29];
  uint64_t x0 = (uint64_t)mc->regs[0];

  /* Faulting instruction address */
  ffic_fmt_hex64(hbuf, pc);
  my_safe_printf_stderr(
      "\n  [PC = 0x%s] -- Faulting instruction address.\n"
      "    To locate in source: addr2line -e mysqld -f -C -p 0x%s\n",
      hbuf, hbuf);

  /* Null or near-null x0 */
  if (x0 <= NULLPTR_BOUND) {
    ffic_fmt_hex64(hbuf, x0);
    my_safe_printf_stderr(
        "\n  [x0 = 0x%s] -- 1st argument / 'this' is NULL or near-NULL.\n"
        "    Strongest indicator of a null-pointer dereference.\n"
        "    A method was called on a NULL 'this', or NULL was passed\n"
        "    as the first argument to the faulting function.\n",
        hbuf);
  }

  /* Stack pointer misalignment */
  if (sp % 16 != 0) {
    ffic_fmt_hex64(hbuf, sp);
    my_safe_printf_stderr(
        "\n  [SP = 0x%s] -- WARNING: stack pointer is NOT 16-byte aligned.\n"
        "    AArch64 hardware enforces 16-byte alignment; misalignment means\n"
        "    stack corruption or an ABI violation.\n",
        hbuf);
  }

  /* Frame pointer below stack pointer */
  if (fp != 0 && fp < sp) {
    ffic_fmt_hex64(hbuf, fp);
    my_safe_printf_stderr(
        "\n  [x29/FP = 0x%s] -- WARNING: frame pointer is below SP.\n"
        "    Stack frame likely corrupted; backtrace may be incomplete.\n",
        hbuf);
  }
#endif /* __x86_64__ / __aarch64__ */
}

/**
 * Print a hex dump of the top-of-stack memory with an interpretation guide.
 *
 * The guide explains what the stack is and how to recognise common patterns
 * such as buffer overflows, stack canary corruption, and infinite recursion.
 * This is designed to be understandable without prior knowledge of CPU/OS
 * internals.
 *
 * @param sp        Stack pointer at crash time (start of the dump).
 * @param num_words Number of pointer-sized words to dump (default: 32).
 */
void ffic_print_stack_memory(const uint64_t *sp, size_t num_words) {
  if (!sp) return;
  ffic_section("Stack Memory Dump");

  /* Plain-language explanation of what the stack is and how to read the dump.
     Split across multiple calls to stay within the 512-byte fprintf buffer. */
  my_safe_printf_stderr(
      "  The stack holds local variables, saved return addresses, and\n"
      "  register spills for the currently-executing function chain.\n");
  my_safe_printf_stderr(
      "  Each function call pushes a return address (where to resume after\n"
      "  the function returns) and a frame pointer onto the stack.\n"
      "  The stack grows DOWNWARD, so SP points to the lowest used address.\n");
  my_safe_printf_stderr(
      "\n  How to read this dump:\n"
      "    Each row: [SP+offset]  value0  value1  value2  value3\n"
      "    Values are 8-byte words in little-endian (native) byte order.\n");
  my_safe_printf_stderr(
      "    Return addresses look like code pointers (e.g. 0x00007f...).\n"
      "    Repeating patterns (0x4141...='AAAA', 0xdead...) mean buffer\n"
      "    overflow or memory poison.\n");
  my_safe_printf_stderr(
      "    All-zero or 0xffff... near the top = uninitialised locals.\n"
      "    Stack canary (__stack_chk_guard) = random value before saved RIP;\n"
      "    if overwritten the binary should have printed"
      " 'stack smashing detected'.\n");
  my_safe_printf_stderr(
      "\n  What problems this helps detect:\n"
      "    1. Stack buffer overflow: fill pattern before saved RIP/PC.\n"
      "    2. Stack smashing: corrupted canary value.\n"
      "    3. Infinite recursion: all frames look identical.\n"
      "    4. Wrong calling convention: saved RIP/PC points to garbage.\n");

  const uint64_t *p = reinterpret_cast<const uint64_t *>(sp);
  constexpr uint32_t ADDR_PER_LINE = 4;
  constexpr uint32_t OFFSET_BUF_SIZE = 12;
  char hex[ADDR_PER_LINE][HEX_PER_WORD];
  char off_buf[OFFSET_BUF_SIZE];

  my_safe_printf_stderr("  SP = %p\n", sp);
  for (size_t i = 0; i < num_words; i += ADDR_PER_LINE) {
    size_t n = num_words - i;
    if (n > ADDR_PER_LINE) n = ADDR_PER_LINE;

    char *off_str = my_safe_utoa(10, (ulonglong)(i * sizeof(uint64_t)),
                                 &off_buf[sizeof(off_buf) - 1]);

    my_safe_printf_stderr("  [SP+%s]", off_str);
    for (size_t j = 0; j < n; ++j) {
      ffic_fmt_hex64(hex[j], (uint64_t)p[i + j]);
      my_safe_printf_stderr("  0x%s", hex[j]);
    }
    my_safe_printf_stderr("%s", "\n");
  }
}

/**
 * Print the raw instruction bytes around the faulting PC.
 *
 * Inspired by openGauss fatal_err.cpp::print_inst_ctx().
 * Memory validity is verified with dladdr() before reading.
 * The row containing the faulting PC is marked with ">>".
 *
 * @param uc  Pointer to ucontext_t, or nullptr.
 */
void ffic_print_instruction_context(const ucontext_t *uc) {
  if (!uc) return;

  uint64_t pc = 0;
#if defined(__x86_64__)
  pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
  pc = (uint64_t)uc->uc_mcontext.pc;
#endif
  if (pc == 0) return;

  ffic_section("Instruction Bytes Around Fault PC");
  my_safe_printf_stderr(
      "  Shows raw machine-code bytes near the faulting instruction.\n"
      "  Primarily useful for advanced analysis or when no debug symbols\n"
      "  are available.  To disassemble, run:\n"
      "    objdump -d mysqld | grep -A10 '<addr>'\n");

  char hbuf[HEX_PER_WORD];
  ffic_fmt_hex64(hbuf, pc);
  my_safe_printf_stderr("  Faulting PC = 0x%s\n", hbuf);

  /* Validate 32 bytes before and after PC with dladdr() before reading. */
  const unsigned char *start = reinterpret_cast<const unsigned char *>(pc) - 32;
  const unsigned char *end = reinterpret_cast<const unsigned char *>(pc) + 32;
  Dl_info dlinfo;
  if (!dladdr(reinterpret_cast<const void *>(start), &dlinfo) ||
      !dladdr(reinterpret_cast<const void *>(end), &dlinfo)) {
    my_safe_printf_stderr(
        "  (memory around PC is not readable -- possibly JIT or unmapped)\n");
    return;
  }

  /* Print 8 bytes per row; mark the row that contains the faulting PC. */
  const unsigned char *row = start;
  const unsigned char *pc_ptr = reinterpret_cast<const unsigned char *>(pc);
  while (row < end) {
    if (row <= pc_ptr && pc_ptr < row + 8)
      my_safe_printf_stderr("  >> %p:", static_cast<const void *>(row));
    else
      my_safe_printf_stderr("     %p:", static_cast<const void *>(row));
    for (int b = 0; b < 8 && row + b < end; ++b) {
      char hbyte[3];
      ffic_fmt_hex8(hbyte, row[b]);
      my_safe_printf_stderr(" %s", hbyte);
    }
    my_safe_printf_stderr("%s", "\n");
    row += 8;
  }
}
#endif /* __x86_64__ || __aarch64__ */

/**
 * Print selected lines from /proc/self/status (memory and thread stats).
 *
 * Helps identify OOM conditions (high VmRSS) and resource exhaustion
 * (high thread count).
 */
void ffic_print_process_resources() {
  ffic_section("Process Resources (/proc/self/status)");
  my_safe_printf_stderr(
      "  Shows memory and thread usage at the time of the crash.\n"
      "  VmRSS  = actual RAM used (resident set size).\n"
      "  VmPeak = peak virtual memory ever used by this process.\n");
  my_safe_printf_stderr(
      "  A very high VmRSS (close to system RAM) suggests an OOM condition.\n"
      "  A high Threads count may indicate a thread-pool or lock problem.\n");

  int fd = open("/proc/self/status", O_RDONLY);
  if (fd == -1) {
    my_safe_printf_stderr("  (cannot open /proc/self/status)\n");
    return;
  }
  char buf[MAX_CHUNK_SIZE];
  ssize_t n = read(fd, buf, (ssize_t)sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    my_safe_printf_stderr("  (cannot read /proc/self/status)\n");
    return;
  }
  buf[n] = '\0';

  /* Print only the lines we care about */
  static const char *const kFields[] = {
      "VmPeak:", "VmRSS:", "VmData:", "VmStk:", "Threads:", nullptr};

  const char *line = buf;
  while (*line) {
    const char *eol = line;
    while (*eol && *eol != '\n') ++eol;

    for (int i = 0; kFields[i]; ++i) {
      const char *f = kFields[i];
      size_t flen = 0;
      while (f[flen]) ++flen;
      bool match = true;
      for (size_t j = 0; j < flen && match; ++j) match = (line[j] == f[j]);
      if (match) {
        my_safe_printf_stderr("  ");
        my_write_stderr(line, (size_t)(eol - line));
        my_write_stderr("\n", 1);
        break;
      }
    }
    line = (*eol == '\n') ? eol + 1 : eol;
    if (*eol == '\0') break;
  }

  my_safe_printf_stderr("  (For open FD count: ls /proc/%d/fd | wc -l)\n",
                        (int)getpid());
}

/**
 * Print executable segments from /proc/self/maps.
 *
 * With ASLR, absolute addresses in the register dump change every run.
 * Knowing the segment base address allows offline symbol resolution:
 *   relative_addr = absolute_addr - segment_base
 *   addr2line -e mysqld -f -C -p <relative_addr>
 */
void ffic_print_loaded_segments() {
  ffic_section("Loaded Segments (/proc/self/maps, selected)");
  my_safe_printf_stderr(
      "  Shows base addresses of key binaries and shared libraries.\n"
      "  With ASLR these change every run.  To resolve a register address:\n");
  my_safe_printf_stderr(
      "    relative_addr = absolute_addr - segment_base\n"
      "    addr2line -e mysqld -f -C -p <relative_addr>\n"
      "  Only executable (r-xp) segments containing 'mysqld' or '.so'"
      " are shown.\n");

  int fd = open("/proc/self/maps", O_RDONLY);
  if (fd == -1) {
    my_safe_printf_stderr("  (cannot open /proc/self/maps)\n");
    return;
  }

  /* Read in 4096-byte chunks; parse line-by-line using a small line buffer. */
  char buf[MAX_CHUNK_SIZE];
  char line_buf[MAX_LINE_SIZE];
  int line_pos = 0;
  ssize_t nr;

  while ((nr = read(fd, buf, (ssize_t)sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < nr; ++i) {
      char c = buf[i];
      if (c == '\n') {
        line_buf[line_pos] = '\0';

        /* Filter: must contain "r-xp" (executable segment) */
        bool is_exec = false;
        for (int j = 0; j + 3 < line_pos; ++j) {
          if (line_buf[j] == 'r' && line_buf[j + 1] == '-' &&
              line_buf[j + 2] == 'x' && line_buf[j + 3] == 'p') {
            is_exec = true;
            break;
          }
        }
        if (is_exec) {
          /* Further filter: path must contain ".so" or "mysqld" */
          bool relevant = false;
          for (int j = 0; j + 2 < line_pos && !relevant; ++j) {
            if (line_buf[j] == '.' && line_buf[j + 1] == 's' &&
                line_buf[j + 2] == 'o')
              relevant = true;
            if (j + 6 < line_pos && line_buf[j] == 'm' &&
                line_buf[j + 1] == 'y' && line_buf[j + 2] == 's' &&
                line_buf[j + 3] == 'q' && line_buf[j + 4] == 'l' &&
                line_buf[j + 5] == 'd')
              relevant = true;
          }
          if (relevant) my_safe_printf_stderr("  %s\n", line_buf);
        }
        line_pos = 0;
      } else if (line_pos < (int)sizeof(line_buf) - 1) {
        line_buf[line_pos++] = c;
      }
    }
  }
  close(fd);
}

/**
 * Print OS kernel version and machine architecture via uname(2).
 *
 * Useful for confirming whether a crash is kernel-version-specific or
 * architecture-specific.
 */
void ffic_print_os_info() {
  ffic_section("OS Information");
  my_safe_printf_stderr(
      "  Kernel version and architecture.\n"
      "  Helps confirm whether the crash is OS-version or arch specific.\n");

  struct utsname u;
  if (uname(&u) != 0) {
    my_safe_printf_stderr("  (uname() failed)\n");
    return;
  }
  my_safe_printf_stderr("  Sysname : %s\n", u.sysname);
  my_safe_printf_stderr("  Release : %s\n", u.release);
  my_safe_printf_stderr("  Version : %s\n", u.version);
  my_safe_printf_stderr("  Machine : %s\n", u.machine);
}

/**
 * Print system-wide memory statistics from /proc/meminfo.
 *
 * Captures MemTotal, MemFree, MemAvailable, Buffers, Cached, SwapTotal,
 * and SwapFree at crash time.
 *
 * How to interpret:
 *   MemTotal    -- total physical RAM installed.
 *   MemFree     -- completely unused pages (does NOT include reclaimable
 * cache). MemAvailable-- kernel's estimate of memory available for new
 * allocations without swapping; this is the most meaningful OOM indicator.
 *   Buffers     -- page-cache for block device metadata.
 *   Cached      -- page-cache for file data (reclaimable under pressure).
 *   SwapTotal / SwapFree -- if SwapFree << SwapTotal, the system was swapping.
 *
 * OOM indicator: if MemAvailable < 5%% of MemTotal at crash time, the process
 * or the kernel OOM killer likely contributed to the crash.
 */
void ffic_print_memory_info() {
  ffic_section("System Memory (/proc/meminfo)");
  my_safe_printf_stderr(
      "  System-wide memory at crash time.\n"
      "  MemAvailable < 5%% of MemTotal -> possible OOM-related crash.\n"
      "  SwapFree << SwapTotal         -> system was actively swapping.\n");

  int fd = open("/proc/meminfo", O_RDONLY);
  if (fd == -1) {
    my_safe_printf_stderr("  (cannot open /proc/meminfo)\n");
    return;
  }
  char buf[MAX_CHUNK_SIZE];
  ssize_t n = read(fd, buf, (ssize_t)sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    my_safe_printf_stderr("  (cannot read /proc/meminfo)\n");
    return;
  }
  buf[n] = '\0';

  static const char *const kFields[] = {
      "MemTotal:", "MemFree:",   "MemAvailable:", "Buffers:",
      "Cached:",   "SwapTotal:", "SwapFree:",     nullptr};

  const char *line = buf;
  while (*line) {
    const char *eol = line;
    while (*eol && *eol != '\n') ++eol;
    for (int i = 0; kFields[i]; ++i) {
      const char *f = kFields[i];
      size_t flen = 0;
      while (f[flen]) ++flen;
      bool match = true;
      for (size_t j = 0; j < flen && match; ++j) match = (line[j] == f[j]);
      if (match) {
        my_safe_printf_stderr("  ");
        my_write_stderr(line, (size_t)(eol - line));
        my_write_stderr("\n", 1);
        break;
      }
    }
    line = (*eol == '\n') ? eol + 1 : eol;
    if (*eol == '\0') break;
  }
}

/**
 * Print system load averages from /proc/loadavg.
 *
 * Format: 1-min  5-min  15-min  running/total  last-spawned-PID
 *
 * How to interpret:
 *   The three load numbers represent the average number of runnable or
 *   uninterruptible threads over 1, 5, and 15 minutes.  Compare to the
 *   CPU count (from the OS Information section).
 *
 *   load < CPU_count   -- system was not saturated.
 *   load ~ CPU_count   -- system was fully utilized.
 *   load > CPU_count   -- CPU bottleneck; threads were waiting for the CPU.
 *
 *   High 15-min load relative to 1-min load means the pressure was
 *   sustained before the crash, not a momentary spike.
 */
void ffic_print_system_load() {
  ffic_section("System Load (/proc/loadavg)");
  my_safe_printf_stderr(
      "  Format: 1-min 5-min 15-min  running/total  last-pid\n"
      "  Compare load values to CPU count (see OS Information section).\n"
      "  load >> CPUs -> CPU saturation may have contributed to the crash.\n");

  int fd = open("/proc/loadavg", O_RDONLY);
  if (fd == -1) {
    my_safe_printf_stderr("  (cannot open /proc/loadavg)\n");
    return;
  }
  char buf[MAX_LINE_SIZE];
  ssize_t n = read(fd, buf, (ssize_t)sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    my_safe_printf_stderr("  (cannot read /proc/loadavg)\n");
    return;
  }
  buf[n] = '\0';
  /* Strip trailing newline */
  for (ssize_t i = 0; i < n; ++i)
    if (buf[i] == '\n') {
      buf[i] = '\0';
      break;
    }

  my_safe_printf_stderr("  %s\n", buf);
}

/**
 * Print key process resource limits from /proc/self/limits.
 *
 * Filters to the four limits most relevant to MySQL crashes:
 *
 *   Max stack size    -- if the soft limit is small (e.g. 8 MB default on
 *                        Linux) and the thread stack grew beyond it, the
 *                        kernel sends SIGSEGV with a stack-overflow pattern.
 *                        Check VmStk in Process Resources for current usage.
 *   Max core file size-- '0' means no core dump will be written, even if
 *                        mysqld crashes. To enable: ulimit -c unlimited
 *   Max open files    -- if too small (< 65535 for busy servers), mysqld
 *                        may fail to open sockets or files and crash.
 *   Max address space -- unlimited is normal; a cap can cause mmap() failure.
 *
 * How to read the file: columns are Soft Limit / Hard Limit / Units.
 * 'unlimited' means the OS imposes no cap at that level.
 */
void ffic_print_process_limits() {
  ffic_section("Process Limits (/proc/self/limits, selected)");
  my_safe_printf_stderr(
      "  Key resource limits for this process (soft / hard / units).\n"
      "  Max stack size   small -> possible stack overflow (see VmStk above).\n"
      "  Max core file sz 0     -> no core dump will be written on crash.\n"
      "  Max open files   small -> FD exhaustion may cause socket failures.\n");

  int fd = open("/proc/self/limits", O_RDONLY);
  if (fd == -1) {
    my_safe_printf_stderr("  (cannot open /proc/self/limits)\n");
    return;
  }
  char buf[MAX_CHUNK_SIZE];
  ssize_t n = read(fd, buf, (ssize_t)sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    my_safe_printf_stderr("  (cannot read /proc/self/limits)\n");
    return;
  }
  buf[n] = '\0';

  static const char *const kFields[] = {"Max stack size", "Max core file size",
                                        "Max open files", "Max address space",
                                        nullptr};

  const char *line = buf;
  while (*line) {
    const char *eol = line;
    while (*eol && *eol != '\n') ++eol;
    for (int i = 0; kFields[i]; ++i) {
      const char *f = kFields[i];
      size_t flen = 0;
      while (f[flen]) ++flen;
      bool match = true;
      for (size_t j = 0; j < flen && match; ++j) match = (line[j] == f[j]);
      if (match) {
        my_write_stderr("  ", 2);
        my_write_stderr(line, (size_t)(eol - line));
        my_write_stderr("\n", 1);
        break;
      }
    }
    line = (*eol == '\n') ? eol + 1 : eol;
    if (*eol == '\0') break;
  }
}

/**
 * Print the raw signal information from siginfo_t.
 * Moved from sql/signal_handler.cc to allow unit testing.
 *
 * @param sig      Signal number.
 * @param si       Pointer to siginfo_t, or nullptr (no-op).
 */
void ffic_print_signal_info(int sig, const siginfo_t *si) {
  if (!si) return;

  my_safe_printf_stderr("\n=== Signal Information ===\n");
  my_safe_printf_stderr("  si_signo = %d\n", sig);
  my_safe_printf_stderr("  si_code  = %d\n", si->si_code);

  if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE)
    my_safe_printf_stderr("  si_addr  = %p\n", si->si_addr);

  /* If the signal was explicitly sent by another process (e.g. kill -SIGSEGV),
     si_code == SI_USER (0) or SI_QUEUE (-1), and si_pid / si_uid are valid. */
  if (si->si_code == SI_USER || si->si_code == SI_QUEUE) {
    my_safe_printf_stderr(
        "  Sender   : PID %d, UID %d\n"
        "             (signal was sent by another process, not a CPU fault)\n",
        (int)si->si_pid, (int)si->si_uid);
  }
}

/**
 * Print actionable post-crash tips.
 * Moved from sql/signal_handler.cc to allow unit testing.
 */
void ffic_print_post_crash_tips() {
  my_safe_printf_stderr("\n=== Post-Crash Diagnostic Tips ===\n");
  my_safe_printf_stderr(
      "  The following commands help decode information in this report.\n\n");

  /* c++filt tip */
  my_safe_printf_stderr(
      "  1. Demangle a C++ symbol shown in the stack trace:\n"
      "       c++filt <mangled_name>\n"
      "     Example:\n"
      "       c++filt _ZN3foo3barEv\n"
      "       -> foo::bar()\n\n");

  /* addr2line tip */
  my_safe_printf_stderr(
      "  2. Map an absolute crash address to source file and line number:\n"
      "       Step 1: find segment_base in the 'Loaded Segments' section "
      "above.\n"
      "       Step 2: relative_addr = absolute_addr - segment_base\n"
      "       Step 3: addr2line -e /path/to/mysqld -f -C -p <relative_addr>\n"
      "     Example (RIP = 0x7f1234abcd, base = 0x7f1200000):\n"
      "       addr2line -e mysqld -f -C -p 0x34abcd\n\n");

  /* GDB tip */
  my_safe_printf_stderr(
      "  3. Open a core dump in GDB (requires 'Max core file size' > 0\n"
      "     in the 'Process Limits' section above):\n"
      "       gdb /path/to/mysqld core\n"
      "     Useful GDB commands:\n"
      "       bt full          -- full stack trace with local variables\n"
      "       info registers   -- all CPU register values\n"
      "       x/32xg $rsp      -- hex dump of stack top (x86_64)\n"
      "       x/32xg $sp       -- hex dump of stack top (AArch64)\n\n");

  /* Enable-at-runtime reminder */
  my_safe_printf_stderr(
      "  4. Enable verbose crash diagnostics at runtime (no restart needed):\n"
      "       SET GLOBAL rds_ffic_verbose_crash_diagnostics = ON;\n");
}

#endif /* __linux__ && !_WIN32 */
