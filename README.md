# Monitoramento de sinais cardíacos com ESP32 e MQTT

Projeto da disciplina **Objetos Inteligentes** (Mackenzie). Um ESP32 lê (ou simula) o sinal de ECG, detecta a onda R, calcula a frequência cardíaca (bpm), aciona um **LED** e um **buzzer** conforme o estado e se comunica com um broker **MQTT**: publica bpm e alertas e recebe comandos para os atuadores.

> Projeto educacional. Não substitui equipamento médico nem diagnóstico profissional.

## Como funciona

1. O ESP32 amostra o ECG a 250 Hz (ADC no hardware real; ECG sintético na simulação).
2. Média móvel, remoção da linha de base e detecção da onda R por limiar adaptativo.
3. bpm = 60000 / média dos últimos 4 intervalos R-R.
4. Estados e atuadores:

| Estado | Condição | LED | Buzzer |
|---|---|---|---|
| Normal | 50 ≤ bpm ≤ 120 | pisca a cada batimento | desligado |
| Bradicardia | bpm < 50 | aceso | ligado |
| Taquicardia | bpm > 120 | aceso | ligado |
| Eletrodo solto | LO+ ou LO− em nível alto | pisca a 1 Hz | desligado |

5. Se o Wi-Fi ou o broker cair, os alertas locais continuam funcionando e o ESP32 tenta reconectar a cada 5 s.

Fluxograma: `docs/fluxograma.png`. Interfaces e tópicos MQTT: `docs/mqtt.md`. Hardware: `docs/hardware.md`.

## Rodar no Wokwi (simulação)

1. Entre em https://wokwi.com, faça login e crie um projeto **ESP32** (Arduino).
2. Substitua os arquivos do projeto pelos desta pasta:
   - `firmware/sketch/sketch.ino` → `sketch.ino`
   - `firmware/sketch/ecg_dsp.h` → novo arquivo `ecg_dsp.h`
   - `wokwi/diagram.json` → `diagram.json`
   - `wokwi/libraries.txt` → `libraries.txt`
3. No `sketch.ino`, troque `DEVICE_ID` por um nome só seu (os tópicos do broker público são compartilhados).
4. Clique em **Start Simulation**. No Monitor Serial deve aparecer `[mqtt] conectando... ok` e depois as linhas `[R] bpm ...`.
5. Controles da simulação:
   - **Potenciômetro**: define o bpm simulado (30 a 180).
   - **Chaves LO+ / LO−**: simulam eletrodo solto (posição para o lado do 3V3).
6. Acompanhe pelo MQTT: no cliente web do HiveMQ (https://www.hivemq.com/demos/websocket-client/) conecte em `broker.hivemq.com` e assine `ecg/SEU_ID/#`. Para comandar os atuadores, publique em `ecg/SEU_ID/cmd` os textos `buzzer:on`, `buzzer:off`, `led:on`, `led:off` ou `auto`.

Se o diagrama abrir com as peças desalinhadas, arraste as peças: o que importa são as ligações da tabela em `docs/hardware.md`.

## Medir os tempos de resposta

Com a simulação rodando e conectada ao broker:

```bash
pip install -r tools/requirements.txt
python tools/medir_tempos.py --id SEU_ID
```

O script faz 4 medições para o sensor, o LED e o buzzer e gera `tempos_resposta.csv` e `grafico_tempos.png`.

- **Sensor**: tempo entre o pedido de leitura (comando `leitura`) e a chegada da leitura no tópico `bpm` (ida e volta, pois os relógios do Wokwi e do PC não são comparáveis).
- **Atuadores**: tempo entre o envio do comando e a confirmação publicada em `ecg/SEU_ID/ack`. É ida e volta, e não depende de relógio.
- Na simulação, a velocidade do Wokwi e a rede da internet influenciam os valores. Cite isso no artigo.

## Hardware real

Em `sketch.ino`, defina `#define SIMULACAO 0`. Ligações em `docs/hardware.md` e no esquema `docs/esquema_ligacoes_hardware_real.png`. O firmware lê o AD8232 no GPIO34, o LO+ no GPIO32, o LO− no GPIO33, o SDN no GPIO27, o LED no GPIO18 e o buzzer ativo no GPIO19. Alimente com power bank quando os eletrodos estiverem no corpo.

## Testes do processamento de sinal

O gerador de ECG e o detector (`ecg_dsp.h`) não dependem do Arduino e podem ser testados no PC:

```bash
cd test
g++ -std=c++11 -I ../firmware/sketch test_dsp.cpp -o test_dsp && ./test_dsp
```

Resultado esperado: o bpm medido fica a 2 bpm do alvo entre 35 e 180 bpm, acompanha uma mudança de 60 para 130 bpm e volta a 0 quando o sinal some.

## Estrutura

```
firmware/sketch/   sketch.ino (ESP32 + MQTT) e ecg_dsp.h (ECG sintético e detector de onda R)
wokwi/             diagram.json e libraries.txt
tools/             medir_tempos.py (tempos de resposta, tabela e gráfico)
test/              test_dsp.cpp (teste do processamento no PC)
docs/              hardware.md, mqtt.md, fluxograma e esquema de ligações
```

## Limitações

- Uma única derivação de ECG, sem validação clínica.
- A biblioteca PubSubClient publica apenas com QoS 0 (assina comandos com QoS 1 e usa o Last Will com QoS 1).
- Broker público sem TLS nem autenticação: não envie dados reais de saúde.
- Na simulação o ECG é sintético, então a leitura do AD8232 real não foi exercitada.
