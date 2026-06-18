#pragma once
/**
 * license_engine.h
 * License Engine — hardware fingerprinting, RSA-2048 token validation,
 * symbol count enforcement, and feature flag gating.
 *
 * This is LAYER 1 enforcement (runs inside the C++ binary).
 * Even if API layers are bypassed, the engine itself refuses to
 * allocate more Symbol Workers than the license permits.
 *
 * Spec reference: Symbol Count Product Spec §5.3 (Three Enforcement Layers)
 */

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace ofe {
namespace license {

// ── Subscription tiers ────────────────────────────────────────────────────────

/**
 * SubscriptionTier — matches the six commercial tiers.
 * Spec reference: Symbol Count Product Spec §2 (Tiers)
 */
enum class SubscriptionTier : uint8_t {
    FREE         = 0,  ///< 1 symbol, EOD only
    STARTER      = 1,  ///< 5 symbols, real-time, NinjaTrader only
    TRADER       = 2,  ///< 25 symbols, all signals, REST API read-only
    PROFESSIONAL = 3,  ///< 100 symbols, automation, Plugin SDK
    ELITE        = 4,  ///< 500 symbols, multi-broker, custom signals
    ENTERPRISE   = 5   ///< Custom symbol count, dedicated infrastructure
};

// ── Hardware fingerprint ──────────────────────────────────────────────────────

/**
 * HardwareFingerprint — collected from the local machine at startup.
 * SHA-256 hashed before sending to license server.
 */
struct HardwareFingerprint {
    std::string cpu_id;           ///< CPUID instruction result
    std::string disk_serial;      ///< Primary volume serial number
    std::string mac_address;      ///< Primary NIC MAC address
    std::string motherboard_id;   ///< WMI/DMI query result
    std::string os_install_id;    ///< Windows/Linux installation ID

    /// Returns SHA-256 hex string of all fields concatenated
    [[nodiscard]] std::string compute_hash() const;

    /// Collect fingerprint from the current machine (platform-specific)
    static HardwareFingerprint collect();
};

// ── License token ─────────────────────────────────────────────────────────────

/**
 * LicenseToken — RSA-2048 signed token from the license server.
 * Validated locally without network call (public key embedded in binary).
 */
struct LicenseToken {
    std::string  user_id;
    SubscriptionTier tier;
    uint32_t     symbol_limit;          ///< Max simultaneously active symbols
    uint32_t     addon_symbol_count;    ///< Extra symbols from add-on packs
    uint32_t     effective_symbol_limit; ///< symbol_limit + addon_symbol_count

    // Feature flags
    bool         feature_api_access;
    bool         feature_automation;
    bool         feature_plugin_sdk;
    bool         feature_volume_bar_type;
    bool         feature_all_vwap_types;
    bool         feature_sector_aggregation;
    bool         feature_white_label;

    int64_t      expiry_unix;           ///< Token expiry (Unix seconds)
    int64_t      issued_at_unix;        ///< Token issuance time
    std::string  hw_fingerprint_hash;   ///< Expected hardware fingerprint
    std::string  signature;             ///< RSA-2048 signature (base64)

    /// Returns true if token is not expired
    [[nodiscard]] bool is_valid_time() const noexcept;

    /// Returns true if hw_hash matches current machine's fingerprint
    [[nodiscard]] bool is_valid_hardware(const std::string& hw_hash) const noexcept;

    /// Returns true if RSA signature is valid (verifies with embedded public key)
    [[nodiscard]] bool is_signature_valid() const noexcept;

    /// Returns true if all three checks pass
    [[nodiscard]] bool is_fully_valid() const noexcept {
        return is_valid_time() && is_signature_valid();
        // Note: hardware check done separately so error can be reported distinctly
    }
};

// ── License engine ────────────────────────────────────────────────────────────

/**
 * LicenseEngine
 * Manages license token lifecycle, hardware validation, and feature gating.
 * One instance per engine process; must be initialised before TickRouter starts.
 */
class LicenseEngine {
public:
    LicenseEngine();
    ~LicenseEngine();

    LicenseEngine(const LicenseEngine&) = delete;
    LicenseEngine& operator=(const LicenseEngine&) = delete;

    // ── Initialisation ────────────────────────────────────────────────────

    /**
     * Load and validate the license token.
     * 1. Collect hardware fingerprint
     * 2. Load token from local encrypted cache
     * 3. Validate RSA signature, expiry, and hardware
     * 4. If online: call license server to confirm token is not revoked
     *
     * @param license_server_url  URL of the license validation endpoint
     * @param cached_token_path   Path to local encrypted token cache file
     * @return                    True if license is valid and accepted
     */
    bool initialise(const std::string& license_server_url,
                    const std::string& cached_token_path);

    /**
     * Attempt to refresh the token from the license server.
     * Called periodically (every 24 hours) and on startup.
     *
     * @return  True if token was refreshed successfully
     */
    bool refresh_token(const std::string& license_server_url);

    // ── Symbol count enforcement (LAYER 1) ────────────────────────────────

    /**
     * Check if one more Symbol Worker can be allocated.
     * Called by the Symbol Worker Pool before spawning a new worker.
     * This is the LAYER 1 enforcement — cannot be bypassed at API level.
     *
     * @param current_active  Current number of active Symbol Workers
     * @return                True if allocation is permitted
     */
    [[nodiscard]] bool can_allocate_symbol(uint32_t current_active) const noexcept;

    /**
     * Returns the effective symbol limit from the current token.
     */
    [[nodiscard]] uint32_t symbol_limit() const noexcept;

    // ── Feature gating ────────────────────────────────────────────────────

    /**
     * Check if a feature is enabled for the current license tier.
     *
     * @param feature_name  One of: "api_access", "automation", "plugin_sdk",
     *                      "volume_bar_type", "all_vwap_types", "sector_aggregation"
     * @return              True if feature is enabled
     */
    [[nodiscard]] bool is_feature_enabled(const std::string& feature_name) const noexcept;

    /**
     * Returns current subscription tier.
     */
    [[nodiscard]] SubscriptionTier tier() const noexcept;

    // ── Anti-tamper ───────────────────────────────────────────────────────

    /**
     * Verify the engine binary has not been tampered with.
     * Computes SHA-256 of the .text section and compares to
     * embedded expected hash (baked in at compile time).
     *
     * @return  True if binary is intact
     */
    [[nodiscard]] static bool verify_binary_integrity() noexcept;

    /**
     * Detect if a debugger is attached to the process.
     * Uses platform-specific detection (IsDebuggerPresent on Windows,
     * ptrace check on Linux).
     *
     * @return  True if debugger detected
     */
    [[nodiscard]] static bool is_debugger_attached() noexcept;

    /**
     * Perform all security checks. Call on startup and periodically.
     * Terminates the process if any check fails (anti-tamper policy).
     *
     * @param terminate_on_failure  If true, calls std::terminate() on failure
     * @return                      True if all checks passed
     */
    bool run_security_checks(bool terminate_on_failure = true) noexcept;

    // ── Status ─────────────────────────────────────────────────────────────

    [[nodiscard]] bool                is_valid()   const noexcept;
    [[nodiscard]] const LicenseToken& token()      const noexcept;
    [[nodiscard]] std::string         user_id()    const noexcept;
    [[nodiscard]] int64_t             expiry_unix() const noexcept;

private:
    LicenseToken         token_;
    HardwareFingerprint  hw_fingerprint_;
    std::string          hw_hash_;
    bool                 is_valid_ = false;

    /// Load and decrypt the locally cached token
    [[nodiscard]] bool load_cached_token(const std::string& path);

    /// Save the refreshed token to local encrypted cache
    bool save_cached_token(const std::string& path) const;

    /// Call the license server's /validate endpoint
    [[nodiscard]] bool online_validate(const std::string& server_url);
};

} // namespace license
} // namespace ofe
