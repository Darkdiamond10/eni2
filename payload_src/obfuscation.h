#ifndef OBFUSCATION_H
#define OBFUSCATION_H

#include <string>
#include <array>

// Simple XOR String Obfuscation (Compile-Time)
// Key is 0x55 (simple fixed key for POC readability, normally random per string)

template <size_t N>
struct XorString {
    std::array<char, N> encrypted;

    constexpr XorString(const char (&str)[N]) : encrypted{} {
        for (size_t i = 0; i < N; ++i) {
            encrypted[i] = str[i] ^ 0x55;
        }
    }

    std::string decrypt() const {
        std::string s;
        s.reserve(N);
        for (size_t i = 0; i < N; ++i) {
            s.push_back(encrypted[i] ^ 0x55);
        }
        // Remove null terminator from string size if present
        if (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
};

// Macro para uso fácil
#define OBF(str) (XorString<sizeof(str)>(str).decrypt())

#endif
