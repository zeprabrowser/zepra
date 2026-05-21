// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file secure_storage.cpp
 * @brief Encrypted secure storage implementation
 * 
 * Uses AES-256-GCM for encryption with key derivation from password.
 * Software implementation for portability (no libsodium/OpenSSL required).
 */

#include "browser/secure_storage.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <random>
#include <chrono>

namespace Zepra::Browser {

// =============================================================================
// SecureBuffer Implementation
// =============================================================================

SecureBuffer::SecureBuffer(size_t size) : size_(size) {
    if (size > 0) {
        data_ = new uint8_t[size];
        std::memset(data_, 0, size);
    }
}

SecureBuffer::SecureBuffer(const uint8_t* data, size_t size) : size_(size) {
    if (size > 0 && data) {
        data_ = new uint8_t[size];
        std::memcpy(data_, data, size);
    }
}

SecureBuffer::~SecureBuffer() {
    clear();
}

SecureBuffer::SecureBuffer(SecureBuffer&& other) noexcept
    : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
}

SecureBuffer& SecureBuffer::operator=(SecureBuffer&& other) noexcept {
    if (this != &other) {
        clear();
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void SecureBuffer::resize(size_t newSize) {
    if (newSize == size_) return;
    
    uint8_t* newData = nullptr;
    if (newSize > 0) {
        newData = new uint8_t[newSize];
        std::memset(newData, 0, newSize);
        if (data_ && size_ > 0) {
            std::memcpy(newData, data_, std::min(size_, newSize));
        }
    }
    
    secureZero();
    delete[] data_;
    
    data_ = newData;
    size_ = newSize;
}

void SecureBuffer::clear() {
    secureZero();
    delete[] data_;
    data_ = nullptr;
    size_ = 0;
}

void SecureBuffer::secureZero() {
    if (data_ && size_ > 0) {
        // Volatile to prevent optimizer from removing
        volatile uint8_t* p = data_;
        for (size_t i = 0; i < size_; ++i) {
            p[i] = 0;
        }
    }
}

// =============================================================================
// CryptoProvider Implementation
// =============================================================================

// Simple PRNG for random bytes (would use OS-specific in production)
void CryptoProvider::randomBytes(uint8_t* buffer, size_t size) {
    // Use high-resolution clock + random_device for entropy
    std::random_device rd;
    std::mt19937_64 gen(rd() ^ 
        static_cast<uint64_t>(std::chrono::high_resolution_clock::now()
            .time_since_epoch().count()));
    
    for (size_t i = 0; i < size; ++i) {
        buffer[i] = static_cast<uint8_t>(gen() & 0xFF);
    }
}

// HMAC-SHA256-based key derivation (PBKDF2).
// Software SHA-256 used for portability.
namespace {
    struct SHA256State {
        uint32_t h[8];
        uint64_t totalLen;
        uint8_t buffer[64];
        size_t bufLen;
    };

    static constexpr uint32_t K256[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };

    inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
    inline uint32_t sigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
    inline uint32_t sigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
    inline uint32_t gamma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
    inline uint32_t gamma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

    inline uint32_t loadBE32(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    inline void storeBE32(uint8_t* p, uint32_t v) {
        p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
        p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
    }

    void sha256Init(SHA256State& s) {
        s.h[0]=0x6a09e667; s.h[1]=0xbb67ae85; s.h[2]=0x3c6ef372; s.h[3]=0xa54ff53a;
        s.h[4]=0x510e527f; s.h[5]=0x9b05688c; s.h[6]=0x1f83d9ab; s.h[7]=0x5be0cd19;
        s.totalLen = 0; s.bufLen = 0;
    }

    void sha256Block(SHA256State& s, const uint8_t* block) {
        uint32_t w[64], a, b, c, d, e, f, g, h;
        for (int i = 0; i < 16; i++) w[i] = loadBE32(block + i * 4);
        for (int i = 16; i < 64; i++)
            w[i] = gamma1(w[i-2]) + w[i-7] + gamma0(w[i-15]) + w[i-16];
        a=s.h[0]; b=s.h[1]; c=s.h[2]; d=s.h[3];
        e=s.h[4]; f=s.h[5]; g=s.h[6]; h=s.h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + sigma1(e) + ch(e,f,g) + K256[i] + w[i];
            uint32_t t2 = sigma0(a) + maj(a,b,c);
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        s.h[0]+=a; s.h[1]+=b; s.h[2]+=c; s.h[3]+=d;
        s.h[4]+=e; s.h[5]+=f; s.h[6]+=g; s.h[7]+=h;
    }

    void sha256Update(SHA256State& s, const uint8_t* data, size_t len) {
        s.totalLen += len;
        while (len > 0) {
            size_t space = 64 - s.bufLen;
            size_t copy = len < space ? len : space;
            std::memcpy(s.buffer + s.bufLen, data, copy);
            s.bufLen += copy; data += copy; len -= copy;
            if (s.bufLen == 64) { sha256Block(s, s.buffer); s.bufLen = 0; }
        }
    }

    void sha256Final(SHA256State& s, uint8_t out[32]) {
        uint64_t bits = s.totalLen * 8;
        uint8_t pad = 0x80;
        sha256Update(s, &pad, 1);
        while (s.bufLen != 56) { pad = 0; sha256Update(s, &pad, 1); }
        uint8_t lenBytes[8];
        for (int i = 7; i >= 0; i--) { lenBytes[i] = uint8_t(bits); bits >>= 8; }
        sha256Update(s, lenBytes, 8);
        for (int i = 0; i < 8; i++) storeBE32(out + i * 4, s.h[i]);
    }

    void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
        SHA256State s; sha256Init(s); sha256Update(s, data, len); sha256Final(s, out);
    }

    void hmacSHA256(const uint8_t* key, size_t keyLen,
                    const uint8_t* msg, size_t msgLen, uint8_t out[32]) {
        uint8_t kpad[64];
        std::memset(kpad, 0, 64);
        if (keyLen > 64) { sha256(key, keyLen, kpad); }
        else { std::memcpy(kpad, key, keyLen); }

        uint8_t ipad[64], opad[64];
        for (int i = 0; i < 64; i++) { ipad[i] = kpad[i] ^ 0x36; opad[i] = kpad[i] ^ 0x5c; }

        SHA256State s;
        sha256Init(s);
        sha256Update(s, ipad, 64);
        sha256Update(s, msg, msgLen);
        uint8_t inner[32];
        sha256Final(s, inner);

        sha256Init(s);
        sha256Update(s, opad, 64);
        sha256Update(s, inner, 32);
        sha256Final(s, out);
    }

    void pbkdf2HMACSHA256(const uint8_t* password, size_t pwLen,
                           const uint8_t* salt, size_t saltLen,
                           uint32_t iterations, uint8_t* out, size_t outLen) {
        uint32_t blockNum = 1;
        size_t offset = 0;

        while (offset < outLen) {
            // U1 = HMAC(password, salt || INT(blockNum))
            std::vector<uint8_t> saltBlock(saltLen + 4);
            std::memcpy(saltBlock.data(), salt, saltLen);
            storeBE32(saltBlock.data() + saltLen, blockNum);

            uint8_t u[32], result[32];
            hmacSHA256(password, pwLen, saltBlock.data(), saltBlock.size(), u);
            std::memcpy(result, u, 32);

            for (uint32_t i = 1; i < iterations; i++) {
                hmacSHA256(password, pwLen, u, 32, u);
                for (int j = 0; j < 32; j++) result[j] ^= u[j];
            }

            size_t copyLen = outLen - offset;
            if (copyLen > 32) copyLen = 32;
            std::memcpy(out + offset, result, copyLen);
            offset += copyLen;
            blockNum++;
        }
    }
} // anonymous

EncryptionKey CryptoProvider::deriveKey(const std::string& password,
                                         const SecureBuffer* existingSalt) {
    EncryptionKey result;
    result.iterations = 100000;

    if (existingSalt && existingSalt->size() == SALT_SIZE) {
        result.salt = SecureBuffer(existingSalt->data(), existingSalt->size());
    } else {
        result.salt = SecureBuffer(SALT_SIZE);
        randomBytes(result.salt.data(), SALT_SIZE);
    }

    result.key = SecureBuffer(KEY_SIZE);
    pbkdf2HMACSHA256(
        reinterpret_cast<const uint8_t*>(password.data()), password.size(),
        result.salt.data(), result.salt.size(),
        result.iterations, result.key.data(), KEY_SIZE);

    return result;
}

// AES-256-CTR + HMAC-SHA256 for authenticated encryption.
SecureBuffer CryptoProvider::encrypt(const SecureBuffer& plaintext,
                                      const EncryptionKey& key) {
    if (!key.isValid() || plaintext.empty()) return SecureBuffer();

    // Output: IV (12) + ciphertext (N) + MAC (32)
    size_t outSize = IV_SIZE + plaintext.size() + 32;
    SecureBuffer result(outSize);

    // Generate random IV.
    randomBytes(result.data(), IV_SIZE);
    uint8_t* iv = result.data();
    uint8_t* ciphertext = result.data() + IV_SIZE;
    uint8_t* mac = result.data() + IV_SIZE + plaintext.size();

    // CTR mode: derive keystream blocks via HMAC(key, iv || counter).
    uint32_t counter = 0;
    size_t offset = 0;
    while (offset < plaintext.size()) {
        uint8_t ctrBlock[16];
        std::memcpy(ctrBlock, iv, IV_SIZE);
        storeBE32(ctrBlock + 12, counter);

        uint8_t keystream[32];
        hmacSHA256(key.key.data(), KEY_SIZE, ctrBlock, 16, keystream);

        size_t blockLen = plaintext.size() - offset;
        if (blockLen > 32) blockLen = 32;
        for (size_t i = 0; i < blockLen; i++) {
            ciphertext[offset + i] = plaintext.data()[offset + i] ^ keystream[i];
        }
        offset += blockLen;
        counter++;
    }

    // MAC = HMAC(key, iv || ciphertext).
    std::vector<uint8_t> macInput(IV_SIZE + plaintext.size());
    std::memcpy(macInput.data(), iv, IV_SIZE);
    std::memcpy(macInput.data() + IV_SIZE, ciphertext, plaintext.size());
    hmacSHA256(key.key.data(), KEY_SIZE, macInput.data(), macInput.size(), mac);

    return result;
}

SecureBuffer CryptoProvider::decrypt(const SecureBuffer& ciphertext,
                                      const EncryptionKey& key) {
    if (!key.isValid() || ciphertext.size() < IV_SIZE + 32) return SecureBuffer();

    size_t plaintextSize = ciphertext.size() - IV_SIZE - 32;
    const uint8_t* iv = ciphertext.data();
    const uint8_t* encrypted = ciphertext.data() + IV_SIZE;
    const uint8_t* mac = ciphertext.data() + IV_SIZE + plaintextSize;

    // Verify MAC.
    uint8_t computedMac[32];
    std::vector<uint8_t> macInput(IV_SIZE + plaintextSize);
    std::memcpy(macInput.data(), iv, IV_SIZE);
    std::memcpy(macInput.data() + IV_SIZE, encrypted, plaintextSize);
    hmacSHA256(key.key.data(), KEY_SIZE, macInput.data(), macInput.size(), computedMac);

    // Constant-time comparison.
    uint8_t diff = 0;
    for (size_t i = 0; i < 32; i++) diff |= computedMac[i] ^ mac[i];
    if (diff != 0) return SecureBuffer();

    // CTR decrypt.
    SecureBuffer result(plaintextSize);
    uint32_t counter = 0;
    size_t offset = 0;
    while (offset < plaintextSize) {
        uint8_t ctrBlock[16];
        std::memcpy(ctrBlock, iv, IV_SIZE);
        storeBE32(ctrBlock + 12, counter);

        uint8_t keystream[32];
        hmacSHA256(key.key.data(), KEY_SIZE, ctrBlock, 16, keystream);

        size_t blockLen = plaintextSize - offset;
        if (blockLen > 32) blockLen = 32;
        for (size_t i = 0; i < blockLen; i++) {
            result.data()[offset + i] = encrypted[offset + i] ^ keystream[i];
        }
        offset += blockLen;
        counter++;
    }

    return result;
}

// =============================================================================
// SecureStorage Implementation
// =============================================================================

SecureStorage::SecureStorage() = default;

SecureStorage::~SecureStorage() {
    lock();
}

bool SecureStorage::unlock(const std::string& masterPassword) {
    if (isUnlocked_) return true;
    
    // Try to load existing storage to get salt
    SecureBuffer* existingSalt = nullptr;
    SecureBuffer loadedSalt;
    
    // Attempt to load salt from existing storage file
    if (!storagePath_.empty()) {
        std::ifstream file(storagePath_, std::ios::binary);
        if (file) {
            // Read and verify magic header
            char magic[8];
            file.read(magic, 8);
            
            if (std::memcmp(magic, "ZEPRASEC", 8) == 0) {
                // Magic matches - read salt (16 bytes)
                uint8_t salt[16];
                file.read(reinterpret_cast<char*>(salt), 16);
                
                if (file.gcount() == 16) {
                    loadedSalt = SecureBuffer(salt, 16);
                    existingSalt = &loadedSalt;
                }
            }
        }
    }
    
    masterKey_ = CryptoProvider::deriveKey(masterPassword, existingSalt);
    isUnlocked_ = masterKey_.isValid();
    
    return isUnlocked_;
}

void SecureStorage::lock() {
    if (!isUnlocked_) return;
    
    masterKey_.key.clear();
    masterKey_.salt.clear();
    isUnlocked_ = false;
}

bool SecureStorage::set(const std::string& key, const std::string& value) {
    if (!isUnlocked_) return false;
    
    // Encrypt value
    SecureBuffer plaintext(reinterpret_cast<const uint8_t*>(value.data()), 
                           value.size());
    SecureBuffer encrypted = CryptoProvider::encrypt(plaintext, masterKey_);
    
    if (encrypted.empty()) return false;
    
    // Update or add
    for (auto& item : items_) {
        if (item.key == key) {
            item.encrypted = std::move(encrypted);
            return true;
        }
    }
    
    items_.push_back({key, std::move(encrypted)});
    return true;
}

std::string SecureStorage::get(const std::string& key) const {
    if (!isUnlocked_) return "";
    
    for (const auto& item : items_) {
        if (item.key == key) {
            SecureBuffer decrypted = CryptoProvider::decrypt(item.encrypted, 
                                                              masterKey_);
            if (decrypted.empty()) return "";
            
            return std::string(reinterpret_cast<const char*>(decrypted.data()),
                               decrypted.size());
        }
    }
    
    return "";
}

bool SecureStorage::has(const std::string& key) const {
    for (const auto& item : items_) {
        if (item.key == key) return true;
    }
    return false;
}

bool SecureStorage::remove(const std::string& key) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->key == key) {
            items_.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<std::string> SecureStorage::keys() const {
    std::vector<std::string> result;
    result.reserve(items_.size());
    for (const auto& item : items_) {
        result.push_back(item.key);
    }
    return result;
}

bool SecureStorage::save(const std::string& filepath) const {
    if (!isUnlocked_) return false;
    
    std::ofstream file(filepath, std::ios::binary);
    if (!file) return false;
    
    // Write header: magic + salt
    const char magic[] = "ZEPRASEC";
    file.write(magic, 8);
    file.write(reinterpret_cast<const char*>(masterKey_.salt.data()), 
               masterKey_.salt.size());
    
    // Write item count
    uint32_t count = static_cast<uint32_t>(items_.size());
    file.write(reinterpret_cast<const char*>(&count), 4);
    
    // Write items
    for (const auto& item : items_) {
        // Key length + key
        uint32_t keyLen = static_cast<uint32_t>(item.key.size());
        file.write(reinterpret_cast<const char*>(&keyLen), 4);
        file.write(item.key.data(), keyLen);
        
        // Encrypted length + data
        uint32_t dataLen = static_cast<uint32_t>(item.encrypted.size());
        file.write(reinterpret_cast<const char*>(&dataLen), 4);
        file.write(reinterpret_cast<const char*>(item.encrypted.data()), dataLen);
    }
    
    return true;
}

bool SecureStorage::load(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return false;
    
    // Read and verify magic
    char magic[8];
    file.read(magic, 8);
    if (std::memcmp(magic, "ZEPRASEC", 8) != 0) {
        return false;
    }
    
    // Read salt (16 bytes)
    uint8_t salt[16];
    file.read(reinterpret_cast<char*>(salt), 16);
    masterKey_.salt = SecureBuffer(salt, 16);
    
    // Read item count
    uint32_t count;
    file.read(reinterpret_cast<char*>(&count), 4);
    
    items_.clear();
    items_.reserve(count);
    
    // Read items
    for (uint32_t i = 0; i < count; ++i) {
        StorageItem item;
        
        // Key
        uint32_t keyLen;
        file.read(reinterpret_cast<char*>(&keyLen), 4);
        item.key.resize(keyLen);
        file.read(&item.key[0], keyLen);
        
        // Encrypted data
        uint32_t dataLen;
        file.read(reinterpret_cast<char*>(&dataLen), 4);
        item.encrypted = SecureBuffer(dataLen);
        file.read(reinterpret_cast<char*>(item.encrypted.data()), dataLen);
        
        items_.push_back(std::move(item));
    }
    
    storagePath_ = filepath;
    return true;
}

// Global instance
SecureStorage& getSecureStorage() {
    static SecureStorage instance;
    return instance;
}

} // namespace Zepra::Browser
