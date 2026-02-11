import asyncio
import ssl
import base64
import hashlib
import struct
import logging
from datetime import datetime

# Configuración
HOST = '0.0.0.0'
PORT = 8443
CERT_FILE = 'server.crt'
KEY_FILE = 'server.key'
MAGIC_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

logging.basicConfig(level=logging.INFO, format='[%(asctime)s] [C2] %(message)s')

def create_handshake_response(key):
    accept_key = base64.b64encode(hashlib.sha1((key + MAGIC_GUID).encode()).digest()).decode()
    return (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept_key}\r\n"
        "\r\n"
    ).encode()

def decode_frame(data):
    """Decodifica un frame websocket enmascarado del cliente."""
    if len(data) < 2:
        return None, 0

    byte1, byte2 = data[0], data[1]
    fin = byte1 & 0x80
    opcode = byte1 & 0x0F
    masked = byte2 & 0x80
    payload_len = byte2 & 0x7F

    head_len = 2
    if payload_len == 126:
        payload_len = struct.unpack(">H", data[2:4])[0]
        head_len = 4
    elif payload_len == 127:
        payload_len = struct.unpack(">Q", data[2:10])[0]
        head_len = 10

    if not masked:
        logging.warning("Cliente envió frame sin máscara (Protocol Violation)")
        return None, 0

    mask_key = data[head_len:head_len+4]
    encrypted_payload = data[head_len+4:head_len+4+payload_len]

    decoded = bytearray()
    for i in range(len(encrypted_payload)):
        decoded.append(encrypted_payload[i] ^ mask_key[i % 4])

    return decoded, head_len + 4 + payload_len

def create_frame(message):
    """Crea un frame websocket de texto sin máscara (servidor a cliente)."""
    data = message.encode()
    header = bytearray()
    header.append(0x81) # FIN + Text Opcode

    if len(data) <= 125:
        header.append(len(data))
    elif len(data) <= 65535:
        header.append(126)
        header.extend(struct.pack(">H", len(data)))
    else:
        header.append(127)
        header.extend(struct.pack(">Q", len(data)))

    return header + data

async def handle_client(reader, writer):
    addr = writer.get_extra_info('peername')
    logging.info(f"Conexión entrante de {addr}")

    # Leer handshake HTTP
    try:
        data = await reader.read(4096)
        request = data.decode('utf-8', errors='ignore')

        # Validar headers básicos de mimetismo
        headers = {}
        for line in request.split('\r\n')[1:]:
            if ': ' in line:
                key, value = line.split(': ', 1)
                headers[key] = value

        if 'Sec-WebSocket-Key' not in headers:
            logging.error("No es una petición WebSocket válida")
            writer.close()
            return

        logging.info(f"Handshake recibido. User-Agent: {headers.get('User-Agent', 'Unknown')}")

        # Enviar respuesta handshake
        response = create_handshake_response(headers['Sec-WebSocket-Key'])
        writer.write(response)
        await writer.drain()
        logging.info("Handshake completado. Canal WSS abierto.")

        # Bucle de comunicación
        buffer = bytearray()
        while True:
            chunk = await reader.read(4096)
            if not chunk:
                break
            buffer.extend(chunk)

            while True:
                msg_bytes, consumed = decode_frame(buffer)
                if consumed == 0:
                    break # Esperar más datos

                del buffer[:consumed]

                if msg_bytes:
                    message = msg_bytes.decode('utf-8', errors='ignore')
                    logging.info(f"RX: {message}")

                    if "HEARTBEAT" in message:
                        resp = create_frame("ACK_ALIVE")
                        writer.write(resp)
                        await writer.drain()
                        logging.info("TX: ACK_ALIVE")

    except Exception as e:
        logging.error(f"Error en conexión: {e}")
    finally:
        logging.info("Cerrando conexión")
        writer.close()
        try:
            await writer.wait_closed()
        except:
            pass

async def main():
    ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ssl_context.load_cert_chain(CERT_FILE, KEY_FILE)

    server = await asyncio.start_server(
        handle_client, HOST, PORT, ssl=ssl_context
    )

    logging.info(f"Servidor C2 (WSS) escuchando en {HOST}:{PORT}")
    async with server:
        await server.serve_forever()

if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
