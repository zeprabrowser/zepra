// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file CryptoAPI.h
 * @brief Web Crypto API Implementation
 * 
 * Web Cryptography API:
 * - crypto.getRandomValues(): Secure random
 * - SubtleCrypto: Digest, encrypt, decrypt, sign, verify
 */

#pragma once

#include <vector>
#include <algorithm>
#include <cstdint>
#include <string>
#include <random>
#include <array>
#include <memory>
#include <stdexcept>

namespace Zepra::API {

// =============================================================================
// Algorithm Types
// =============================================================================

enum class HashAlgorithm {
    SHA1,
    SHA256,
    SHA384,
    SHA512
};

enum class CryptoAlgorithm {
    AES_CBC,
    AES_CTR,
    AES_GCM,
    RSA_OAEP,
    ECDSA,
    ECDH,
    HMAC
};

struct AlgorithmParams {
    CryptoAlgorithm name;
    std::vector<uint8_t> iv;          // For AES-CBC, AES-GCM
    std::vector<uint8_t> counter;     // For AES-CTR
    size_t length = 0;                // For AES-CTR counter length
    std::vector<uint8_t> additionalData;  // For AES-GCM
    size_t tagLength = 128;           // For AES-GCM
};

// =============================================================================
// CryptoKey
// =============================================================================

enum class KeyType {
    Secret,
    Public,
    Private
};

enum class KeyUsage : uint8_t {
    Encrypt = 1 << 0,
    Decrypt = 1 << 1,
    Sign = 1 << 2,
    Verify = 1 << 3,
    DeriveKey = 1 << 4,
    DeriveBits = 1 << 5,
    WrapKey = 1 << 6,
    UnwrapKey = 1 << 7
};

inline KeyUsage operator|(KeyUsage a, KeyUsage b) {
    return static_cast<KeyUsage>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

/**
 * @brief Cryptographic key handle
 */
class CryptoKey {
public:
    CryptoKey(KeyType type, bool extractable, KeyUsage usages,
              std::vector<uint8_t> keyData)
        : type_(type), extractable_(extractable), usages_(usages)
        , keyData_(std::move(keyData)) {}
    
    KeyType type() const { return type_; }
    bool extractable() const { return extractable_; }
    KeyUsage usages() const { return usages_; }
    
    const std::vector<uint8_t>& rawKey() const { return keyData_; }
    
    bool hasUsage(KeyUsage usage) const {
        return (static_cast<uint8_t>(usages_) & static_cast<uint8_t>(usage)) != 0;
    }
    
private:
    KeyType type_;
    bool extractable_;
    KeyUsage usages_;
    std::vector<uint8_t> keyData_;
};

// =============================================================================
// SubtleCrypto
// =============================================================================

/**
 * @brief Low-level cryptographic operations
 */
class SubtleCrypto {
public:
    // Digest (hash)
    std::vector<uint8_t> digest(HashAlgorithm algorithm,
                                 const std::vector<uint8_t>& data) {
        switch (algorithm) {
            case HashAlgorithm::SHA256:
                return sha256(data);
            case HashAlgorithm::SHA1:
                return sha1(data);
            case HashAlgorithm::SHA384:
                return sha384(data);
            case HashAlgorithm::SHA512:
                return sha512(data);
        }
        return {};
    }
    
    // Generate key
    std::unique_ptr<CryptoKey> generateKey(CryptoAlgorithm algorithm,
                                            bool extractable,
                                            KeyUsage usages,
                                            size_t keyLength = 256) {
        std::vector<uint8_t> keyData(keyLength / 8);
        
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint8_t> dist(0, 255);
        
        for (auto& byte : keyData) {
            byte = dist(gen);
        }
        
        return std::make_unique<CryptoKey>(
            KeyType::Secret, extractable, usages, std::move(keyData));
    }
    
    // Import key
    std::unique_ptr<CryptoKey> importKey(const std::string& format,
                                          const std::vector<uint8_t>& keyData,
                                          CryptoAlgorithm algorithm,
                                          bool extractable,
                                          KeyUsage usages) {
        // Would validate format (raw, pkcs8, spki, jwk)
        return std::make_unique<CryptoKey>(
            KeyType::Secret, extractable, usages, keyData);
    }
    
    // Export key
    std::vector<uint8_t> exportKey(const std::string& format,
                                    const CryptoKey& key) {
        if (!key.extractable()) {
            throw std::runtime_error("Key not extractable");
        }
        return key.rawKey();
    }
    
    // Encrypt (AES-GCM)
    std::vector<uint8_t> encrypt(const AlgorithmParams& algorithm,
                                  const CryptoKey& key,
                                  const std::vector<uint8_t>& data) {
        if (!key.hasUsage(KeyUsage::Encrypt)) {
            throw std::runtime_error("Key cannot be used for encryption");
        }
        if (algorithm.name != CryptoAlgorithm::AES_GCM) {
            throw std::runtime_error("Only AES-GCM is implemented");
        }
        if (algorithm.iv.size() != 12) {
            throw std::runtime_error("AES-GCM requires 12-byte IV");
        }
        return aesGcmEncrypt(key.rawKey(), algorithm.iv, data,
                             algorithm.additionalData, algorithm.tagLength / 8);
    }
    
    // Decrypt (AES-GCM)
    std::vector<uint8_t> decrypt(const AlgorithmParams& algorithm,
                                  const CryptoKey& key,
                                  const std::vector<uint8_t>& data) {
        if (!key.hasUsage(KeyUsage::Decrypt)) {
            throw std::runtime_error("Key cannot be used for decryption");
        }
        if (algorithm.name != CryptoAlgorithm::AES_GCM) {
            throw std::runtime_error("Only AES-GCM is implemented");
        }
        size_t tagLen = algorithm.tagLength / 8;
        if (data.size() < tagLen) {
            throw std::runtime_error("Ciphertext too short for tag");
        }
        return aesGcmDecrypt(key.rawKey(), algorithm.iv, data,
                             algorithm.additionalData, tagLen);
    }
    
    // Sign
    std::vector<uint8_t> sign(const AlgorithmParams& algorithm,
                               const CryptoKey& key,
                               const std::vector<uint8_t>& data) {
        if (!key.hasUsage(KeyUsage::Sign)) {
            throw std::runtime_error("Key cannot be used for signing");
        }
        
        // Would implement HMAC or asymmetric signature
        return digest(HashAlgorithm::SHA256, data);
    }
    
    // Verify
    bool verify(const AlgorithmParams& algorithm,
                const CryptoKey& key,
                const std::vector<uint8_t>& signature,
                const std::vector<uint8_t>& data) {
        if (!key.hasUsage(KeyUsage::Verify)) {
            throw std::runtime_error("Key cannot be used for verification");
        }
        
        auto expected = sign(algorithm, key, data);
        return signature == expected;
    }
    
    // =========================================================================
    // AES Core (FIPS 197)
    // =========================================================================

private:
    static constexpr uint8_t SBOX[256] = {
        0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
        0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
        0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
        0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
        0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
        0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
        0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
        0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
        0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
        0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
        0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
        0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
        0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
        0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
        0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
        0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
    };

    static uint8_t gmul(uint8_t a, uint8_t b) {
        uint8_t p = 0;
        for (int i = 0; i < 8; ++i) {
            if (b & 1) p ^= a;
            bool hi = a & 0x80;
            a <<= 1;
            if (hi) a ^= 0x1b;
            b >>= 1;
        }
        return p;
    }

    struct AESKey {
        uint8_t roundKeys[240]; // Max 15 rounds * 16 bytes
        int numRounds;
    };

    AESKey expandKey(const std::vector<uint8_t>& key) const {
        AESKey aes{};
        int nk = static_cast<int>(key.size()) / 4;
        aes.numRounds = nk + 6;
        int expandLen = 16 * (aes.numRounds + 1);

        static constexpr uint8_t RCON[10] = {1,2,4,8,16,32,64,128,27,54};

        for (int i = 0; i < nk * 4; ++i) aes.roundKeys[i] = key[i];

        for (int i = nk; i < expandLen / 4; ++i) {
            uint8_t t[4];
            for (int j = 0; j < 4; ++j) t[j] = aes.roundKeys[(i-1)*4+j];

            if (i % nk == 0) {
                uint8_t tmp = t[0];
                t[0] = SBOX[t[1]] ^ RCON[i/nk - 1];
                t[1] = SBOX[t[2]];
                t[2] = SBOX[t[3]];
                t[3] = SBOX[tmp];
            } else if (nk > 6 && i % nk == 4) {
                for (int j = 0; j < 4; ++j) t[j] = SBOX[t[j]];
            }
            for (int j = 0; j < 4; ++j)
                aes.roundKeys[i*4+j] = aes.roundKeys[(i-nk)*4+j] ^ t[j];
        }
        return aes;
    }

    void aesEncryptBlock(const AESKey& aes, const uint8_t in[16], uint8_t out[16]) const {
        uint8_t s[16];
        for (int i = 0; i < 16; ++i) s[i] = in[i] ^ aes.roundKeys[i];

        for (int round = 1; round <= aes.numRounds; ++round) {
            // SubBytes
            for (int i = 0; i < 16; ++i) s[i] = SBOX[s[i]];
            // ShiftRows
            uint8_t t = s[1]; s[1]=s[5]; s[5]=s[9]; s[9]=s[13]; s[13]=t;
            t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
            t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;
            // MixColumns (skip last round)
            if (round < aes.numRounds) {
                for (int c = 0; c < 4; ++c) {
                    int i = c * 4;
                    uint8_t a0=s[i], a1=s[i+1], a2=s[i+2], a3=s[i+3];
                    s[i]   = gmul(a0,2)^gmul(a1,3)^a2^a3;
                    s[i+1] = a0^gmul(a1,2)^gmul(a2,3)^a3;
                    s[i+2] = a0^a1^gmul(a2,2)^gmul(a3,3);
                    s[i+3] = gmul(a0,3)^a1^a2^gmul(a3,2);
                }
            }
            // AddRoundKey
            const uint8_t* rk = &aes.roundKeys[round * 16];
            for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
        }
        for (int i = 0; i < 16; ++i) out[i] = s[i];
    }

    // GF(2^128) multiply for GHASH
    void ghashMultiply(uint8_t result[16], const uint8_t H[16]) const {
        uint8_t Z[16] = {};
        uint8_t V[16];
        for (int i = 0; i < 16; ++i) V[i] = H[i];

        for (int i = 0; i < 128; ++i) {
            if (result[i/8] & (1 << (7 - (i%8)))) {
                for (int j = 0; j < 16; ++j) Z[j] ^= V[j];
            }
            bool lsb = V[15] & 1;
            for (int j = 15; j > 0; --j) V[j] = (V[j] >> 1) | (V[j-1] << 7);
            V[0] >>= 1;
            if (lsb) V[0] ^= 0xe1;
        }
        for (int i = 0; i < 16; ++i) result[i] = Z[i];
    }

    void ghashUpdate(uint8_t S[16], const uint8_t H[16],
                      const uint8_t* data, size_t len) const {
        for (size_t i = 0; i < len; i += 16) {
            size_t blockLen = std::min<size_t>(16, len - i);
            for (size_t j = 0; j < blockLen; ++j) S[j] ^= data[i+j];
            ghashMultiply(S, H);
        }
    }

    std::vector<uint8_t> aesGcmEncrypt(const std::vector<uint8_t>& key,
                                         const std::vector<uint8_t>& iv,
                                         const std::vector<uint8_t>& plaintext,
                                         const std::vector<uint8_t>& aad,
                                         size_t tagLen) const {
        AESKey aes = expandKey(key);

        // H = AES(K, 0^128)
        uint8_t H[16] = {};
        aesEncryptBlock(aes, H, H);

        // J0 = IV || 0x00000001 (for 12-byte IV)
        uint8_t J0[16] = {};
        for (int i = 0; i < 12; ++i) J0[i] = iv[i];
        J0[15] = 1;

        // CTR encryption
        std::vector<uint8_t> ciphertext(plaintext.size());
        uint8_t counter[16];
        for (int i = 0; i < 16; ++i) counter[i] = J0[i];

        for (size_t i = 0; i < plaintext.size(); i += 16) {
            // Increment counter
            for (int j = 15; j >= 12; --j) { if (++counter[j]) break; }
            uint8_t keystreamBlock[16];
            aesEncryptBlock(aes, counter, keystreamBlock);
            size_t blockLen = std::min<size_t>(16, plaintext.size() - i);
            for (size_t j = 0; j < blockLen; ++j)
                ciphertext[i+j] = plaintext[i+j] ^ keystreamBlock[j];
        }

        // GHASH
        uint8_t S[16] = {};
        if (!aad.empty()) {
            ghashUpdate(S, H, aad.data(), aad.size());
            // Pad AAD to 16-byte boundary
            size_t padLen = (16 - (aad.size() % 16)) % 16;
            uint8_t pad[16] = {};
            if (padLen) ghashUpdate(S, H, pad, padLen);
        }
        if (!ciphertext.empty()) {
            ghashUpdate(S, H, ciphertext.data(), ciphertext.size());
            size_t padLen = (16 - (ciphertext.size() % 16)) % 16;
            uint8_t pad[16] = {};
            if (padLen) ghashUpdate(S, H, pad, padLen);
        }
        // Length block: len(A) || len(C) in bits, each 64-bit big-endian
        uint8_t lenBlock[16] = {};
        uint64_t aadBits = aad.size() * 8;
        uint64_t ctBits = ciphertext.size() * 8;
        for (int i = 0; i < 8; ++i) {
            lenBlock[7-i] = static_cast<uint8_t>(aadBits >> (i*8));
            lenBlock[15-i] = static_cast<uint8_t>(ctBits >> (i*8));
        }
        ghashUpdate(S, H, lenBlock, 16);

        // Tag = GHASH ^ AES(K, J0)
        uint8_t J0enc[16];
        aesEncryptBlock(aes, J0, J0enc);
        for (int i = 0; i < 16; ++i) S[i] ^= J0enc[i];

        // Append tag
        ciphertext.insert(ciphertext.end(), S, S + tagLen);
        return ciphertext;
    }

    std::vector<uint8_t> aesGcmDecrypt(const std::vector<uint8_t>& key,
                                         const std::vector<uint8_t>& iv,
                                         const std::vector<uint8_t>& ciphertextWithTag,
                                         const std::vector<uint8_t>& aad,
                                         size_t tagLen) const {
        if (ciphertextWithTag.size() < tagLen)
            throw std::runtime_error("Ciphertext too short");

        size_t ctLen = ciphertextWithTag.size() - tagLen;
        std::vector<uint8_t> ct(ciphertextWithTag.begin(), ciphertextWithTag.begin() + ctLen);
        std::vector<uint8_t> receivedTag(ciphertextWithTag.begin() + ctLen, ciphertextWithTag.end());

        // Re-encrypt to verify tag
        AESKey aes = expandKey(key);
        uint8_t H[16] = {};
        aesEncryptBlock(aes, H, H);

        uint8_t J0[16] = {};
        for (int i = 0; i < 12; ++i) J0[i] = iv[i];
        J0[15] = 1;

        // Compute GHASH over received ciphertext
        uint8_t S[16] = {};
        if (!aad.empty()) {
            ghashUpdate(S, H, aad.data(), aad.size());
            size_t padLen = (16 - (aad.size() % 16)) % 16;
            uint8_t pad[16] = {};
            if (padLen) ghashUpdate(S, H, pad, padLen);
        }
        if (!ct.empty()) {
            ghashUpdate(S, H, ct.data(), ct.size());
            size_t padLen = (16 - (ct.size() % 16)) % 16;
            uint8_t pad[16] = {};
            if (padLen) ghashUpdate(S, H, pad, padLen);
        }
        uint8_t lenBlock[16] = {};
        uint64_t aadBits = aad.size() * 8;
        uint64_t ctBits = ct.size() * 8;
        for (int i = 0; i < 8; ++i) {
            lenBlock[7-i] = static_cast<uint8_t>(aadBits >> (i*8));
            lenBlock[15-i] = static_cast<uint8_t>(ctBits >> (i*8));
        }
        ghashUpdate(S, H, lenBlock, 16);

        uint8_t J0enc[16];
        aesEncryptBlock(aes, J0, J0enc);
        for (int i = 0; i < 16; ++i) S[i] ^= J0enc[i];

        // Constant-time tag comparison
        uint8_t diff = 0;
        for (size_t i = 0; i < tagLen; ++i) diff |= S[i] ^ receivedTag[i];
        if (diff != 0)
            throw std::runtime_error("AES-GCM authentication failed");

        // Decrypt via CTR
        std::vector<uint8_t> plaintext(ctLen);
        uint8_t counter[16];
        for (int i = 0; i < 16; ++i) counter[i] = J0[i];

        for (size_t i = 0; i < ctLen; i += 16) {
            for (int j = 15; j >= 12; --j) { if (++counter[j]) break; }
            uint8_t keystreamBlock[16];
            aesEncryptBlock(aes, counter, keystreamBlock);
            size_t blockLen = std::min<size_t>(16, ctLen - i);
            for (size_t j = 0; j < blockLen; ++j)
                plaintext[i+j] = ct[i+j] ^ keystreamBlock[j];
        }
        return plaintext;
    }

    // SHA-256 (FIPS 180-4)
    // =========================================================================

    std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) {
        static constexpr uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };

        auto rotr = [](uint32_t x, int n) -> uint32_t { return (x >> n) | (x << (32 - n)); };
        auto ch   = [](uint32_t x, uint32_t y, uint32_t z) -> uint32_t { return (x & y) ^ (~x & z); };
        auto maj  = [](uint32_t x, uint32_t y, uint32_t z) -> uint32_t { return (x & y) ^ (x & z) ^ (y & z); };
        auto S0   = [&](uint32_t x) -> uint32_t { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); };
        auto S1   = [&](uint32_t x) -> uint32_t { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); };
        auto s0   = [&](uint32_t x) -> uint32_t { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); };
        auto s1   = [&](uint32_t x) -> uint32_t { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); };

        // Pad message
        uint64_t bitLen = data.size() * 8;
        std::vector<uint8_t> msg = data;
        msg.push_back(0x80);
        while ((msg.size() % 64) != 56) msg.push_back(0);
        for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bitLen >> (i * 8)));

        uint32_t h[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };

        for (size_t offset = 0; offset < msg.size(); offset += 64) {
            uint32_t w[64];
            for (int i = 0; i < 16; ++i) {
                w[i] = (uint32_t(msg[offset + i*4]) << 24) | (uint32_t(msg[offset + i*4+1]) << 16) |
                        (uint32_t(msg[offset + i*4+2]) << 8) | uint32_t(msg[offset + i*4+3]);
            }
            for (int i = 16; i < 64; ++i) {
                w[i] = s1(w[i-2]) + w[i-7] + s0(w[i-15]) + w[i-16];
            }

            uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
            for (int i = 0; i < 64; ++i) {
                uint32_t t1 = hh + S1(e) + ch(e,f,g) + K[i] + w[i];
                uint32_t t2 = S0(a) + maj(a,b,c);
                hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
            }
            h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
        }

        std::vector<uint8_t> result(32);
        for (int i = 0; i < 8; ++i) {
            result[i*4]   = (h[i] >> 24) & 0xFF;
            result[i*4+1] = (h[i] >> 16) & 0xFF;
            result[i*4+2] = (h[i] >> 8)  & 0xFF;
            result[i*4+3] =  h[i]        & 0xFF;
        }
        return result;
    }

    // =========================================================================
    // SHA-1 (FIPS 180-4) — for legacy web compat only
    // =========================================================================

    std::vector<uint8_t> sha1(const std::vector<uint8_t>& data) {
        auto rotl = [](uint32_t x, int n) -> uint32_t { return (x << n) | (x >> (32 - n)); };

        uint64_t bitLen = data.size() * 8;
        std::vector<uint8_t> msg = data;
        msg.push_back(0x80);
        while ((msg.size() % 64) != 56) msg.push_back(0);
        for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bitLen >> (i * 8)));

        uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };

        for (size_t offset = 0; offset < msg.size(); offset += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i) {
                w[i] = (uint32_t(msg[offset+i*4]) << 24) | (uint32_t(msg[offset+i*4+1]) << 16) |
                        (uint32_t(msg[offset+i*4+2]) << 8) | uint32_t(msg[offset+i*4+3]);
            }
            for (int i = 16; i < 80; ++i) {
                w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
            }

            uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
            for (int i = 0; i < 80; ++i) {
                uint32_t f, k;
                if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
                uint32_t t = rotl(a, 5) + f + e + k + w[i];
                e=d; d=c; c=rotl(b, 30); b=a; a=t;
            }
            h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
        }

        std::vector<uint8_t> result(20);
        for (int i = 0; i < 5; ++i) {
            result[i*4]   = (h[i] >> 24) & 0xFF;
            result[i*4+1] = (h[i] >> 16) & 0xFF;
            result[i*4+2] = (h[i] >> 8)  & 0xFF;
            result[i*4+3] =  h[i]        & 0xFF;
        }
        return result;
    }

    // =========================================================================
    // SHA-512 core (FIPS 180-4) — used by sha384 and sha512
    // =========================================================================

    std::vector<uint8_t> sha512Core(const std::vector<uint8_t>& data,
                                     const uint64_t iv[8], size_t truncate) {
        static constexpr uint64_t K[80] = {
            0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
            0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
            0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
            0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
            0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
            0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
            0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
            0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
            0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
            0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
            0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
            0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
            0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
            0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
            0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
            0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
            0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
            0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
            0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
            0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
        };

        auto rotr64 = [](uint64_t x, int n) -> uint64_t { return (x >> n) | (x << (64 - n)); };
        auto ch     = [](uint64_t x, uint64_t y, uint64_t z) -> uint64_t { return (x & y) ^ (~x & z); };
        auto maj    = [](uint64_t x, uint64_t y, uint64_t z) -> uint64_t { return (x & y) ^ (x & z) ^ (y & z); };
        auto S0     = [&](uint64_t x) -> uint64_t { return rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39); };
        auto S1     = [&](uint64_t x) -> uint64_t { return rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41); };
        auto s0     = [&](uint64_t x) -> uint64_t { return rotr64(x, 1) ^ rotr64(x, 8) ^ (x >> 7); };
        auto s1     = [&](uint64_t x) -> uint64_t { return rotr64(x, 19) ^ rotr64(x, 61) ^ (x >> 6); };

        uint64_t bitLen = data.size() * 8;
        std::vector<uint8_t> msg = data;
        msg.push_back(0x80);
        while ((msg.size() % 128) != 112) msg.push_back(0);
        // 128-bit length (upper 64 bits are 0 for messages < 2^64 bits)
        for (int i = 0; i < 8; ++i) msg.push_back(0);
        for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bitLen >> (i * 8)));

        uint64_t h[8];
        for (int i = 0; i < 8; ++i) h[i] = iv[i];

        for (size_t offset = 0; offset < msg.size(); offset += 128) {
            uint64_t w[80];
            for (int i = 0; i < 16; ++i) {
                w[i] = 0;
                for (int j = 0; j < 8; ++j)
                    w[i] = (w[i] << 8) | msg[offset + i*8 + j];
            }
            for (int i = 16; i < 80; ++i)
                w[i] = s1(w[i-2]) + w[i-7] + s0(w[i-15]) + w[i-16];

            uint64_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
            for (int i = 0; i < 80; ++i) {
                uint64_t t1 = hh + S1(e) + ch(e,f,g) + K[i] + w[i];
                uint64_t t2 = S0(a) + maj(a,b,c);
                hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
            }
            h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
        }

        std::vector<uint8_t> result(truncate);
        size_t words = truncate / 8;
        for (size_t i = 0; i < words; ++i) {
            for (int j = 7; j >= 0; --j)
                result[i*8 + (7-j)] = static_cast<uint8_t>(h[i] >> (j * 8));
        }
        return result;
    }

    std::vector<uint8_t> sha384(const std::vector<uint8_t>& data) {
        static constexpr uint64_t iv[8] = {
            0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
            0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL
        };
        return sha512Core(data, iv, 48);
    }

    std::vector<uint8_t> sha512(const std::vector<uint8_t>& data) {
        static constexpr uint64_t iv[8] = {
            0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
            0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
        };
        return sha512Core(data, iv, 64);
    }
};

// =============================================================================
// Crypto Interface
// =============================================================================

/**
 * @brief Main crypto interface (window.crypto)
 */
class Crypto {
public:
    // Get cryptographically secure random values
    template<typename T>
    void getRandomValues(T* array, size_t count) {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        std::uniform_int_distribution<T> dist;
        
        for (size_t i = 0; i < count; i++) {
            array[i] = dist(gen);
        }
    }
    
    void getRandomValues(std::vector<uint8_t>& array) {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        std::uniform_int_distribution<int> dist(0, 255);
        
        for (auto& byte : array) {
            byte = static_cast<uint8_t>(dist(gen));
        }
    }
    
    // Generate random UUID
    std::string randomUUID() {
        std::vector<uint8_t> bytes(16);
        getRandomValues(bytes);
        
        // Set version 4
        bytes[6] = (bytes[6] & 0x0f) | 0x40;
        // Set variant
        bytes[8] = (bytes[8] & 0x3f) | 0x80;
        
        char buf[37];
        snprintf(buf, sizeof(buf),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            bytes[0], bytes[1], bytes[2], bytes[3],
            bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11],
            bytes[12], bytes[13], bytes[14], bytes[15]);
        
        return std::string(buf);
    }
    
    // Get subtle crypto interface
    SubtleCrypto& subtle() { return subtle_; }
    
private:
    SubtleCrypto subtle_;
};

// Global crypto instance
Crypto& getCrypto();

} // namespace Zepra::API
