#ifndef PULSELINK_CORE_PL_REGISTRY_H
#define PULSELINK_CORE_PL_REGISTRY_H

#include <cstdint>
#include <cstring>

#include "pl_config.h"
#include "pl_mac.h"

// MAC <-> device_id <-> {sleep_profile, last_seen} registry (TRD.md §4.3).
// field_map and pending_cmds join this struct in later phases, once the
// gateway's topic mapping (Phase 4) and command table (Phase 3) exist to
// populate them — no point carrying fields nothing writes yet.
//
// Storage is pluggable: RegistryStorage is the seam. RamRegistryStorage
// below is the host-test backend; an NVS-backed implementation is a
// gateway-side, ESP32-only concern deferred to Phase 4 hardware bring-up.

namespace pulselink {

enum class SleepProfile : unsigned char {
  kAlwaysOn = 0,
  kWakeAndPoll,
};

struct RegistryEntry {
  bool valid;
  uint8_t mac[6];
  uint8_t device_id;
  SleepProfile sleep_profile;
  uint32_t last_seen_ticks;
};

class RegistryStorage {
 public:
  virtual ~RegistryStorage() = default;
  virtual void save(const RegistryEntry entries[], int count) = 0;
  // Fills `entries` (capacity `max_count`) and returns how many were
  // loaded — 0 for a fresh/empty backend.
  virtual int load(RegistryEntry entries[], int max_count) = 0;
};

class RamRegistryStorage : public RegistryStorage {
 public:
  RamRegistryStorage() : count_(0) {}

  void save(const RegistryEntry entries[], int count) override {
    count_ = count > PULSELINK_MAX_NODES ? PULSELINK_MAX_NODES : count;
    for (int i = 0; i < count_; ++i) saved_[i] = entries[i];
  }

  int load(RegistryEntry entries[], int max_count) override {
    int n = count_ < max_count ? count_ : max_count;
    for (int i = 0; i < n; ++i) entries[i] = saved_[i];
    return n;
  }

 private:
  RegistryEntry saved_[PULSELINK_MAX_NODES];
  int count_;
};

class Registry {
 public:
  // `storage` is optional — pass nullptr for a registry that never
  // persists (fine for a scratch/throwaway test registry).
  explicit Registry(RegistryStorage* storage = nullptr) : storage_(storage) {
    for (int i = 0; i < PULSELINK_MAX_NODES; ++i) entries_[i].valid = false;
    if (!storage_) return;
    RegistryEntry loaded[PULSELINK_MAX_NODES];
    int n = storage_->load(loaded, PULSELINK_MAX_NODES);
    for (int i = 0; i < n; ++i) {
      entries_[i] = loaded[i];
      entries_[i].valid = true;
    }
  }

  RegistryEntry* find_by_mac(const uint8_t mac[6]) {
    for (int i = 0; i < PULSELINK_MAX_NODES; ++i) {
      if (entries_[i].valid && mac_equal(entries_[i].mac, mac)) {
        return &entries_[i];
      }
    }
    return nullptr;
  }

  // Registers a node or refreshes an already-known one's profile/last-seen.
  // Returns the node's device_id, or -1 if the registry is full and this
  // MAC isn't already in it.
  int add_or_update(const uint8_t mac[6], SleepProfile profile,
                     uint32_t now_ticks) {
    RegistryEntry* existing = find_by_mac(mac);
    if (existing) {
      existing->sleep_profile = profile;
      existing->last_seen_ticks = now_ticks;
      persist();
      return existing->device_id;
    }

    int slot = -1;
    for (int i = 0; i < PULSELINK_MAX_NODES; ++i) {
      if (!entries_[i].valid) {
        slot = i;
        break;
      }
    }
    if (slot < 0) return -1;

    entries_[slot].valid = true;
    memcpy(entries_[slot].mac, mac, 6);
    entries_[slot].device_id = static_cast<uint8_t>(slot);
    entries_[slot].sleep_profile = profile;
    entries_[slot].last_seen_ticks = now_ticks;
    persist();
    return entries_[slot].device_id;
  }

  void touch(const uint8_t mac[6], uint32_t now_ticks) {
    RegistryEntry* e = find_by_mac(mac);
    if (!e) return;
    e->last_seen_ticks = now_ticks;
    persist();
  }

  int size() const {
    int n = 0;
    for (int i = 0; i < PULSELINK_MAX_NODES; ++i) {
      if (entries_[i].valid) ++n;
    }
    return n;
  }

 private:
  void persist() {
    if (!storage_) return;
    RegistryEntry snapshot[PULSELINK_MAX_NODES];
    int n = 0;
    for (int i = 0; i < PULSELINK_MAX_NODES; ++i) {
      if (entries_[i].valid) snapshot[n++] = entries_[i];
    }
    storage_->save(snapshot, n);
  }

  RegistryStorage* storage_;
  RegistryEntry entries_[PULSELINK_MAX_NODES];
};

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_REGISTRY_H
