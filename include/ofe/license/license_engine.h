#ifndef OFE_LICENSE_LICENSE_ENGINE_H
#define OFE_LICENSE_LICENSE_ENGINE_H

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif

namespace ofe::license {

struct HardwareFingerprint final {
  std::string cpu_signature;
  std::string mac_address;
  std::string disk_signature;
  std::string machine_id;

  [[nodiscard]] std::string canonical() const {
    return cpu_signature + '|' + mac_address + '|' + disk_signature + '|' + machine_id;
  }
};

class ILicenseVerifier {
 public:
  virtual ~ILicenseVerifier() = default;

  virtual bool verify_rsa2048_token(std::string_view token,
                                    std::string_view public_key_pem,
                                    const HardwareFingerprint& fingerprint) const = 0;
};

class LicenseEngine final {
 public:
  explicit LicenseEngine(std::shared_ptr<ILicenseVerifier> verifier = {})
      : verifier_(std::move(verifier)) {}

  [[nodiscard]] HardwareFingerprint collect_fingerprint() const {
    HardwareFingerprint fingerprint{};
    fingerprint.cpu_signature = cpu_signature();
    fingerprint.mac_address = mac_address();
    fingerprint.disk_signature = disk_signature();
    fingerprint.machine_id = machine_id();
    return fingerprint;
  }

  [[nodiscard]] bool verify_token(std::string_view token, std::string_view public_key_pem) const {
    if (!verifier_) {
      return false;
    }
    return verifier_->verify_rsa2048_token(token, public_key_pem, collect_fingerprint());
  }

 private:
  [[nodiscard]] static std::string cpu_signature() {
    std::ostringstream stream;
#if defined(__GNUC__) || defined(__clang__)
    unsigned int eax = 0U;
    unsigned int ebx = 0U;
    unsigned int ecx = 0U;
    unsigned int edx = 0U;
    if (__get_cpuid(0U, &eax, &ebx, &ecx, &edx) != 0) {
      stream << eax << ':' << ebx << ':' << ecx << ':' << edx;
    }
#else
    stream << "cpuid-unavailable";
#endif
    return stream.str();
  }

  [[nodiscard]] static std::string mac_address() {
    const std::filesystem::path net_dir{"/sys/class/net"};
    if (!std::filesystem::exists(net_dir)) {
      return {};
    }

    for (const auto& entry : std::filesystem::directory_iterator(net_dir)) {
      const auto address_path = entry.path() / "address";
      std::ifstream file(address_path);
      std::string value;
      if (file && std::getline(file, value) && !value.empty()) {
        return value;
      }
    }
    return {};
  }

  [[nodiscard]] static std::string disk_signature() {
    const std::filesystem::path block_dir{"/sys/block"};
    if (!std::filesystem::exists(block_dir)) {
      return {};
    }

    for (const auto& entry : std::filesystem::directory_iterator(block_dir)) {
      const auto device_dir = entry.path() / "device";
      const auto serial_path = device_dir / "serial";
      std::ifstream serial_file(serial_path);
      std::string serial;
      if (serial_file && std::getline(serial_file, serial) && !serial.empty()) {
        return serial;
      }

      const auto wwid_path = device_dir / "wwid";
      std::ifstream wwid_file(wwid_path);
      std::string wwid;
      if (wwid_file && std::getline(wwid_file, wwid) && !wwid.empty()) {
        return wwid;
      }
    }
    return {};
  }

  [[nodiscard]] static std::string machine_id() {
    const char* candidates[] = {"/etc/machine-id", "/var/lib/dbus/machine-id"};
    for (const char* candidate : candidates) {
      std::ifstream file(candidate);
      std::string value;
      if (file && std::getline(file, value) && !value.empty()) {
        return value;
      }
    }
    return {};
  }

  std::shared_ptr<ILicenseVerifier> verifier_{};
};

} // namespace ofe::license

#endif
