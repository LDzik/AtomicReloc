#pragma once

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <cstdint>
#include <mutex>

// Helper to get formatted Windows error messages
inline std::wstring GetLastErrorAsString(DWORD error = 0) {
    if (error == 0) {
        error = ::GetLastError();
    }
    if (error == 0) {
        return L"No error occurred.";
    }
    
    LPWSTR messageBuffer = nullptr;
    size_t size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&messageBuffer),
        0,
        NULL
    );
    
    std::wstring message;
    if (messageBuffer != nullptr && size > 0) {
        message = std::wstring(messageBuffer, size);
        LocalFree(messageBuffer);
        
        // Strip trailing whitespaces and newlines
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
            message.pop_back();
        }
    } else {
        message = L"System error code: " + std::to_wstring(error);
    }
    return message;
}

// Thread-safe Logging system for AtomicReloc
class Logger {
public:
    enum class Level {
        Info,
        Warning,
        Error,
        Success
    };

    static void Log(Level level, const std::wstring& message) {
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);

        std::wstring prefix;
        switch (level) {
            case Level::Info:    prefix = L"[INFO] "; break;
            case Level::Warning: prefix = L"[WARN] "; break;
            case Level::Error:   prefix = L"[ERR ] "; break;
            case Level::Success: prefix = L"[OK  ] "; break;
        }

        std::wstring formatted = prefix + message + L"\n";
        
        // Output to debugger console
        ::OutputDebugStringW(formatted.c_str());
        
        // Output to stdout
        std::wcout << formatted;
        std::wcout.flush();
    }
};

// --- Hyper-optimized Incremental 128-bit MurmurHash3 for x64 architecture ---
class Murmur3Hasher {
private:
    uint64_t h1 = 0;
    uint64_t h2 = 0;
    uint64_t total_len = 0;
    uint8_t buffer[16] = {0};
    size_t buffer_len = 0;
    uint32_t seed = 0;

    static inline uint64_t rotl64(uint64_t x, int8_t r) {
        return (x << r) | (x >> (64 - r));
    }

    static inline uint64_t fmix64(uint64_t k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return k;
    }

    inline void process_block(const uint8_t* block) {
        const uint64_t c1 = 0x87c37b91114253d5ULL;
        const uint64_t c2 = 0x4cf5ad432745937fULL;

        // Perform memory-safe casting using memcpy to bypass alignment issues
        uint64_t k1 = 0;
        uint64_t k2 = 0;
        std::memcpy(&k1, block, 8);
        std::memcpy(&k2, block + 8, 8);

        k1 *= c1; 
        k1 = rotl64(k1, 31); 
        k1 *= c2; 
        h1 ^= k1;

        h1 = rotl64(h1, 27); 
        h1 += h2; 
        h1 = h1 * 5 + 0x52dce729;

        k2 *= c2; 
        k2 = rotl64(k2, 33); 
        k2 *= c1; 
        h2 ^= k2;

        h2 = rotl64(h2, 31); 
        h2 += h1; 
        h2 = h2 * 5 + 0x38495ab5;
    }

public:
    struct Hash128 {
        uint64_t high;
        uint64_t low;

        bool operator==(const Hash128& other) const {
            return high == other.high && low == other.low;
        }

        bool operator!=(const Hash128& other) const {
            return !(*this == other);
        }

        std::wstring ToString() const {
            wchar_t buf[33];
            swprintf_s(buf, 33, L"%016llx%016llx", high, low);
            return std::wstring(buf);
        }
    };

    explicit Murmur3Hasher(uint32_t seed = 42) : seed(seed) {
        Reset();
    }

    void Reset() {
        h1 = seed;
        h2 = seed;
        total_len = 0;
        buffer_len = 0;
        std::memset(buffer, 0, 16);
    }

    void Update(const void* data, size_t len) {
        const uint8_t* src = static_cast<const uint8_t*>(data);
        total_len += len;

        // Consume remainder in cached buffer first
        if (buffer_len > 0) {
            size_t to_copy = (std::min)(static_cast<size_t>(16 - buffer_len), len);
            std::memcpy(buffer + buffer_len, src, to_copy);
            buffer_len += to_copy;
            src += to_copy;
            len -= to_copy;

            if (buffer_len == 16) {
                process_block(buffer);
                buffer_len = 0;
            }
        }

        // Process main body in fast 16-byte blocks
        while (len >= 16) {
            process_block(src);
            src += 16;
            len -= 16;
        }

        // Store residual bytes in cached buffer
        if (len > 0) {
            std::memcpy(buffer, src, len);
            buffer_len = len;
        }
    }

    Hash128 Finalize() {
        uint64_t fh1 = h1;
        uint64_t fh2 = h2;

        const uint64_t c1 = 0x87c37b91114253d5ULL;
        const uint64_t c2 = 0x4cf5ad432745937fULL;

        // Process remaining tail in buffer
        uint64_t k1 = 0;
        uint64_t k2 = 0;

        switch (buffer_len) {
            case 15: k2 ^= static_cast<uint64_t>(buffer[14]) << 48; [[fallthrough]];
            case 14: k2 ^= static_cast<uint64_t>(buffer[13]) << 40; [[fallthrough]];
            case 13: k2 ^= static_cast<uint64_t>(buffer[12]) << 32; [[fallthrough]];
            case 12: k2 ^= static_cast<uint64_t>(buffer[11]) << 24; [[fallthrough]];
            case 11: k2 ^= static_cast<uint64_t>(buffer[10]) << 16; [[fallthrough]];
            case 10: k2 ^= static_cast<uint64_t>(buffer[9]) << 8;   [[fallthrough]];
            case  9: k2 ^= static_cast<uint64_t>(buffer[8]) << 0;
                     k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; fh2 ^= k2;
                     [[fallthrough]];
            case  8: k1 ^= static_cast<uint64_t>(buffer[7]) << 56;  [[fallthrough]];
            case  7: k1 ^= static_cast<uint64_t>(buffer[6]) << 48;  [[fallthrough]];
            case  6: k1 ^= static_cast<uint64_t>(buffer[5]) << 40;  [[fallthrough]];
            case  5: k1 ^= static_cast<uint64_t>(buffer[4]) << 32;  [[fallthrough]];
            case  4: k1 ^= static_cast<uint64_t>(buffer[3]) << 24;  [[fallthrough]];
            case  3: k1 ^= static_cast<uint64_t>(buffer[2]) << 16;  [[fallthrough]];
            case  2: k1 ^= static_cast<uint64_t>(buffer[1]) << 8;   [[fallthrough]];
            case  1: k1 ^= static_cast<uint64_t>(buffer[0]) << 0;
                     k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; fh1 ^= k1;
        };

        // Hash finalization mixing
        fh1 ^= total_len;
        fh2 ^= total_len;

        fh1 += fh2;
        fh2 += fh1;

        fh1 = fmix64(fh1);
        fh2 = fmix64(fh2);

        fh1 += fh2;
        fh2 += fh1;

        return Hash128{ fh1, fh2 };
    }
};
