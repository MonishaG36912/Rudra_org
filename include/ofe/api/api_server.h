#ifndef OFE_API_API_SERVER_H
#define OFE_API_API_SERVER_H

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ofe::api {

enum class SymbolQuotaTier : std::uint8_t {
  Bronze = 0,
  Silver = 1,
  Gold = 2,
  Platinum = 3,
};

struct SymbolQuotaLimits final {
  std::array<std::uint32_t, 4U> max_symbols{{32U, 128U, 512U, 2048U}};
};

class SymbolQuotaEnforcer final {
 public:
  void set_limits(SymbolQuotaLimits limits) {
    std::lock_guard<std::mutex> lock(mutex_);
    limits_ = limits;
  }

  [[nodiscard]] bool can_allocate(SymbolQuotaTier tier, std::uint32_t current_symbol_count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_symbol_count < limits_.max_symbols[static_cast<std::size_t>(tier)];
  }

 private:
  mutable std::mutex mutex_;
  SymbolQuotaLimits limits_{};
};

class IApiServer {
 public:
  virtual ~IApiServer() = default;

  virtual bool start() = 0;
  virtual void stop() = 0;
  virtual bool register_symbol(std::string_view symbol, SymbolQuotaTier tier) = 0;
  virtual void unregister_symbol(std::string_view symbol) = 0;
  virtual std::uint32_t symbol_count(SymbolQuotaTier tier) const = 0;
  virtual void set_quota_limits(SymbolQuotaLimits limits) = 0;
};

} // namespace ofe::api

#endif
