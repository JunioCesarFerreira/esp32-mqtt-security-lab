#!/usr/bin/env bash
# Compila e grava um dos firmwares (esp32/ ou esp32_oled/) no ESP32 usando
# arduino-cli. Se o usuário não tiver acesso à porta serial (grupo dialout),
# a gravação e o monitor rodam dentro de um container Docker com acesso à porta.
set -euo pipefail

usage() {
  cat >&2 <<EOF
Uso: $0 [sketch] [opções]

  sketch          esp32 ou esp32_oled (padrão: esp32_oled)

Opções:
  -p <porta>      porta serial (padrão: primeira /dev/ttyUSB* ou /dev/ttyACM*)
  -b <baud>       velocidade de gravação: 115200, 230400, 460800 ou 921600
                  (padrão: 115200, a única que funcionou com o adaptador do lab)
  -m              abre o monitor serial depois de gravar (Ctrl+C para sair)
  -M              só abre o monitor serial, sem compilar nem gravar
  -f              grava mesmo com Wi-Fi não configurado ou IP do broker diferente
  -h              mostra esta ajuda

Exemplos:
  $0                       # compila e grava esp32_oled
  $0 esp32_oled -m         # grava e acompanha a serial
  $0 esp32 -p /dev/ttyUSB1
  $0 -M                    # só monitor
EOF
  exit 1
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FQBN="esp32:esp32:esp32"
ESP32_INDEX_URL="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
LIBRARIES=("PubSubClient" "Adafruit SSD1306")
DOCKER_IMAGE="python:3.12-slim"

SKETCH="esp32_oled"
PORT=""
BAUD="115200"
MONITOR=0
MONITOR_ONLY=0
FORCE=0

if [ $# -gt 0 ] && [[ "$1" != -* ]]; then
  SKETCH="${1%/}"
  shift
fi

while getopts "p:b:mMfh" opt; do
  case "$opt" in
    p) PORT="$OPTARG" ;;
    b) BAUD="$OPTARG" ;;
    m) MONITOR=1 ;;
    M) MONITOR_ONLY=1 ;;
    f) FORCE=1 ;;
    *) usage ;;
  esac
done

SKETCH_DIR="$ROOT_DIR/$SKETCH"
BUILD_DIR="$ROOT_DIR/.build/$SKETCH"

if [ ! -f "$SKETCH_DIR/$SKETCH.ino" ]; then
  echo "Sketch não encontrado: $SKETCH_DIR/$SKETCH.ino" >&2
  usage
fi

# ---------------------------------------------------------------- arduino-cli

if command -v arduino-cli >/dev/null 2>&1; then
  ARDUINO_CLI="$(command -v arduino-cli)"
else
  ARDUINO_CLI="$HOME/.local/bin/arduino-cli"
fi

ensure_toolchain() {
  if [ ! -x "$ARDUINO_CLI" ]; then
    echo "==> Instalando arduino-cli em $ARDUINO_CLI"
    mkdir -p "$(dirname "$ARDUINO_CLI")"
    curl -fsSL https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Linux_64bit.tar.gz \
      | tar -xz -C "$(dirname "$ARDUINO_CLI")" arduino-cli
  fi

  if ! "$ARDUINO_CLI" core list | grep -q '^esp32:esp32 '; then
    echo "==> Instalando o core ESP32 (Espressif) - pode demorar alguns minutos"
    "$ARDUINO_CLI" config init --overwrite >/dev/null
    "$ARDUINO_CLI" config set board_manager.additional_urls "$ESP32_INDEX_URL"
    "$ARDUINO_CLI" core update-index
    "$ARDUINO_CLI" core install esp32:esp32
  fi

  local installed
  installed="$("$ARDUINO_CLI" lib list)"
  for lib in "${LIBRARIES[@]}"; do
    if ! grep -q "^$lib " <<<"$installed"; then
      echo "==> Instalando biblioteca $lib"
      "$ARDUINO_CLI" lib install "$lib"
    fi
  done
}

# ---------------------------------------------------------------- porta serial

detect_port() {
  if [ -z "$PORT" ]; then
    PORT="$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1 || true)"
  fi
  if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
    echo "Nenhuma porta serial encontrada. O ESP32 está conectado? Use -p <porta>." >&2
    exit 1
  fi
}

# Executa arduino-cli direto, ou num container com acesso à porta quando o
# usuário não pode ler/escrever nela (não está no grupo dono do dispositivo).
run_with_port() {
  if [ -r "$PORT" ] && [ -w "$PORT" ]; then
    "$ARDUINO_CLI" "$@"
    return
  fi

  if ! command -v docker >/dev/null 2>&1; then
    echo "Sem permissão em $PORT e Docker indisponível." >&2
    echo "Rode: sudo usermod -aG $(stat -c %G "$PORT") \$USER  (e faça logout/login)" >&2
    exit 1
  fi

  local tty_flag=()
  [ -t 0 ] && tty_flag=(-t)

  # Sem isso o Docker criaria as pastas montadas como root
  mkdir -p "$HOME/.arduino15" "$HOME/Arduino"

  docker run --rm -i "${tty_flag[@]}" \
    --user "$(id -u):$(id -g)" \
    --group-add "$(stat -c %g "$PORT")" \
    --device "$PORT" \
    -e HOME="$HOME" \
    -v "$HOME/.arduino15:$HOME/.arduino15" \
    -v "$HOME/Arduino:$HOME/Arduino" \
    -v "$ARDUINO_CLI:/usr/local/bin/arduino-cli:ro" \
    -v "$ROOT_DIR:$ROOT_DIR" \
    -w "$ROOT_DIR" \
    "$DOCKER_IMAGE" arduino-cli "$@"
}

monitor() {
  echo "==> Monitor serial em $PORT (115200 baud) - Ctrl+C para sair"
  run_with_port monitor -p "$PORT" -c baudrate=115200
}

# ---------------------------------------------------------------- secrets.h

check_secrets() {
  local secrets="$SKETCH_DIR/secrets.h"
  local problems=0

  if [ ! -f "$secrets" ] || [ ! -f "$SKETCH_DIR/ca_cert.h" ]; then
    echo "Faltam secrets.h/ca_cert.h em $SKETCH_DIR." >&2
    echo "Rode ./setup.sh <ip-do-host-docker> (ou copie de esp32/ para esp32_oled/)." >&2
    exit 1
  fi

  if grep -qE 'SUA_REDE_WIFI|SUA_SENHA_WIFI' "$secrets"; then
    echo "AVISO: Wi-Fi não configurado em $secrets (WIFI_SSID/WIFI_PASSWORD)." >&2
    problems=1
  fi

  local mqtt_host
  mqtt_host="$(sed -nE 's/^#define MQTT_HOST +"([^"]+)".*/\1/p' "$secrets")"
  if ! hostname -I | tr ' ' '\n' | grep -qxF "$mqtt_host"; then
    echo "AVISO: MQTT_HOST=$mqtt_host não é um IP deste computador ($(hostname -I | awk '{print $1}'))." >&2
    echo "       Se o broker roda aqui, rode ./setup.sh com o IP certo (o certificado TLS é emitido para ele)." >&2
    problems=1
  fi

  if [ "$problems" -eq 1 ] && [ "$FORCE" -eq 0 ]; then
    echo "Corrija os avisos acima ou use -f para gravar assim mesmo." >&2
    exit 1
  fi
}

# ---------------------------------------------------------------- execução

ensure_toolchain
detect_port

if [ "$MONITOR_ONLY" -eq 1 ]; then
  monitor
  exit 0
fi

check_secrets

echo "==> Compilando $SKETCH"
"$ARDUINO_CLI" compile --fqbn "$FQBN" --output-dir "$BUILD_DIR" "$SKETCH_DIR"

echo "==> Gravando em $PORT ($BAUD baud)"
run_with_port upload --fqbn "$FQBN:UploadSpeed=$BAUD" -p "$PORT" --input-dir "$BUILD_DIR" "$SKETCH_DIR"

echo "==> Gravação concluída"

if [ "$MONITOR" -eq 1 ]; then
  monitor
fi
