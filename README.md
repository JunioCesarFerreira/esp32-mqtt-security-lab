# ESP32 + MQTT + TLS + HMAC-SHA256

Laboratório de segurança para publicação de telemetria de um ESP32 via MQTT,
com três camadas independentes de proteção.

## 1. Arquitetura

O ESP32 publica pela rede Wi-Fi para um broker Mosquitto rodando em Docker.
Os serviços `broker` e `subscriber` compartilham uma rede Docker privada
(`mqttnet`); apenas a porta MQTT/TLS (`8883`) do broker é exposta à rede
local, para o ESP32 alcançar.

Três camadas de segurança, independentes entre si:

- **TLS** (porta `8883`): criptografia do transporte.
- **Usuário e senha MQTT**: autenticação de acesso ao broker (via ACL, cada
  cliente só pode publicar ou assinar os tópicos que lhe cabem).
- **HMAC-SHA256**: autenticidade e integridade dos dados de ponta a ponta,
  do ESP32 até o consumidor Python. O broker MQTT **não** conhece essa
  chave — ele apenas transporta bytes.

## 2. Estrutura do repositório

```
compose.yaml                  # serviços broker + subscriber
setup.sh                      # gera certificados, credenciais e chave HMAC
mosquitto/
  config/mosquitto.conf       # listener TLS, auth, ACL
  config/acl.conf             # permissões por usuário MQTT
  config/passwd               # gerado por setup.sh (não versionado)
  certs/                      # CA e certificado do broker (gerados, não versionados)
  data/, log/                 # persistência e logs do Mosquitto
subscriber/
  Dockerfile, requirements.txt
  subscriber.py                # valida HMAC e mostra teste de adulteração
  .env                          # gerado por setup.sh (não versionado)
esp32/
  esp32.ino                    # firmware (WiFi + TLS + HMAC-SHA256 via mbedtls)
  secrets.h.example             # modelo de referência
  secrets.h, ca_cert.h          # gerados por setup.sh (não versionados)
```

## 3. Como executar

Descubra o IP do computador que vai executar o Docker (o ESP32 precisa
alcançar esse IP pela rede Wi-Fi):

```bash
hostname -I
```

Supondo `192.168.1.50`, na raiz do projeto:

```bash
./setup.sh 192.168.1.50
```

O script gera:

- CA própria e certificado TLS do broker com SAN para esse IP;
- dois usuários MQTT (`esp32-01` para publicar, `subscriber` para assinar),
  com senhas aleatórias;
- uma chave HMAC-SHA256 aleatória (32 bytes), compartilhada apenas entre o
  firmware do ESP32 (`esp32/secrets.h`) e o consumidor (`subscriber/.env`).

Edite o SSID e a senha do Wi-Fi em `esp32/secrets.h` (gerado pelo script).

Suba os containers:

```bash
docker compose up -d --build
```

No Arduino IDE, abra `esp32/esp32.ino`, instale a biblioteca `PubSubClient`,
selecione sua placa ESP32 e grave o firmware.

Para acompanhar as mensagens validadas:

```bash
docker compose logs -f subscriber
```

Saída esperada:

```
[VALIDA] device=esp32-01 seq=1
temperatura=25.00 C

TESTE: temperatura adulterada (+1 C):
REJEITADA
```

O firmware inicialmente gera temperaturas simuladas, sem depender de sensor
físico. Depois, basta substituir `readSimulatedTemperature()` pela leitura
de um DHT22, DS18B20 ou outro sensor real.

## 4. Cuidados com a rede

- O ESP32 deve acessar o **IP** do computador que hospeda o Docker — nunca
  `localhost` nem o IP interno de um container. A rede Wi-Fi precisa permitir
  essa comunicação, e a porta TCP `8883` precisa estar acessível a partir do
  ESP32.
- O certificado TLS do broker é emitido especificamente para o IP informado
  ao `setup.sh`. Se esse IP mudar, rode `setup.sh` novamente.
- O firmware carrega a CA correspondente (`esp32/ca_cert.h`) e valida o
  certificado do broker — a validação criptográfica não é desabilitada.

## 5. Modelo de assinatura HMAC

O ESP32 assina a string `"<device>|<seq>|<temperatura>"` (temperatura já
formatada com duas casas decimais) usando HMAC-SHA256 com a chave
compartilhada, e publica:

```json
{"device": "esp32-01", "seq": 1, "temp": "25.00", "hmac": "<hex>"}
```

O `subscriber.py` recalcula o mesmo HMAC a partir dos campos recebidos e
compara com `hmac.compare_digest`. Qualquer alteração em `device`, `seq` ou
`temp` — mesmo pelo próprio broker ou por um atacante na rede — invalida a
assinatura e a mensagem é marcada como `REJEITADA`.
