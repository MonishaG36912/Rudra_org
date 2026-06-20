/**
 * test/test_coinbase_jwt.cpp
 * §AGT-05b: Offline unit tests for JWT ES256 signing algorithm.
 *
 * Tests the exact JWT construction algorithm used by CoinbaseAdapter::Impl::build_jwt()
 * without requiring any network connection or Coinbase credentials.
 *
 * Strategy:
 *   1. Generate a fresh P-256 EC key pair in memory using OpenSSL EVP.
 *   2. Extract the private key as PEM — feeds the same signing path as the adapter.
 *   3. Re-implement base64url_encode + ecdsa_sign_jwt + build_jwt (duplicated here
 *      because they live inside the adapter's private Impl; production code unchanged).
 *   4. Verify JWT structure: exactly 3 dot-separated parts.
 *   5. Verify base64url header decodes to valid JSON with "alg":"ES256".
 *   6. Verify base64url payload decodes to valid JSON with "iss":"cdp" and "sub".
 *   7. Verify signature: base64url decode → 64 raw bytes → verify against public key.
 *
 * These tests are part of the standard ofe_tests suite and run in CI (no network).
 *
 * Dependencies: OpenSSL 3.x (already linked via ofe_engine → OpenSSL::Crypto).
 */

#include <gtest/gtest.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <array>
#include <cstring>
#include <ctime>
#include <chrono>
#include <string>
#include <vector>

// ── Duplicate of CoinbaseAdapter::Impl helpers (production code untouched) ────

static std::string b64url_encode(const uint8_t* data, size_t len) {
    static const char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        buf = (buf << 8) | data[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            char c = kB64[(buf >> bits) & 0x3F];
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
            out += c;
        }
    }
    if (bits > 0) {
        buf <<= (6 - bits);
        char c = kB64[buf & 0x3F];
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
        out += c;
    }
    return out;  // no '=' padding
}

static std::string b64url_encode_str(const std::string& s) {
    return b64url_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

static std::string b64url_decode(const std::string& in) {
    static const int kDec[128] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };
    std::string out;
    uint32_t buf = 0;
    int bits = 0;
    for (unsigned char c : in) {
        if (c >= 128) continue;
        int v = kDec[c];
        if (v < 0) continue;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}

// Sign `message` with EC private key PEM → base64url(R||S 64 bytes)
static std::string ecdsa_sign_test(const std::string& message,
                                   const std::string& pem_key) {
    BIO* bio = BIO_new_mem_buf(pem_key.data(), static_cast<int>(pem_key.size()));
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) return "";

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey);
    EVP_DigestSignUpdate(ctx, message.data(), message.size());

    size_t der_len = 0;
    EVP_DigestSignFinal(ctx, nullptr, &der_len);
    std::vector<uint8_t> der(der_len);
    EVP_DigestSignFinal(ctx, der.data(), &der_len);
    der.resize(der_len);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    const uint8_t* p = der.data();
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size()));
    if (!sig) return "";

    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(sig, &r, &s);

    std::array<uint8_t, 64> raw {};
    BN_bn2binpad(r, raw.data(),      32);
    BN_bn2binpad(s, raw.data() + 32, 32);
    ECDSA_SIG_free(sig);

    return b64url_encode(raw.data(), 64);
}

// Build a complete ES256 JWT with the given key name and PEM key.
// Returns empty string if either argument is empty (mirrors production build_jwt guard).
static std::string build_test_jwt(const std::string& key_name,
                                   const std::string& pem_key) {
    if (key_name.empty() || pem_key.empty()) return "";

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    const std::string header  =
        R"({"alg":"ES256","kid":")" + key_name + R"("})";
    const std::string payload =
        R"({"sub":")" + key_name + R"(","iss":"cdp","nbf":)" +
        std::to_string(now) + R"(,"exp":)" + std::to_string(now + 120) + "}";

    const std::string signing_input =
        b64url_encode_str(header) + "." + b64url_encode_str(payload);

    const std::string sig = ecdsa_sign_test(signing_input, pem_key);
    if (sig.empty()) return "";
    return signing_input + "." + sig;
}

// ── Test fixture: generate a fresh P-256 key once per test suite ──────────────

class JwtTest : public ::testing::Test {
protected:
    std::string pem_private_;
    std::string pem_public_;
    EVP_PKEY*   pkey_ = nullptr;

    void SetUp() override {
        // Generate P-256 EC key pair using OpenSSL 3.x EVP_PKEY_CTX
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        ASSERT_NE(ctx, nullptr) << "EVP_PKEY_CTX_new_from_name failed";
        ASSERT_EQ(EVP_PKEY_keygen_init(ctx), 1);
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string("group",
                const_cast<char*>("P-256"), 0),
            OSSL_PARAM_construct_end()
        };
        EVP_PKEY_CTX_set_params(ctx, params);
        ASSERT_EQ(EVP_PKEY_generate(ctx, &pkey_), 1);
        EVP_PKEY_CTX_free(ctx);

        // Write private key as PEM
        {
            BIO* bio = BIO_new(BIO_s_mem());
            PEM_write_bio_PrivateKey(bio, pkey_, nullptr, nullptr, 0, nullptr, nullptr);
            char* data = nullptr;
            long  len  = BIO_get_mem_data(bio, &data);
            pem_private_.assign(data, static_cast<size_t>(len));
            BIO_free(bio);
        }
        // Write public key as PEM
        {
            BIO* bio = BIO_new(BIO_s_mem());
            PEM_write_bio_PUBKEY(bio, pkey_);
            char* data = nullptr;
            long  len  = BIO_get_mem_data(bio, &data);
            pem_public_.assign(data, static_cast<size_t>(len));
            BIO_free(bio);
        }
    }

    void TearDown() override {
        EVP_PKEY_free(pkey_);
        pkey_ = nullptr;
    }
};

// ── JWT-01: PEM private key is non-empty and contains expected header ─────────

TEST_F(JwtTest, Jwt01_GeneratedPrivateKeyPemFormat) {
    EXPECT_FALSE(pem_private_.empty());
    // OpenSSL 3.x writes PKCS8 by default ("-----BEGIN PRIVATE KEY-----")
    // OpenSSL 1.x writes traditional ("-----BEGIN EC PRIVATE KEY-----")
    // Either is acceptable to PEM_read_bio_PrivateKey
    bool has_header = pem_private_.find("-----BEGIN PRIVATE KEY-----") != std::string::npos
                   || pem_private_.find("-----BEGIN EC PRIVATE KEY-----") != std::string::npos;
    EXPECT_TRUE(has_header)
        << "PEM private key should start with a known header.\nGot:\n" << pem_private_;
}

// ── JWT-02: base64url_encode round-trips correctly (no padding, URL-safe) ────

TEST_F(JwtTest, Jwt02_Base64UrlEncodeRoundTrip) {
    const std::string original = "Hello, JWT! {\"test\":true}";
    const std::string encoded  = b64url_encode_str(original);
    const std::string decoded  = b64url_decode(encoded);

    EXPECT_EQ(decoded, original);

    // Must not contain standard base64 padding or unsafe chars
    EXPECT_EQ(encoded.find('='), std::string::npos) << "No padding allowed";
    EXPECT_EQ(encoded.find('+'), std::string::npos) << "+ must become -";
    EXPECT_EQ(encoded.find('/'), std::string::npos) << "/ must become _";
}

// ── JWT-03: base64url_encode known vector (RFC 4648 §10 test vector) ─────────

TEST_F(JwtTest, Jwt03_Base64UrlKnownVector) {
    // RFC 4648 §10: encode("") = "", encode("f") = "Zg", encode("fo") = "Zm8"
    // With URL-safe alphabet (same chars for these vectors)
    EXPECT_EQ(b64url_encode_str(""),   "");
    EXPECT_EQ(b64url_encode_str("f"),  "Zg");
    EXPECT_EQ(b64url_encode_str("fo"), "Zm8");
    // Verify a vector that would produce + or / in standard base64:
    // 0xFB 0xFF = standard "+" → URL "−"
    const uint8_t tricky[] = {0xFB, 0xFF};
    std::string enc = b64url_encode(tricky, 2);
    EXPECT_EQ(enc.find('+'), std::string::npos);
    EXPECT_EQ(enc.find('/'), std::string::npos);
}

// ── JWT-04: build_jwt returns a 3-part dot-separated token ───────────────────

TEST_F(JwtTest, Jwt04_JwtHasThreeParts) {
    const std::string token = build_test_jwt(
        "organizations/test-org/apiKeys/test-key", pem_private_);

    ASSERT_FALSE(token.empty()) << "JWT should not be empty";

    // Count dots
    size_t dots = 0;
    for (char c : token) { if (c == '.') ++dots; }
    EXPECT_EQ(dots, 2u) << "JWT must be header.payload.signature (2 dots)";
}

// ── JWT-05: JWT header decodes to correct JSON structure ─────────────────────

TEST_F(JwtTest, Jwt05_JwtHeaderJson) {
    const std::string key_name = "organizations/abc/apiKeys/xyz";
    const std::string token    = build_test_jwt(key_name, pem_private_);
    ASSERT_FALSE(token.empty());

    const std::string header_b64 = token.substr(0, token.find('.'));
    const std::string header_json = b64url_decode(header_b64);

    // Must contain "alg":"ES256" and "kid":"<key_name>"
    EXPECT_NE(header_json.find("\"alg\":\"ES256\""), std::string::npos)
        << "Header must have alg=ES256. Got: " << header_json;
    EXPECT_NE(header_json.find("\"kid\":\"" + key_name + "\""), std::string::npos)
        << "Header must have kid=<key_name>. Got: " << header_json;
}

// ── JWT-06: JWT payload decodes to correct JSON structure ─────────────────────

TEST_F(JwtTest, Jwt06_JwtPayloadJson) {
    const std::string key_name = "organizations/abc/apiKeys/xyz";
    const std::string token    = build_test_jwt(key_name, pem_private_);
    ASSERT_FALSE(token.empty());

    const size_t dot1 = token.find('.');
    const size_t dot2 = token.find('.', dot1 + 1);
    const std::string payload_b64  = token.substr(dot1 + 1, dot2 - dot1 - 1);
    const std::string payload_json = b64url_decode(payload_b64);

    EXPECT_NE(payload_json.find("\"iss\":\"cdp\""),         std::string::npos)
        << "Payload must have iss=cdp. Got: " << payload_json;
    EXPECT_NE(payload_json.find("\"sub\":\"" + key_name + "\""), std::string::npos)
        << "Payload must have sub=<key_name>. Got: " << payload_json;
    EXPECT_NE(payload_json.find("\"nbf\":"),                 std::string::npos)
        << "Payload must have nbf (not-before). Got: " << payload_json;
    EXPECT_NE(payload_json.find("\"exp\":"),                 std::string::npos)
        << "Payload must have exp (expiry). Got: " << payload_json;
}

// ── JWT-07: JWT signature is 86 chars (64 raw bytes → base64url) ─────────────

TEST_F(JwtTest, Jwt07_SignatureLength) {
    const std::string token = build_test_jwt("k", pem_private_);
    ASSERT_FALSE(token.empty());

    const size_t dot2 = token.rfind('.');
    const std::string sig = token.substr(dot2 + 1);

    // P-256 signature = R(32) || S(32) = 64 bytes
    // 64 bytes → base64url without padding: ceil(64*4/3) rounds to 86 chars
    // (63 bytes → 84 chars + 1 remaining byte → 2 chars = 86 total)
    EXPECT_EQ(sig.size(), 86u)
        << "ES256 signature must be 86 base64url chars (64 raw bytes). Got: "
        << sig.size();
    EXPECT_EQ(sig.find('='), std::string::npos) << "No padding in JWT signatures";
    EXPECT_EQ(sig.find('+'), std::string::npos) << "No + chars in URL-safe base64";
    EXPECT_EQ(sig.find('/'), std::string::npos) << "No / chars in URL-safe base64";
}

// ── JWT-08: Signature is cryptographically valid (round-trip verify) ──────────

TEST_F(JwtTest, Jwt08_SignatureVerification) {
    const std::string key_name = "test-key";
    const std::string token    = build_test_jwt(key_name, pem_private_);
    ASSERT_FALSE(token.empty());

    // Extract signing input (header.payload) and raw signature
    const size_t dot2          = token.rfind('.');
    const std::string input    = token.substr(0, dot2);
    const std::string sig_b64  = token.substr(dot2 + 1);

    // Decode signature: base64url → 64 raw bytes
    const std::string raw_sig  = b64url_decode(sig_b64);
    ASSERT_EQ(raw_sig.size(), 64u) << "Decoded signature must be 64 bytes";

    // Reconstruct DER ECDSA_SIG from raw R||S (for OpenSSL verify API)
    ECDSA_SIG* ecdsa_sig = ECDSA_SIG_new();
    BIGNUM* r = BN_bin2bn(reinterpret_cast<const uint8_t*>(raw_sig.data()),      32, nullptr);
    BIGNUM* s = BN_bin2bn(reinterpret_cast<const uint8_t*>(raw_sig.data() + 32), 32, nullptr);
    ECDSA_SIG_set0(ecdsa_sig, r, s);  // transfers ownership of r, s

    // DER-encode back for EVP_DigestVerify (which expects DER)
    uint8_t* der_buf = nullptr;
    int der_len = i2d_ECDSA_SIG(ecdsa_sig, &der_buf);
    ECDSA_SIG_free(ecdsa_sig);
    ASSERT_GT(der_len, 0) << "DER encoding of reconstructed signature failed";

    // Verify using the public key
    BIO* bio = BIO_new_mem_buf(pem_public_.data(), static_cast<int>(pem_public_.size()));
    EVP_PKEY* pub_key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    ASSERT_NE(pub_key, nullptr) << "Failed to load public key for verification";

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pub_key);
    EVP_DigestVerifyUpdate(ctx, input.data(), input.size());
    int verify_result = EVP_DigestVerifyFinal(
        ctx, der_buf, static_cast<size_t>(der_len));

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pub_key);
    OPENSSL_free(der_buf);

    EXPECT_EQ(verify_result, 1)
        << "JWT signature verification failed — ES256 signing algorithm is broken";
}

// ── JWT-09: JWT token is different on each call (nbf/exp change with time) ───

TEST_F(JwtTest, Jwt09_TokenChangesEachCall) {
    // Two tokens built 1 second apart must differ (due to different nbf/exp)
    // We simulate this by comparing structure — same algorithm, different timestamps
    const std::string t1 = build_test_jwt("key", pem_private_);
    // Sleep 1 second to ensure time difference
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const std::string t2 = build_test_jwt("key", pem_private_);

    EXPECT_NE(t1, t2)
        << "Consecutive JWT tokens should differ (different nbf/exp timestamps)";
}

// ── JWT-10: Empty key_name or empty PEM returns empty string ─────────────────

TEST_F(JwtTest, Jwt10_EmptyCredentialsReturnEmptyToken) {
    EXPECT_TRUE(build_test_jwt("", pem_private_).empty())
        << "Empty key_name must produce empty JWT";
    EXPECT_TRUE(build_test_jwt("key-name", "").empty())
        << "Empty PEM key must produce empty JWT";
    EXPECT_TRUE(build_test_jwt("", "").empty())
        << "Both empty must produce empty JWT";
}

// ── JWT-11: Tampered signing input invalidates the signature ─────────────────

TEST_F(JwtTest, Jwt11_TamperedInputFailsVerification) {
    const std::string key_name = "test-key";
    const std::string token    = build_test_jwt(key_name, pem_private_);
    ASSERT_FALSE(token.empty());

    const size_t dot2       = token.rfind('.');
    // Tamper: flip one character in the signing input
    std::string tampered    = token;
    tampered[dot2 / 2]     ^= 0x01;  // flip one bit in header.payload
    const std::string sig_b64 = token.substr(dot2 + 1);
    const std::string tampered_input = tampered.substr(0, dot2);

    // Decode original signature
    const std::string raw_sig = b64url_decode(sig_b64);
    ASSERT_EQ(raw_sig.size(), 64u);

    ECDSA_SIG* ecdsa_sig = ECDSA_SIG_new();
    BIGNUM* r = BN_bin2bn(reinterpret_cast<const uint8_t*>(raw_sig.data()),      32, nullptr);
    BIGNUM* s = BN_bin2bn(reinterpret_cast<const uint8_t*>(raw_sig.data() + 32), 32, nullptr);
    ECDSA_SIG_set0(ecdsa_sig, r, s);

    uint8_t* der_buf = nullptr;
    int der_len = i2d_ECDSA_SIG(ecdsa_sig, &der_buf);
    ECDSA_SIG_free(ecdsa_sig);
    ASSERT_GT(der_len, 0);

    BIO* bio = BIO_new_mem_buf(pem_public_.data(), static_cast<int>(pem_public_.size()));
    EVP_PKEY* pub_key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    ASSERT_NE(pub_key, nullptr);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pub_key);
    EVP_DigestVerifyUpdate(ctx, tampered_input.data(), tampered_input.size());
    int verify_result = EVP_DigestVerifyFinal(
        ctx, der_buf, static_cast<size_t>(der_len));

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pub_key);
    OPENSSL_free(der_buf);

    EXPECT_NE(verify_result, 1)
        << "Tampered signing input must fail signature verification";
}

// Need <thread> for JWT-09
#include <thread>
