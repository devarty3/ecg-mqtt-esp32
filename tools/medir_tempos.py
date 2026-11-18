#!/usr/bin/env python3
"""
Mede os tempos de resposta do protótipo via MQTT e gera a tabela e o gráfico do artigo.

  Sensor (AD8232/ECG simulado): tempo entre o pedido de leitura (cmd "leitura") e a chegada da
      leitura (ecg/<id>/bpm) no PC. É ida e volta, porque os relógios do Wokwi e do PC não são
      comparáveis (o simulador roda mais devagar que o tempo real).
  Atuadores (LED e buzzer): tempo entre o envio do comando (ecg/<id>/cmd) e o recebimento da
      confirmação (ecg/<id>/ack), publicada pelo ESP32 logo depois de acionar o atuador.
      É um tempo de ida e volta (não depende de relógio), então é um pouco maior que só a ida.

Uso:
  pip install -r requirements.txt
  python medir_tempos.py --id mjs01            # 4 medições por sensor/atuador
  python medir_tempos.py --id mjs01 --n 4 --host broker.hivemq.com
Com o ESP32 (ou o Wokwi) rodando e conectado ao broker.
"""
import argparse, csv, json, statistics, sys, threading, time
import paho.mqtt.client as mqtt

ap = argparse.ArgumentParser()
ap.add_argument("--id", required=True, help="DEVICE_ID configurado no firmware")
ap.add_argument("--host", default="broker.hivemq.com")
ap.add_argument("--port", type=int, default=1883)
ap.add_argument("--n", type=int, default=4, help="medições por sensor/atuador")
ap.add_argument("--timeout", type=float, default=15.0, help="espera máxima por resposta (s)")
a = ap.parse_args()

T_BEAT = f"ecg/{a.id}/batimento"
T_ACK = f"ecg/{a.id}/ack"
T_CMD = f"ecg/{a.id}/cmd"
T_BPM = f"ecg/{a.id}/bpm"

beats, acks, bpms = [], [], []
cond = threading.Condition()

def on_message(c, u, m):
    recv_ms = time.time() * 1000
    try:
        d = json.loads(m.payload.decode())
    except Exception:
        return
    with cond:
        if m.topic == T_BEAT:
            beats.append((recv_ms, d.get("t", 0)))
        elif m.topic == T_BPM:
            bpms.append(time.perf_counter())
        elif m.topic == T_ACK:
            acks.append((time.perf_counter(), d.get("cmd", "")))
        cond.notify_all()

cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"medidor-{a.id}-{int(time.time())}")
cli.on_message = on_message
cli.connect(a.host, a.port, 60)
cli.subscribe([(T_BEAT, 0), (T_ACK, 1), (T_BPM, 0)])
cli.loop_start()
time.sleep(1.5)

rows = []   # (num, nome, tempo_ms)

# ---------------- sensor ----------------
# Os relógios do Wokwi e do PC não são comparáveis (o simulador roda mais devagar que o tempo real),
# então o sensor é medido por ida e volta: pedido de leitura (cmd "leitura") -> leitura publicada em bpm.
nome = "Sensor AD8232 (pedido de leitura → leitura recebida)"
print("Medindo o sensor ...")
for i in range(1, a.n + 1):
    ok = None
    for tentativa in range(1, 6):
        with cond: bpms.clear()
        t0 = time.perf_counter()
        cli.publish(T_CMD, "leitura", qos=1)
        with cond:
            fim = time.time() + 8
            while not bpms and time.time() < fim:
                cond.wait(timeout=0.5)
            if bpms: ok = bpms[0]
        if ok: break
        print(f"  sem resposta do sensor (tentativa {tentativa}/5), tentando de novo ...")
        time.sleep(3)
    if not ok:
        sys.exit("Sem resposta do sensor. Confira o DEVICE_ID e se o ESP32 está online.")
    rows.append((i, nome, (ok - t0) * 1000))
    time.sleep(1.0)

# ---------------- atuadores ----------------
def mede_comando(dev):
    nome = f"Atuador {'LED' if dev == 'led' else 'buzzer'} (comando → acionamento)"
    for i in range(1, a.n + 1):
        cmd = f"{dev}:on"
        ok = []
        for tentativa in range(1, 6):          # a conexão do Wokwi oscila: tenta de novo se não houver confirmação
            with cond: acks.clear()
            t0 = time.perf_counter()
            cli.publish(T_CMD, cmd, qos=1)
            with cond:
                fim = time.time() + 8
                while not any(c == cmd for _, c in acks) and time.time() < fim:
                    cond.wait(timeout=0.5)
                ok = [t for t, c in acks if c == cmd]
            if ok: break
            print(f"  sem confirmação de {cmd} (tentativa {tentativa}/5), tentando de novo ...")
            time.sleep(3)
        if not ok:
            sys.exit(f"Sem confirmação do comando {cmd}. Confira o DEVICE_ID e se o ESP32 está online.")
        rows.append((i, nome, (ok[0] - t0) * 1000))
        with cond: acks.clear()
        cli.publish(T_CMD, f"{dev}:off", qos=1)
        time.sleep(1.0)

print("Medindo o LED ...");    mede_comando("led")
print("Medindo o buzzer ..."); mede_comando("buzzer")
cli.publish(T_CMD, "auto", qos=1)
time.sleep(0.5)
cli.loop_stop(); cli.disconnect()

# ---------------- resultados ----------------
print("\nTabela 6 - Tempos de resposta do sensor e dos atuadores\n")
print(f"{'Núm. medida':<12}{'Sensor/atuador':<52}{'Tempo de resposta':>18}")
medias = {}
for nm in dict.fromkeys(r[1] for r in rows):
    v = [r[2] for r in rows if r[1] == nm]
    for r in rows:
        if r[1] == nm: print(f"{r[0]:<12}{nm:<52}{r[2]:>14.0f} ms")
    medias[nm] = statistics.mean(v)
    print(f"{'Média':<12}{nm:<52}{medias[nm]:>14.0f} ms\n")

with open("tempos_resposta.csv", "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f); w.writerow(["num_medida", "sensor_atuador", "tempo_ms"])
    for r in rows: w.writerow([r[0], r[1], f"{r[2]:.1f}"])
    for nm, m in medias.items(): w.writerow(["media", nm, f"{m:.1f}"])
print("Salvo: tempos_resposta.csv")

try:
    import matplotlib; matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    nomes = list(medias)
    curtos = ["Sensor\n(pedido → leitura)", "LED\n(comando → ação)", "Buzzer\n(comando → ação)"]
    fig, ax = plt.subplots(figsize=(8, 4.5))
    larg = 0.18
    for i in range(a.n):
        ax.bar([k + (i - (a.n - 1) / 2) * larg for k in range(len(nomes))],
               [[r[2] for r in rows if r[1] == nm][i] for nm in nomes], larg, label=f"Medição {i+1}")
    ax.plot(range(len(nomes)), [medias[nm] for nm in nomes], "k_", markersize=60, mew=2, label="Média")
    ax.set_xticks(range(len(nomes))); ax.set_xticklabels(curtos)
    ax.set_ylabel("Tempo de resposta (ms)"); ax.set_title("Tempos de resposta do sensor e dos atuadores")
    ax.legend(); ax.grid(axis="y", alpha=.3); fig.tight_layout()
    fig.savefig("grafico_tempos.png", dpi=200); print("Salvo: grafico_tempos.png")
except ImportError:
    print("matplotlib não instalado: gráfico não gerado (pip install matplotlib).")
