/*
 * Copyright 2025 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * Small win32 application that is used to launcher emscripten via python3.dll.
 * On non-windows platforms this is done via the run_pyton.sh shell script.
 *
 * The binary will look for a python script that matches its own name and run
 * that using python3.dll.
 */

// Define _WIN32_WINNT to Windows 7 for max portability
#define _WIN32_WINNT 0x0601

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <windows.h>

#include <minwindef.h>
#include <wchar.h>

typedef int (*Py_MainFunction)(int argc, wchar_t** argv);

#define malloc malloc_local
#define realloc realloc_local
#define wcslen wcslen_local
#define wcsdup wcsdup_local
#define memcpy memcpy_local
#define free free_local

static void* malloc(size_t _Size) {
  return HeapAlloc(GetProcessHeap(), 0, _Size);
}

static void* realloc(void* _Block, size_t _Size) {
  if (_Block == NULL)
    return malloc(_Size);
  return HeapReAlloc(GetProcessHeap(), 0, _Block, _Size);
}

static void free(void* _Block) {
  HeapFree(GetProcessHeap(), 0, _Block);
}

static size_t wcslen(const wchar_t* String) {
  size_t len = 0;
  while (String[len] > 0)
    len += 1;
  return len;
}

static void* memcpy(void* _Dst, void const* _Src, size_t _Size) {
  uint8_t* dst = (uint8_t*)_Dst;
  uint8_t* src = (uint8_t*)_Src;
  for (size_t i = 0; i < _Size; ++i) {
    dst[i] = src[i];
  }
  return dst;
}

static wchar_t* wcsdup(const wchar_t* String) {
  size_t len = wcslen(String);
  size_t alloca_size = sizeof(wchar_t) * (len + 1);
  wchar_t* new_string = malloc(alloca_size);
  memcpy(new_string, String, alloca_size);
  return new_string;
}

typedef DWORD (*windows_get_buffer_api_callback)(const void* context,
                                                 wchar_t* buffer,
                                                 DWORD buffer_size);

static wchar_t* call_windows_get_buffer_api(windows_get_buffer_api_callback api,
                                            const void* context) {
  wchar_t* buffer = NULL;
  DWORD size = api(context, NULL, 0);
  if (size > 0) {
    // The size fetched, directly use it
    DWORD buffer_size = size;
    buffer = malloc(buffer_size * sizeof(wchar_t));
    size = api(context, buffer, buffer_size);
    if (size + 1 != buffer_size) {
      free(buffer);
      buffer = NULL;
    }
  } else {
    DWORD err;
    // The size can not be fetched when err is ERROR_INSUFFICIENT_BUFFER, then
    // start with a basic bufer size, dynamically double buffer size when
    // needed.
    DWORD buffer_size = 64;
    for (; (err = GetLastError()) == ERROR_INSUFFICIENT_BUFFER;) {
      buffer_size *= 2;
      buffer = realloc(buffer, buffer_size * sizeof(wchar_t));
      if (buffer == NULL) {
        break;
      }
      size = api(context, buffer, buffer_size);
    }
    if (buffer != NULL) {
      if (err == ERROR_SUCCESS) {
        buffer[size] = 0;
        if ((size + 1) != buffer_size) {
          buffer = realloc(buffer, (size + 1) * sizeof(wchar_t));
        }
      } else {
        free(buffer);
        buffer = NULL;
      }
    }
  }
  return buffer;
}

DWORD GetModuleFileNameW_callback(const void* context,
                                  wchar_t* buffer,
                                  DWORD buffer_size) {
  HMODULE hmodule = (HMODULE)context;
  return GetModuleFileNameW(hmodule, buffer, buffer_size);
}

static inline wchar_t* get_module_file_name(HMODULE module) {
  return call_windows_get_buffer_api(GetModuleFileNameW_callback, module);
}

DWORD GetEnvironmentVariableW_callback(const void* context,
                                       wchar_t* buffer,
                                       DWORD buffer_size) {
  const wchar_t* name = (const wchar_t*)context;
  return GetEnvironmentVariableW(name, buffer, buffer_size);
}

static inline wchar_t* get_environment_variable(wchar_t* name) {
  return call_windows_get_buffer_api(GetEnvironmentVariableW_callback, name);
}

DWORD GetFullPathNameW_callback(const void* context,
                                wchar_t* buffer,
                                DWORD buffer_size) {
  const wchar_t* path = (const wchar_t*)context;
  return GetFullPathNameW(path, buffer_size, buffer, NULL);
}

static inline wchar_t* get_full_path_name(const wchar_t* path) {
  return call_windows_get_buffer_api(GetFullPathNameW_callback, path);
}

static void parse_command_line(wchar_t* cmdstart,
                               wchar_t** argv,
                               wchar_t* args,
                               size_t* argument_count,
                               size_t* character_count) {
  *character_count = 0;
  *argument_count = 1;  // We'll have at least the program name

  wchar_t c;
  int copy_character; /* 1 = copy char to *args */
  unsigned numslash;  /* num of backslashes seen */

  /* first scan the program name, copy it, and count the bytes */
  wchar_t* p = cmdstart;
  if (argv)
    *argv++ = args;

  // A quoted program name is handled here. The handling is much
  // simpler than for other arguments. Basically, whatever lies
  // between the leading double-quote and next one, or a terminal null
  // character is simply accepted. Fancier handling is not required
  // because the program name must be a legal NTFS/HPFS file name.
  // Note that the double-quote characters are not copied, nor do they
  // contribute to character_count.
  bool in_quotes = false;
  do {
    if (*p == '"') {
      in_quotes = !in_quotes;
      c = *p++;
      continue;
    }

    ++*character_count;
    if (args)
      *args++ = *p;

    c = *p++;
  } while (c != '\0' && (in_quotes || (c != ' ' && c != '\t')));

  if (c == '\0') {
    p--;
  } else {
    if (args)
      *(args - 1) = '\0';
  }

  in_quotes = false;

  // Loop on each argument
  for (;;) {
    if (*p) {
      while (*p == ' ' || *p == '\t')
        ++p;
    }

    if (*p == '\0')
      break;  // End of arguments

    // Scan an argument:
    if (argv)
      *argv++ = args;

    ++*argument_count;

    // Loop through scanning one argument:
    for (;;) {
      copy_character = 1;

      // Rules:
      // 2N     backslashes   + " ==> N backslashes and begin/end quote
      // 2N + 1 backslashes   + " ==> N backslashes + literal "
      // N      backslashes       ==> N backslashes
      numslash = 0;

      while (*p == '\\') {
        // Count number of backslashes for use below
        ++p;
        ++numslash;
      }

      if (*p == '"') {
        // if 2N backslashes before, start/end quote, otherwise
        // copy literally:
        if (numslash % 2 == 0) {
          if (in_quotes && p[1] == '"') {
            p++;  // Double quote inside quoted string
          } else {
            // Skip first quote char and copy second:
            copy_character = 0;  // Don't copy quote
            in_quotes = !in_quotes;
          }
        }

        numslash /= 2;
      }

      // Copy slashes:
      while (numslash--) {
        if (args)
          *args++ = '\\';
        ++*character_count;
      }

      // If at end of arg, break loop:
      if (*p == '\0' || (!in_quotes && (*p == ' ' || *p == '\t')))
        break;

      // Copy character into argument:
      if (copy_character) {
        if (args)
          *args++ = *p;

        ++*character_count;
      }

      ++p;
    }

    // Null-terminate the argument:
    if (args)
      *args++ = '\0';  // Terminate the string

    ++*character_count;
  }

  // We put one last argument in -- a null pointer:
  if (argv)
    *argv++ = NULL;

  ++*argument_count;
}

wchar_t* get_python_dll() {
  wchar_t* python_dll_w = get_environment_variable(L"EMSDK_PYTHON_DLL");
  if (!python_dll_w) {
    return wcsdup(L"python3.dll");
  }
  return python_dll_w;
}

void wmainCRTStartup() {
  // -E will not ignore _PYTHON_SYSCONFIGDATA_NAME an internal
  // of cpython used in cross compilation via setup.py.
  SetEnvironmentVariableW(L"_PYTHON_SYSCONFIGDATA_NAME", L"");

  // Work around python bug 34780 by closing stdin, so that it is not
  // inherited by the python subprocess.
  wchar_t* workaround_env =
      get_environment_variable(L"EM_WORKAROUND_PYTHON_BUG_34780");
  if (workaround_env) {
    CloseHandle(GetStdHandle(STD_INPUT_HANDLE));
  }

  wchar_t* launcher_path_w = get_module_file_name(NULL);
  size_t launcher_path_w_length = wcslen(launcher_path_w);
  // Change the .exe extension to .py
  memcpy(launcher_path_w + launcher_path_w_length - 3, L"py", 6);
  wchar_t* long_script_path = get_full_path_name(launcher_path_w);
  free(launcher_path_w);

  wchar_t* command_line = GetCommandLineW();
  size_t argument_count = 0;
  size_t character_count = 0;
  parse_command_line(command_line, NULL, NULL, &argument_count,
                     &character_count);
  size_t const argument_array_prepend_size = 2 * sizeof(wchar_t*);
  size_t const argument_array_size = argument_count * sizeof(wchar_t*);
  size_t const character_array_size = character_count * sizeof(wchar_t);
  size_t const total_size =
      argument_array_prepend_size + argument_array_size + character_array_size;
  uint8_t* argv_buffer = malloc(total_size);
  wchar_t** const first_argument =
      (wchar_t**)(argv_buffer + argument_array_prepend_size);
  wchar_t* const first_string =
      (wchar_t*)(argv_buffer + argument_array_prepend_size +
                 argument_count * sizeof(wchar_t*));
  parse_command_line(command_line, first_argument, first_string,
                     &argument_count, &character_count);

  // Build the final command line by appending the original arguments
  int argc = (int)(2 + argument_count - 1);
  wchar_t** argv = (wchar_t**)(argv_buffer);
  argv[0] = argv[2];
  argv[1] = L"-E";
  argv[2] = long_script_path;

  wchar_t* python_application_dll = get_python_dll();
  HMODULE python_dll_hmodule = LoadLibraryW(python_application_dll);
  Py_MainFunction Py_Main =
      (Py_MainFunction)GetProcAddress(python_dll_hmodule, "Py_Main");
  free(python_application_dll);

  int ret = Py_Main(argc, argv);
  FreeLibrary(python_dll_hmodule);

  free(long_script_path);
  free(argv);
  ExitProcess(ret);
}
