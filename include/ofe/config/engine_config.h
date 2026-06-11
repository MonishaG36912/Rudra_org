#ifndef OFE_CONFIG_ENGINE_CONFIG_H
#define OFE_CONFIG_ENGINE_CONFIG_H

#include <cstdint>
#include <filesystem>
#include <string>

#include <yaml-cpp/yaml.h>

namespace ofe::config {

struct EngineConfig final {
  std::uint32_t worker_threads{8U};
  std::uint32_t symbol_capacity{1024U};
  std::uint32_t max_symbols_per_tier{256U};
  std::uint32_t feed_threads{2U};
  std::uint32_t router_shards{64U};
  std::uint32_t ring_capacity{4096U};
  std::uint64_t bar_timeframe_ns{1'000'000'000ULL};
  std::uint32_t range_ticks{12U};
  std::uint32_t volume_threshold{1'000U};
  double imbalance_ratio{3.0};
  double vwap_band_sigma{2.0};
  std::uint32_t vwap_band_levels{2U};
  double pulse_threshold{0.7};
  double turn_threshold{0.6};
  double cot_threshold{0.5};
  double sweep_threshold{0.8};
  double delta_surge_threshold{2.5};
  std::uint32_t cvd_lookback{64U};
  std::uint32_t volume_profile_levels{64U};
  double value_area_ratio{0.70};
  double max_daily_loss{0.0};
  double max_position_size{0.0};
  std::uint32_t max_orders_per_second{100U};
  std::uint32_t quote_age_ms{250U};
  std::uint32_t tick_timeout_ms{50U};
  std::uint32_t reconnection_backoff_ms{1000U};
  std::uint32_t license_refresh_minutes{30U};
  std::uint32_t symbol_quota_bronze{32U};
  std::uint32_t symbol_quota_silver{128U};
  std::uint32_t symbol_quota_gold{512U};
  std::uint32_t symbol_quota_platinum{2048U};
  std::uint16_t api_port{8080U};
  std::uint32_t api_threads{4U};
  std::uint32_t feed_batch_size{256U};
  bool drop_late_ticks{true};
  bool enable_lee_ready{true};
  bool enable_vwap{true};
  bool enable_volume_profile{true};
  bool enable_delta{true};
  bool enable_imbalance{true};
  bool enable_signals{true};
  bool persist_ticks{false};
  bool persist_bars{false};
  bool persist_signals{false};
  std::string log_level{"info"};
  std::string machine_name{"ofe"};
  std::string venue_name{"default"};
  std::string strategy_name{"order_flow"};

  [[nodiscard]] static EngineConfig from_yaml(const YAML::Node& root) {
    EngineConfig config{};
    config.worker_threads = read_value<std::uint32_t>(root, "worker_threads", config.worker_threads);
    config.symbol_capacity = read_value<std::uint32_t>(root, "symbol_capacity", config.symbol_capacity);
    config.max_symbols_per_tier = read_value<std::uint32_t>(root, "max_symbols_per_tier", config.max_symbols_per_tier);
    config.feed_threads = read_value<std::uint32_t>(root, "feed_threads", config.feed_threads);
    config.router_shards = read_value<std::uint32_t>(root, "router_shards", config.router_shards);
    config.ring_capacity = read_value<std::uint32_t>(root, "ring_capacity", config.ring_capacity);
    config.bar_timeframe_ns = read_value<std::uint64_t>(root, "bar_timeframe_ns", config.bar_timeframe_ns);
    config.range_ticks = read_value<std::uint32_t>(root, "range_ticks", config.range_ticks);
    config.volume_threshold = read_value<std::uint32_t>(root, "volume_threshold", config.volume_threshold);
    config.imbalance_ratio = read_value<double>(root, "imbalance_ratio", config.imbalance_ratio);
    config.vwap_band_sigma = read_value<double>(root, "vwap_band_sigma", config.vwap_band_sigma);
    config.vwap_band_levels = read_value<std::uint32_t>(root, "vwap_band_levels", config.vwap_band_levels);
    config.pulse_threshold = read_value<double>(root, "pulse_threshold", config.pulse_threshold);
    config.turn_threshold = read_value<double>(root, "turn_threshold", config.turn_threshold);
    config.cot_threshold = read_value<double>(root, "cot_threshold", config.cot_threshold);
    config.sweep_threshold = read_value<double>(root, "sweep_threshold", config.sweep_threshold);
    config.delta_surge_threshold = read_value<double>(root, "delta_surge_threshold", config.delta_surge_threshold);
    config.cvd_lookback = read_value<std::uint32_t>(root, "cvd_lookback", config.cvd_lookback);
    config.volume_profile_levels = read_value<std::uint32_t>(root, "volume_profile_levels", config.volume_profile_levels);
    config.value_area_ratio = read_value<double>(root, "value_area_ratio", config.value_area_ratio);
    config.max_daily_loss = read_value<double>(root, "max_daily_loss", config.max_daily_loss);
    config.max_position_size = read_value<double>(root, "max_position_size", config.max_position_size);
    config.max_orders_per_second = read_value<std::uint32_t>(root, "max_orders_per_second", config.max_orders_per_second);
    config.quote_age_ms = read_value<std::uint32_t>(root, "quote_age_ms", config.quote_age_ms);
    config.tick_timeout_ms = read_value<std::uint32_t>(root, "tick_timeout_ms", config.tick_timeout_ms);
    config.reconnection_backoff_ms = read_value<std::uint32_t>(root, "reconnection_backoff_ms", config.reconnection_backoff_ms);
    config.license_refresh_minutes = read_value<std::uint32_t>(root, "license_refresh_minutes", config.license_refresh_minutes);
    config.symbol_quota_bronze = read_value<std::uint32_t>(root, "symbol_quota_bronze", config.symbol_quota_bronze);
    config.symbol_quota_silver = read_value<std::uint32_t>(root, "symbol_quota_silver", config.symbol_quota_silver);
    config.symbol_quota_gold = read_value<std::uint32_t>(root, "symbol_quota_gold", config.symbol_quota_gold);
    config.symbol_quota_platinum = read_value<std::uint32_t>(root, "symbol_quota_platinum", config.symbol_quota_platinum);
    config.api_port = read_value<std::uint16_t>(root, "api_port", config.api_port);
    config.api_threads = read_value<std::uint32_t>(root, "api_threads", config.api_threads);
    config.feed_batch_size = read_value<std::uint32_t>(root, "feed_batch_size", config.feed_batch_size);
    config.drop_late_ticks = read_value<bool>(root, "drop_late_ticks", config.drop_late_ticks);
    config.enable_lee_ready = read_value<bool>(root, "enable_lee_ready", config.enable_lee_ready);
    config.enable_vwap = read_value<bool>(root, "enable_vwap", config.enable_vwap);
    config.enable_volume_profile = read_value<bool>(root, "enable_volume_profile", config.enable_volume_profile);
    config.enable_delta = read_value<bool>(root, "enable_delta", config.enable_delta);
    config.enable_imbalance = read_value<bool>(root, "enable_imbalance", config.enable_imbalance);
    config.enable_signals = read_value<bool>(root, "enable_signals", config.enable_signals);
    config.persist_ticks = read_value<bool>(root, "persist_ticks", config.persist_ticks);
    config.persist_bars = read_value<bool>(root, "persist_bars", config.persist_bars);
    config.persist_signals = read_value<bool>(root, "persist_signals", config.persist_signals);
    config.log_level = read_value<std::string>(root, "log_level", config.log_level);
    config.machine_name = read_value<std::string>(root, "machine_name", config.machine_name);
    config.venue_name = read_value<std::string>(root, "venue_name", config.venue_name);
    config.strategy_name = read_value<std::string>(root, "strategy_name", config.strategy_name);
    return config;
  }

  [[nodiscard]] static EngineConfig load_from_file(const std::filesystem::path& file_path) {
    return from_yaml(YAML::LoadFile(file_path.string()));
  }

 private:
  template <typename T>
  [[nodiscard]] static T read_value(const YAML::Node& root, const char* key, const T& fallback) {
    const YAML::Node value = root[key];
    if (!value || value.IsNull()) {
      return fallback;
    }
    return value.as<T>();
  }
};

} // namespace ofe::config

#endif
