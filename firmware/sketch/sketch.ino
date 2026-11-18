/*
  Sistema IoT para monitoramento de sinais cardíacos - ESP32 + MQTT
  Objetos Inteligentes - Mackenzie

  - Lê (ou simula) o sinal de ECG a 250 Hz, detecta a onda R e calcula o bpm.
  - Aciona LED e buzzer conforme o estado (normal, bradicardia, taquicardia, eletrodo solto).
  - Publica bpm, batimentos e alertas via MQTT e recebe comandos para os atuadores.

  SIMULACAO = 1 : roda no Wokwi (ECG sintético; potenciômetro define o bpm; chaves simulam LO+/LO-).
  SIMULACAO = 0 : hardware real (AD8232 em GPIO34, LO+ em GPIO32, LO- em GPIO33).
*/
#include <WiFi.h>
#include <PubSubClient.h>
#include <sys/time.h>
#include "ecg_dsp.h"

// ----------------------------- Configuração -------------------------------
#define SIMULACAO 1

const char* WIFI_SSID = "Wokwi-GUEST";      // rede Wi-Fi do Wokwi (sem senha)
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com"; // broker público de testes (porta 1883, sem TLS)
const uint16_t MQTT_PORT = 1883;
const char* DEVICE_ID = "mjs01";             // TROQUE por um nome único: ele faz parte dos tópicos

const float LIMITE_BRADI = 50.0f;            // bpm abaixo disso => bradicardia
const float LIMITE_TAQUI = 120.0f;           // bpm acima disso => taquicardia
const uint32_t PERIODO_AMOSTRA_US = 4000;    // 4 ms = 250 Hz
const uint32_t PERIODO_BPM_MS = 5000;        // publicação do bpm a cada 5 s
const uint32_t PERIODO_RECONEXAO_MS = 5000;

// ------------------------------- Pinos ------------------------------------
const int PIN_ECG = 34;     // saída do AD8232 (ADC1). No Wokwi: potenciômetro que define o bpm
const int PIN_LO_P = 32;    // LO+  (nível alto = eletrodo solto)
const int PIN_LO_N = 33;    // LO-  (nível alto = eletrodo solto)
const int PIN_SDN = 27;     // SDN do AD8232 (hardware real; mantido em nível alto)
const int PIN_LED = 18;
const int PIN_BUZZER = 19;

// ------------------------------ Estados -----------------------------------
enum Estado { NORMAL, BRADICARDIA, TAQUICARDIA, ELETRODO_SOLTO };
enum Modo { AUTO, FORCA_ON, FORCA_OFF };

Estado estado = NORMAL;
Estado candidato = NORMAL;
int contCandidato = 0;
Modo modoLed = AUTO, modoBuzzer = AUTO;
uint32_t ledPulsoAte = 0;
bool buzzerLigado = false;
float bpmAtual = 0;
float bpmAlvo = 75;                 // só no modo simulação

DetectorR detector;
#if SIMULACAO
EcgSintetico gerador;
#endif

WiFiClient wifi;
PubSubClient mqtt(wifi);
char tBpm[48], tBeat[48], tAlerta[48], tStatus[48], tCmd[48], tAck[48];
char idCliente[48];
uint32_t ultTentativa = 0, ultBpm = 0, proxAmostraUs = 0, ultAlvo = 0;

// ------------------------------ Utilidades --------------------------------
uint64_t epochMs() {                // hora UTC em ms (0 se o NTP ainda não sincronizou)
  struct timeval tv;
  gettimeofday(&tv, NULL);
  if (tv.tv_sec < 1700000000) return 0;
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

void publica(const char* topico, const char* msg, bool retida = false) {
  if (mqtt.connected()) mqtt.publish(topico, msg, retida);   // PubSubClient publica apenas com QoS 0
}

void publicaEvento(const char* evento) {
  char m[128];
  snprintf(m, sizeof(m), "{\"evento\":\"%s\",\"bpm\":%.0f,\"t\":%llu}", evento, bpmAtual, (unsigned long long)epochMs());
  publica(tAlerta, m);
  Serial.printf("[alerta] %s (bpm %.0f)\n", evento, bpmAtual);
}

// ------------------------------- Atuadores --------------------------------
void setBuzzer(bool ligar) {
  if (ligar == buzzerLigado) return;
  buzzerLigado = ligar;
#if SIMULACAO
  // No Wokwi o buzzer precisa de uma onda; num buzzer ativo real basta nível alto.
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    if (ligar) tone(PIN_BUZZER, 1000); else noTone(PIN_BUZZER);
  #else
    if (ligar) ledcWriteTone(0, 1000); else ledcWriteTone(0, 0);
  #endif
#else
  digitalWrite(PIN_BUZZER, ligar ? HIGH : LOW);
#endif
}

void atualizaAtuadores() {
  uint32_t agora = millis();
  bool ledAuto = false, buzAuto = false;
  switch (estado) {
    case NORMAL:         ledAuto = (agora < ledPulsoAte); break;          // pisca a cada batimento
    case BRADICARDIA:
    case TAQUICARDIA:    ledAuto = true; buzAuto = true; break;           // alerta
    case ELETRODO_SOLTO: ledAuto = ((agora / 500) % 2 == 0); break;       // pisca lento (1 Hz)
  }
  bool led = (modoLed == AUTO) ? ledAuto : (modoLed == FORCA_ON);
  bool buz = (modoBuzzer == AUTO) ? buzAuto : (modoBuzzer == FORCA_ON);
  digitalWrite(PIN_LED, led ? HIGH : LOW);
  setBuzzer(buz);
}

// --------------------------- Estado e alertas -----------------------------
void mudaEstado(Estado novo) {
  if (novo == estado) return;
  estado = novo;
  switch (novo) {
    case NORMAL:         publicaEvento("normal"); break;
    case BRADICARDIA:    publicaEvento("bradicardia"); break;
    case TAQUICARDIA:    publicaEvento("taquicardia"); break;
    case ELETRODO_SOLTO: publicaEvento("eletrodo_solto"); break;
  }
}

void classifica(float bpm) {        // exige 2 classificações iguais seguidas (evita oscilar no limite)
  Estado c = NORMAL;
  if (bpm < LIMITE_BRADI) c = BRADICARDIA;
  else if (bpm > LIMITE_TAQUI) c = TAQUICARDIA;
  if (c == candidato) contCandidato++; else { candidato = c; contCandidato = 1; }
  if (contCandidato >= 2) mudaEstado(c);
}

// -------------------------------- MQTT ------------------------------------
void aoReceber(char* topico, byte* dados, unsigned int tam) {
  char cmd[32];
  unsigned int n = tam < sizeof(cmd) - 1 ? tam : sizeof(cmd) - 1;
  for (unsigned int i = 0; i < n; i++) cmd[i] = tolower((char)dados[i]);
  cmd[n] = 0;
  while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\n' || cmd[n - 1] == '\r')) cmd[--n] = 0;

  bool ok = true;
  if (!strcmp(cmd, "buzzer:on")) modoBuzzer = FORCA_ON;
  else if (!strcmp(cmd, "buzzer:off")) modoBuzzer = FORCA_OFF;
  else if (!strcmp(cmd, "led:on")) modoLed = FORCA_ON;
  else if (!strcmp(cmd, "led:off")) modoLed = FORCA_OFF;
  else if (!strcmp(cmd, "auto")) { modoLed = AUTO; modoBuzzer = AUTO; }
  else if (!strcmp(cmd, "leitura")) {       // devolve a leitura atual do sensor (usado na medição de tempo)
    char m[64];
    float b = (estado == ELETRODO_SOLTO) ? 0 : detector.bpm(millis());
    snprintf(m, sizeof(m), "{\"bpm\":%.0f,\"t\":%llu}", b, (unsigned long long)epochMs());
    publica(tBpm, m);
  }
  else ok = false;

  if (ok) {
    atualizaAtuadores();            // aciona o atuador e só depois confirma
    char m[96];
    snprintf(m, sizeof(m), "{\"cmd\":\"%s\",\"t\":%llu}", cmd, (unsigned long long)epochMs());
    publica(tAck, m);
  }
  Serial.printf("[cmd] %s %s\n", cmd, ok ? "executado" : "desconhecido");
}

void conectaMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - ultTentativa < PERIODO_RECONEXAO_MS) return;
  ultTentativa = millis();
  Serial.print("[mqtt] conectando... ");
  // Last Will: se a conexão cair, o broker publica "offline" (retida, QoS 1)
  if (mqtt.connect(idCliente, NULL, NULL, tStatus, 1, true, "offline")) {
    Serial.println("ok");
    mqtt.publish(tStatus, "online", true);
    mqtt.subscribe(tCmd, 1);
  } else {
    Serial.printf("falhou (estado %d)\n", mqtt.state());
  }
}

// -------------------------- Aquisição e processamento ---------------------
void tratarAmostra() {
  bool solto = digitalRead(PIN_LO_P) || digitalRead(PIN_LO_N);
#if SIMULACAO
  float x = gerador.proxima(bpmAlvo, PERIODO_AMOSTRA_US / 1e6f);
#else
  float x = analogRead(PIN_ECG);
#endif
  if (solto) {                      // eletrodo solto: descarta a amostra
    if (estado != ELETRODO_SOLTO) { detector.reset(); bpmAtual = 0; mudaEstado(ELETRODO_SOLTO); }
    return;
  }
  if (estado == ELETRODO_SOLTO) {   // eletrodo reconectado
    detector.reset(); candidato = NORMAL; contCandidato = 0;
    estado = NORMAL; publicaEvento("normal");
  }
  uint32_t agora = millis();
  if (detector.processa(x, agora)) {                       // onda R detectada
    uint64_t t = epochMs();
    ledPulsoAte = agora + 80;
    bpmAtual = detector.bpm(agora);
    char m[64];
    snprintf(m, sizeof(m), "{\"t\":%llu}", (unsigned long long)t);
    publica(tBeat, m);
    if (bpmAtual > 0) { classifica(bpmAtual); Serial.printf("[R] bpm %.1f\n", bpmAtual); }
  }
}

// -------------------------------- setup/loop ------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LO_P, INPUT);
  pinMode(PIN_LO_N, INPUT);
  pinMode(PIN_SDN, OUTPUT);
  digitalWrite(PIN_SDN, HIGH);      // mantém o AD8232 ativo
#if SIMULACAO && !(defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcSetup(0, 2000, 8); ledcAttachPin(PIN_BUZZER, 0);
#endif

  snprintf(tBpm, sizeof(tBpm), "ecg/%s/bpm", DEVICE_ID);
  snprintf(tBeat, sizeof(tBeat), "ecg/%s/batimento", DEVICE_ID);
  snprintf(tAlerta, sizeof(tAlerta), "ecg/%s/alerta", DEVICE_ID);
  snprintf(tStatus, sizeof(tStatus), "ecg/%s/status", DEVICE_ID);
  snprintf(tCmd, sizeof(tCmd), "ecg/%s/cmd", DEVICE_ID);
  snprintf(tAck, sizeof(tAck), "ecg/%s/ack", DEVICE_ID);
  snprintf(idCliente, sizeof(idCliente), "esp32-ecg-%s", DEVICE_ID);

  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);          // canal 6 acelera a conexão no Wokwi
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // hora UTC (carimbos de tempo)
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(aoReceber);
  proxAmostraUs = micros();
  Serial.printf("ECG MQTT iniciado. Dispositivo: %s | simulacao: %d\n", DEVICE_ID, SIMULACAO);
}

void loop() {
  conectaMqtt();                    // sem conexão o sistema segue operando localmente
  mqtt.loop();

#if SIMULACAO
  if (millis() - ultAlvo >= 100) {  // potenciômetro define o bpm simulado: 30 a 180
    ultAlvo = millis();
    bpmAlvo = 30.0f + 150.0f * analogRead(PIN_ECG) / 4095.0f;
  }
#endif

  uint32_t us = micros();
  if ((int32_t)(us - proxAmostraUs) >= 0) {     // amostragem a 250 Hz
    proxAmostraUs += PERIODO_AMOSTRA_US;
    if ((int32_t)(us - proxAmostraUs) > 40000) proxAmostraUs = us + PERIODO_AMOSTRA_US;  // recupera atraso
    tratarAmostra();
  }

  atualizaAtuadores();

  if (millis() - ultBpm >= PERIODO_BPM_MS) {    // publica o bpm a cada 5 s
    ultBpm = millis();
    bpmAtual = (estado == ELETRODO_SOLTO) ? 0 : detector.bpm(millis());
    if (bpmAtual > 0) {
      char m[64];
      snprintf(m, sizeof(m), "{\"bpm\":%.0f,\"t\":%llu}", bpmAtual, (unsigned long long)epochMs());
      publica(tBpm, m);
    }
  }
}
