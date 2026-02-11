import sys

strings_to_hide = {
    "S_IP": "127.0.0.1",
    "S_HEARTBEAT": "HEARTBEAT",
    "S_USER_AGENT": "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36\r\n\r\n",
    "S_UPGRADE": "Upgrade: websocket",
    "S_SWITCHING": "101 Switching Protocols",
    "S_REQ_START": "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "
}

key = 0xAA

print("#ifndef HIDDEN_STRINGS_H")
print("#define HIDDEN_STRINGS_H")
print("#include <string>")
print("#include <vector>")

print("inline std::string decrypt_str(const unsigned char* data, size_t len) {")
print("    std::string s; s.resize(len);")
print(f"    for(size_t i=0; i<len; i++) s[i] = data[i] ^ {hex(key)};")
print("    return s;")
print("}")

for name, val in strings_to_hide.items():
    encrypted = [ord(c) ^ key for c in val]
    c_array = ", ".join([hex(x) for x in encrypted])
    sanitized_val = val.replace("\r", "\\r").replace("\n", "\\n")
    print(f"// Original: {sanitized_val[:50]}...")
    print(f"const unsigned char {name}_RAW[] = {{ {c_array} }};")
    print(f"#define {name} decrypt_str({name}_RAW, sizeof({name}_RAW))")

print("#endif")
