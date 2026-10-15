// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Loads a VOX core (vox_core_api.h) for one emulated module.
//
// The firmware keeps its state in globals, as on the module, so every
// emulated module needs its own copy of those globals. The loader copies the
// core file to a private temporary file and loads that copy: the operating
// system then maps a separate instance of the library, with its own data.
// Header-only, C++11, POSIX (Linux, macOS) and Windows.

#ifndef VOX_HOST_CORE_LOADER_H_
#define VOX_HOST_CORE_LOADER_H_

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "vox/host/vox_core_api.h"

namespace vox_host {

class Core {
 public:
  Core() : handle_(NULL) { Clear(); }
  ~Core() { Unload(); }

  // Loads a private copy of the core at `path`. Returns false (and sets
  // error()) when the file cannot be copied or loaded, or is not a VOX core
  // of this API version.
  bool Load(const std::string& path, const std::string& temp_dir) {
    Unload();
    source_ = path;
    copy_ = temp_dir + "/" + UniqueName(path);
    if (!CopyFile(path, copy_)) {
      error_ = "cannot copy " + path + " to " + copy_;
      return false;
    }
#if defined(_WIN32)
    handle_ = (void*)LoadLibraryA(copy_.c_str());
    if (!handle_) error_ = "cannot load " + copy_;
#else
    handle_ = dlopen(copy_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) error_ = dlerror();
#endif
    if (!handle_) {
      std::remove(copy_.c_str());
      return false;
    }
    bool ok = true;
    ok = Bind(&api_version, "vox_core_api_version") && ok;
    ok = Bind(&firmware_name, "vox_core_firmware_name") && ok;
    ok = Bind(&num_algorithms, "vox_core_num_algorithms") && ok;
    ok = Bind(&algorithm_name, "vox_core_algorithm_name") && ok;
    ok = Bind(&set_storage, "vox_core_set_storage") && ok;
    ok = Bind(&get_persistent, "vox_core_get_persistent") && ok;
    ok = Bind(&get_state, "vox_core_get_state") && ok;
    ok = Bind(&set_controls, "vox_core_set_controls") && ok;
    ok = Bind(&power_on, "vox_core_power_on") && ok;
    ok = Bind(&process, "vox_core_process") && ok;
    ok = Bind(&get_status, "vox_core_get_status") && ok;
    if (!ok) {
      error_ = path + " is not a VOX core";
      Unload();
      return false;
    }
    if (api_version() != VOX_CORE_API_VERSION) {
      error_ = path + " was built for another version of the VOX core API";
      Unload();
      return false;
    }
    return true;
  }

  void Unload() {
    if (handle_) {
#if defined(_WIN32)
      FreeLibrary((HMODULE)handle_);
#else
      dlclose(handle_);
#endif
      handle_ = NULL;
      std::remove(copy_.c_str());
    }
    Clear();
  }

  inline bool loaded() const { return handle_ != NULL; }
  inline const std::string& error() const { return error_; }
  inline const std::string& source() const { return source_; }

  // The core's functions (valid while loaded()).
  int (*api_version)(void);
  const char* (*firmware_name)(void);
  int (*num_algorithms)(void);
  const char* (*algorithm_name)(int);
  void (*set_storage)(const uint8_t*, size_t, const uint8_t*, size_t);
  size_t (*get_persistent)(uint8_t*, size_t);
  size_t (*get_state)(uint8_t*, size_t);
  void (*set_controls)(const VoxCoreControls*);
  void (*power_on)(void);
  void (*process)(const float*, const float*, const uint8_t*, float*, float*);
  void (*get_status)(VoxCoreStatus*);

 private:
  void Clear() {
    api_version = NULL;
    firmware_name = NULL;
    num_algorithms = NULL;
    algorithm_name = NULL;
    set_storage = NULL;
    get_persistent = NULL;
    get_state = NULL;
    set_controls = NULL;
    power_on = NULL;
    process = NULL;
    get_status = NULL;
  }

  template<typename F>
  bool Bind(F* f, const char* name) {
#if defined(_WIN32)
    *f = reinterpret_cast<F>(GetProcAddress((HMODULE)handle_, name));
#else
    *f = reinterpret_cast<F>(dlsym(handle_, name));
#endif
    return *f != NULL;
  }

  static std::string UniqueName(const std::string& path) {
    static std::atomic<unsigned> counter(0);
    size_t slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : base.substr(dot);
    unsigned long pid;
#if defined(_WIN32)
    pid = (unsigned long)GetCurrentProcessId();
#else
    pid = (unsigned long)getpid();
#endif
    const long long t = (long long)std::chrono::steady_clock::now()
        .time_since_epoch().count();
    char suffix[96];
    snprintf(suffix, sizeof(suffix), "-%lu-%u-%llx", pid, counter++,
             (unsigned long long)t);
    return stem + suffix + ext;
  }

  static bool CopyFile(const std::string& from, const std::string& to) {
    std::ifstream in(from.c_str(), std::ios::binary);
    if (!in) return false;
    std::ofstream out(to.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return bool(out);
  }

  void* handle_;
  std::string source_;
  std::string copy_;
  std::string error_;

  Core(const Core&);
  Core& operator=(const Core&);
};

}  // namespace vox_host

#endif  // VOX_HOST_CORE_LOADER_H_
