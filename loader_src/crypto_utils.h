#ifndef CRYPTO_UTILS_H
#define CRYPTO_UTILS_H

#include <stdint.h>
#include <stddef.h>

// SHA-256 Context
typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} SHA256_CTX_MINI;

void sha256_init_mini(SHA256_CTX_MINI *ctx);
void sha256_update_mini(SHA256_CTX_MINI *ctx, const uint8_t *data, size_t len);
void sha256_final_mini(SHA256_CTX_MINI *ctx, uint8_t hash[32]);

// AES-CTR Context
// Debe estar definido aquí para que loader_v2.c pueda instanciar "struct AES_ctx ctx;"
struct AES_ctx {
  uint8_t RoundKey[240]; // For AES-256
  uint8_t Iv[16];
};

void AES_init_ctx_iv(struct AES_ctx* ctx, const uint8_t* key, const uint8_t* iv);
void AES_CTR_xcrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, uint32_t length);

// Anti-Analysis Init
void init_crypto_tables(); // Generates S-Boxes at runtime (Stubbed in standard tiny-AES)

#endif
