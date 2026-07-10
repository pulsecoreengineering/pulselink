#ifndef PULSELINK_GATEWAY_PL_NVS_REGISTRY_STORAGE_H
#define PULSELINK_GATEWAY_PL_NVS_REGISTRY_STORAGE_H

// NVS-backed RegistryStorage (ESP32-only, via Arduino's Preferences
// library). NOT compiled in this repo's CI — no ESP32 toolchain in the
// host-native test environment (see gateway/README.md). The RAM backend
// (core/pl_registry.h's RamRegistryStorage) is what test/test_registry.cpp
// exercises; this is the same RegistryStorage interface, just persisted.
//
// Raw struct persistence here is NOT the raw-packed-struct-cast-on-the-wire
// anti-pattern CLAUDE.md/D-005 forbid — that rule is about serializing
// between two different devices (padding/endianness can differ across
// compilers and architectures). This is one device's own compiler and ABI
// writing RegistryEntry to flash and reading the identical layout back —
// no cross-device boundary, no portability hazard.

#include <cstring>

#include <Preferences.h>

#include "pl_registry.h"

namespace pulselink {
namespace gateway {

class NvsRegistryStorage : public pulselink::RegistryStorage {
 public:
  // `prefs` must already have begin() called on it (a namespace, e.g.
  // "pulselink") before this is used — owned by the caller so gateway.ino
  // controls the Preferences lifecycle alongside everything else in setup().
  explicit NvsRegistryStorage(Preferences* prefs) : prefs_(prefs) {}

  void save(const pulselink::RegistryEntry entries[], int count) override {
    prefs_->putUInt("count", static_cast<uint32_t>(count));
    if (count > 0) {
      prefs_->putBytes("entries", entries,
                        sizeof(pulselink::RegistryEntry) *
                            static_cast<size_t>(count));
    }
  }

  int load(pulselink::RegistryEntry entries[], int max_count) override {
    uint32_t count = prefs_->getUInt("count", 0);
    if (count == 0) return 0;

    int n = static_cast<int>(count) < max_count ? static_cast<int>(count)
                                                  : max_count;
    size_t expected = sizeof(pulselink::RegistryEntry) * static_cast<size_t>(n);
    size_t got = prefs_->getBytes("entries", entries, expected);
    if (got != expected) return 0;  // corrupt/short record: start fresh
    return n;
  }

 private:
  Preferences* prefs_;
};

}  // namespace gateway
}  // namespace pulselink

#endif  // PULSELINK_GATEWAY_PL_NVS_REGISTRY_STORAGE_H
