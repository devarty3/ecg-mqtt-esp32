# Hardware

## Componentes

| Item | Qtd. | Função |
|---|---|---|
| ESP32-DevKitC (ESP32-WROOM-32) | 1 | Processamento, ADC, Wi-Fi e cliente MQTT |
| Módulo AD8232 + 3 eletrodos | 1 | Sensor de ECG de uma derivação (hardware real) |
| Buzzer ativo (módulo de 3 pinos) | 1 | Atuador sonoro |
| LED vermelho 5 mm + resistor 220 Ω | 1 | Atuador visual |
| Protoboard, jumpers, cabo micro-USB | 1 | Montagem e alimentação |

Na simulação (Wokwi) o AD8232 é substituído por um ECG sintético gerado no firmware, o potenciômetro define o bpm e duas chaves simulam LO+ e LO−. O buzzer simulado precisa de uma onda, por isso o firmware usa `tone()`; o buzzer ativo real só precisa de nível alto.

## Ligações

| Módulo | Pino do módulo | Pino do ESP32 | Função |
|---|---|---|---|
| AD8232 (ou potenciômetro SIG) | OUTPUT | GPIO34 (ADC1) | Sinal de ECG (no Wokwi: define o bpm) |
| AD8232 (ou chave 1) | LO+ | GPIO32 | Eletrodo solto |
| AD8232 (ou chave 2) | LO− | GPIO33 | Eletrodo solto |
| AD8232 | SDN | GPIO27 | Habilita o circuito (nível alto) |
| AD8232 | 3.3V / GND | 3V3 / GND | Alimentação |
| Buzzer | S | GPIO19 | Sinal |
| LED | Ânodo (via 220 Ω) | GPIO18 | Indicação |
| LED / Buzzer | Cátodo / GND | GND | Retorno |

O GPIO34 é somente entrada e pertence ao ADC1, que funciona com o Wi-Fi ligado (o ADC2 não funciona).

## Eletrodos

RA (braço direito), LA (braço esquerdo) e RL (perna direita), com eletrodos descartáveis e cabo de 3 vias com plugue de 3,5 mm. Use power bank, e não equipamentos ligados à rede elétrica, quando os eletrodos estiverem no corpo.
