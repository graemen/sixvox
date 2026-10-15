// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host shim of stmlib/system/storage.h (ChunkStorage only). The two chunks
// are kept in vox_host::storage, which the host saves and restores (VCV Rack:
// in the patch file) instead of a flash sector.

#ifndef STMLIB_SYSTEM_STORAGE_H_
#define STMLIB_SYSTEM_STORAGE_H_

#include <string.h>

#include "stmlib/stmlib.h"

namespace vox_host {

struct Storage {
  uint8_t persistent[512];
  size_t persistent_size;   // 0 = never saved
  uint8_t state[64];
  size_t state_size;        // 0 = never saved
  uint32_t state_saves;     // incremented by every SaveState
};
extern Storage storage;

}  // namespace vox_host

namespace stmlib {

template<size_t sector_index, typename PersistentData, typename State>
class ChunkStorage {
 public:
  ChunkStorage() { }
  ~ChunkStorage() { }

  // Loads what the host restored; returns false when there was nothing
  // (as a blank flash sector does on the module).
  bool Init(PersistentData* persistent_data, State* state) {
    static_assert(sizeof(PersistentData) <= sizeof(vox_host::storage.persistent), "");
    static_assert(sizeof(State) <= sizeof(vox_host::storage.state), "");
    persistent_data_ = persistent_data;
    state_ = state;
    bool found = false;
    if (vox_host::storage.persistent_size == sizeof(PersistentData)) {
      memcpy(persistent_data, vox_host::storage.persistent, sizeof(PersistentData));
      found = true;
    }
    if (vox_host::storage.state_size == sizeof(State)) {
      memcpy(state, vox_host::storage.state, sizeof(State));
      found = true;
    }
    return found;
  }

  // The module would roll back to the previous chunk version; the host keeps
  // a single version, so this is the same as Init.
  void Recover(PersistentData* persistent_data, State* state) {
    Init(persistent_data, state);
  }

  void SavePersistentData() {
    memcpy(vox_host::storage.persistent, persistent_data_, sizeof(PersistentData));
    vox_host::storage.persistent_size = sizeof(PersistentData);
  }

  void SaveState() {
    memcpy(vox_host::storage.state, state_, sizeof(State));
    vox_host::storage.state_size = sizeof(State);
    ++vox_host::storage.state_saves;
  }

 private:
  PersistentData* persistent_data_;
  State* state_;

  DISALLOW_COPY_AND_ASSIGN(ChunkStorage);
};

}  // namespace stmlib

#endif  // STMLIB_SYSTEM_STORAGE_H_
