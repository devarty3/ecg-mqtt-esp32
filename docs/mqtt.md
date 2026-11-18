# Interfaces e protocolo MQTT

Protocolo MQTT 3.1.1 sobre TCP/IP, com a biblioteca PubSubClient no ESP32. Broker de testes: `broker.hivemq.com:1883` (público, sem TLS). `id` é o `DEVICE_ID` definido no firmware.

## Tópicos

| Tópico | Sentido | QoS | Retida | Conteúdo |
|---|---|---|---|---|
| `ecg/<id>/bpm` | ESP32 → broker | 0 | não | `{"bpm":72,"t":1731873000123}` a cada 5 s |
| `ecg/<id>/batimento` | ESP32 → broker | 0 | não | `{"t":...}` a cada onda R detectada |
| `ecg/<id>/alerta` | ESP32 → broker | 0 | não | `{"evento":"taquicardia","bpm":130,"t":...}` |
| `ecg/<id>/status` | ESP32 → broker | 0 (Last Will: 1) | sim | `online` / `offline` |
| `ecg/<id>/cmd` | broker → ESP32 | 1 (assinatura) | não | `buzzer:on`, `buzzer:off`, `led:on`, `led:off`, `auto`, `leitura` (publica o bpm atual em `bpm`) |
| `ecg/<id>/ack` | ESP32 → broker | 0 | não | `{"cmd":"led:on","t":...}` após acionar o atuador |

`t` é a hora UTC em milissegundos (NTP). Vale 0 se o ESP32 ainda não sincronizou.

Valores de `evento`: `normal`, `bradicardia`, `taquicardia`, `eletrodo_solto`.

## Conversões entre interfaces

- ADC (0 a 4095) → amostras de ECG → intervalos R-R → bpm (número).
- bpm e eventos → texto JSON publicado no tópico MQTT.
- Texto do comando recebido → nível lógico nos pinos GPIO18 (LED) e GPIO19 (buzzer).

## Falhas de conexão

Sem Wi-Fi ou broker, o ESP32 segue medindo e alertando localmente e tenta reconectar a cada 5 s. Ao cair sem aviso, o broker publica `offline` em `ecg/<id>/status` (Last Will).
