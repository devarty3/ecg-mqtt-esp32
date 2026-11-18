// Teste nativo (PC) do gerador de ECG e do detector de onda R.
// Compilar: g++ -std=c++11 -I ../firmware/sketch test_dsp.cpp -o test_dsp && ./test_dsp
#include <cstdio>
#include <cmath>
#include "ecg_dsp.h"

static float roda(float bpmAlvo, int segundos, float &bpmFinal, int &batimentos) {
  EcgSintetico g; DetectorR d;
  batimentos = 0; bpmFinal = 0;
  for (int i = 0; i < segundos * 250; i++) {
    uint32_t t = i * 4;
    if (d.processa(g.proxima(bpmAlvo, 0.004f), t)) batimentos++;
    bpmFinal = d.bpm(t);
  }
  return bpmFinal;
}

int main() {
  float alvos[] = {35, 45, 60, 75, 100, 120, 150, 180};
  int falhas = 0;
  for (float a : alvos) {
    float b; int n;
    roda(a, 30, b, n);
    float esperado = a * (30 - 2) / 60.0f;       // batimentos esperados em 28 s (após 2 s de aprendizado)
    bool ok = fabsf(b - a) <= 2.0f && fabsf(n - esperado) <= 3;
    printf("alvo %5.1f bpm -> medido %6.2f bpm, batimentos %3d  %s\n", a, b, n, ok ? "OK" : "FALHA");
    if (!ok) falhas++;
  }
  // mudança de frequência durante a execução: 60 -> 130 bpm
  {
    EcgSintetico g; DetectorR d; float b = 0; float alvo = 60;
    for (int i = 0; i < 40 * 250; i++) {
      uint32_t t = i * 4;
      if (t == 15000) alvo = 130;
      d.processa(g.proxima(alvo, 0.004f), t);
      b = d.bpm(t);
      if (t == 14996) printf("em 15 s (alvo 60): %.2f bpm\n", b);
    }
    bool ok = fabsf(b - 130) <= 2.0f;
    printf("apos mudar para 130 bpm: %.2f bpm  %s\n", b, ok ? "OK" : "FALHA");
    if (!ok) falhas++;
  }
  // sem sinal: o bpm volta a 0
  {
    EcgSintetico g; DetectorR d; float b = 0;
    for (int i = 0; i < 20 * 250; i++) {
      uint32_t t = i * 4;
      float x = (t < 10000) ? g.proxima(75, 0.004f) : 2048.0f;
      d.processa(x, t);
      b = d.bpm(t);
    }
    bool ok = (b == 0);
    printf("sinal removido apos 10 s: bpm = %.1f  %s\n", b, ok ? "OK" : "FALHA");
    if (!ok) falhas++;
  }
  printf(falhas ? "\n%d teste(s) com FALHA\n" : "\nTodos os testes passaram\n", falhas);
  return falhas ? 1 : 0;
}
