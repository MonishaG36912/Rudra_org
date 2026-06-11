#ifndef OFE_FEED_ADAPTER_INTERFACE_H
#define OFE_FEED_ADAPTER_INTERFACE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ofe/core/tick_record.h>

namespace ofe::feed {

struct FeedConnectionConfig final {
  std::string endpoint;
  std::string username;
  std::string password;
  std::uint16_t port{0U};
  std::uint32_t heartbeat_interval_ms{1000U};
  bool reconnect{true};
};

struct KinetickConfig final : FeedConnectionConfig {
  std::string product;
  std::string account;
};

struct eSignalConfig final : FeedConnectionConfig {
  std::string stream_name;
  std::string session_token;
};

struct IQFeedConfig final : FeedConnectionConfig {
  std::string login;
  std::string password_token;
};

class IDataFeedAdapter {
 public:
  virtual ~IDataFeedAdapter() = default;

  virtual bool connect() = 0;
  virtual void disconnect() = 0;
  virtual bool subscribe(std::string_view symbol) = 0;
  virtual bool unsubscribe(std::string_view symbol) = 0;
  virtual bool next_tick(ofe::core::UniversalTickRecord& out_tick) = 0;
  virtual std::string name() const = 0;
};

} // namespace ofe::feed

#endif
