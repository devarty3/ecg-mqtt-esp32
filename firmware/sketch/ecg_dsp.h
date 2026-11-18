// ecg_dsp.h - processamento do sinal de ECG (sem dependências do Arduino).
// Contém: (1) gerador de ECG sintético, usado no modo simulação (Wokwi);
//         (2) detector de onda R com limiar adaptativo e cálculo de bpm.
#pragma once
#include <stdint.h>
#include <math.h>

// ---------------------------------------------------------------------------
// 1. Gerador de ECG sintético: soma de gaussianas (ondas P, Q, R, S e T)
// ---------------------------------------------------------------------------
struct EcgSintetico {
  float tempo = 0;          // segundos desde o último pico R
  uint32_t semente = 12345; // gerador pseudoaleatório simples (ruído)

  // Devolve uma amostra equivalente à leitura do ADC de 12 bits (0..4095).
  float proxima(float bpm, float dt) {
    if (bpm < 20) bpm = 20;
    float T = 60.0f / bpm;                 // período do batimento (s)
    tempo += dt;
    while (tempo >= T) tempo -= T;
    float tr = tempo;
    if (tr > T / 2) tr -= T;               // tr em [-T/2, T/2)
    float v = 0;
    for (int k = -1; k <= 1; k++) v += onda(tr + k * T);  // inclui batimentos vizinhos
    semente = semente * 1664525u + 1013904223u;
    float ruido = ((semente >> 16) & 0x7FFF) / 32767.0f - 0.5f;   // -0,5 a 0,5
    float s = 2048.0f + 1000.0f * v + 30.0f * ruido;
    if (s < 0) s = 0;
    if (s > 4095) s = 4095;
    return s;
  }

 private:
  static float g(float x, float c, float a, float w) {
    float d = x - c;
    return a * expf(-(d * d) / (2.0f * w * w));
  }
  static float onda(float x) {
    return g(x, -0.200f, 0.12f, 0.035f)    // P
         + g(x, -0.030f, -0.10f, 0.010f)   // Q
         + g(x, 0.000f, 1.00f, 0.011f)     // R
         + g(x, 0.030f, -0.20f, 0.012f)    // S
         + g(x, 0.260f, 0.30f, 0.055f);    // T
  }
};

// ---------------------------------------------------------------------------
// 2. Detector de onda R e cálculo da frequência cardíaca
// ---------------------------------------------------------------------------
struct DetectorR {
  static const int JANELA = 5;                       // média móvel (5 amostras = 20 ms a 250 Hz)
  static const uint32_t APRENDIZADO_MS = 2000;       // tempo inicial para estimar a amplitude do pico
  static const uint32_t RR_MIN_MS = 300;             // intervalo mínimo entre ondas R (200 bpm)
  static const uint32_t SEM_SINAL_MS = 4000;         // sem batimento por este tempo => bpm = 0
  static const int NRR = 4;                          // intervalos usados na média

  float buf[JANELA] = {0};
  int pos = 0, n = 0;
  float soma = 0, base = 0;
  bool baseIni = false;
  float pico = 0;
  uint32_t t0 = 0;
  bool t0Ini = false;
  bool emPico = false;
  float pMax = 0;
  uint32_t tIni = 0, tMax = 0, ultimoR = 0;
  bool temR = false;
  uint32_t rr[NRR] = {0};
  int nrr = 0, prr = 0;

  void reset() { *this = DetectorR(); }

  // Processa uma amostra (x) no instante t (ms). Devolve true quando uma onda R é confirmada.
  bool processa(float x, uint32_t t) {
    if (!t0Ini) { t0 = t; t0Ini = true; }
    soma -= buf[pos];
    buf[pos] = x;
    soma += x;
    pos = (pos + 1) % JANELA;
    if (n < JANELA) n++;
    float f = soma / n;                 // filtro de média móvel
    if (!baseIni) { base = f; baseIni = true; }
    base += 0.01f * (f - base);         // linha de base (média exponencial lenta)
    float y = f - base;

    if (t - t0 < APRENDIZADO_MS) {      // fase de aprendizado: só mede a amplitude
      if (y > pico) pico = y;
      return false;
    }

    float lim = 0.5f * pico;            // limiar adaptativo
    if (y > lim) {
      if (!emPico) { emPico = true; tIni = t; pMax = y; tMax = t; }
      else if (y > pMax) { pMax = y; tMax = t; }
    }
    if (emPico && (y <= lim || (t - tIni) > 150)) {
      emPico = false;
      if (!temR || (tMax - ultimoR) >= RR_MIN_MS) {
        if (temR) {
          rr[prr] = tMax - ultimoR;
          prr = (prr + 1) % NRR;
          if (nrr < NRR) nrr++;
        }
        ultimoR = tMax;
        temR = true;
        pico = 0.7f * pico + 0.3f * pMax;   // o limiar acompanha a amplitude recente
        return true;
      }
    }
    return false;
  }

  // Frequência cardíaca (bpm) = 60000 / média dos últimos intervalos R-R. 0 se não houver dado válido.
  float bpm(uint32_t agora) const {
    if (nrr < 2 || !temR) return 0;
    if (agora - ultimoR > SEM_SINAL_MS) return 0;
    float s = 0;
    for (int i = 0; i < nrr; i++) s += rr[i];
    return 60000.0f / (s / nrr);
  }
};
