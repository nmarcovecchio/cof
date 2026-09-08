# Hardware v1 — CallOnFail

**Retomar:** Telegram y email ya andan. Falta **probar el ciclo de alarma**.
Alimentación: fuente 5 V 5 A + gel 12 V + **ZK-S4** (80 W provisorio, se
setea 13,7 V / 0,5 A; revisar módulo más chico) + XL4015 de backup + WDT/RESET.

Placa: WT32-ETH01 + A7672SA-FASE. Perfil `cof-wt32-a7672-v1`.

**Lab** = ya cableado y en firmware. **v1** = placa a diseñar; firmware de I/O, RESET, WDT y LEDs de gabinete todavía no.

No usar GPIO21/22 para I2C. No alimentar el WT32 por 5V y 3V3 a la vez.

---

## Qué lleva el producto

| Función | Cómo |
|---|---|
| Temp ×4 (máx. de venta) | DS18B20 en un bus, ~15–20 m, cadena |
| Humedad + temp ambiente | SHT31 en la placa |
| 220 V | ZMPT101B (no conectar 220 hasta validar) |
| Fuga de agua | IN1 (contacto seco) |
| 3 entradas más | IN2–IN4 |
| 2 salidas de campo | OUT1 sirena, OUT2 auxiliar (relé/MOSFET) |
| LEDs de **gabinete** | PWR, NET, ALARMA (frente; no los OUT de bornes) |
| Botón **RESET** de gabinete | Hundido; corta `5V_SYS` ~2 s (placa + PHY). No silencia alarmas |
| OLED | Servicio / lab, adentro, no en el frente |
| Backup 4 h | Gel 12 V 4 Ah (VRLA). LiFePO4 queda para después |

---

## 1. Alimentación (v1)

Riel de equipo: **5 V**. La fuente de 220 que ya está comprada es **5 V 5 A**.
Eso alimenta WT32 + A7672 + sensores. Un solo buck (**XL4015**) y solo para
backup. Los **3V3 salen del WT32**; no hay otro regulador de 3V3.

Gel **12 V ~4 Ah** (sellada). Usar ~la mitad. Flote **13,5–13,8 V**, no 14,7 V
de auto. La gel **no** va en paralelo con la fuente de 5 V. Se carga desde
los 5 V con un **boost CC/CV** (provisorio: **ZK-S4** 80 W; 4 A es el techo,
se setea **13,7 V / 0,5 A**. Revisar módulo más chico después).

GND común. El supervisor (WDT + one-shot del RESET) vive en **`5V_BUS`**,
antes de los MOSFET de corte. Así funciona en el banco sin gel, y sigue
vivo cuando se corta `5V_SYS`.

```text
220 VAC
  └── Fuente 5V 5A
          ├── 5V_PSU ──────────────────────────────── 5V_BUS
          │     │
          │     └── “hay PSU” → apaga el XL4015 (EN a GND)
          │
          └── ZK-S4 boost CC/CV (13,7 V / 0,5 A)  [provisorio 80 W]
                    └── Gel 12 V 4 Ah + fusible 3–5 A
                          └── XL4015 (ajuste 5,4 V) ── SB560 ── 5V_BUS
                              (solo si se cortó la 220)


5V_BUS  (+1000–2200 µF)
  ├── Supervisor WDT + one-shot RESET     (siempre que haya 5V_BUS)
  │
  ├── AO3401  →  5V_SYS
  │     WT32 pin 5V
  │     LED PWR
  │     3V3 del WT32 → OLED, SHT31, PCF, DS18B20
  │
  └── AO3401  →  5V_MODEM
        A7672 VCC (placa de lab, entra 5 V)
        +1000 µF junto al módulo
```

Hay 220: la fuente de 5 V lleva todo. El ZK-S4 deja la gel en flote a
13,7 V / 0,5 A. El XL4015 está apagado.

Se corta la 220: se muere la fuente y el boost. El EN del XL4015 se suelta,
la gel pasa a 5 V. El Schottky del buck (ajuste 5,4 V → ~5,0 V después del
diodo) no pelea con la fuente. El capellón en `5V_BUS` tapa el hueco. El
ZMPT avisa “se cortó la luz”. Ethernet off, LTE si hace falta, ~4 h.

El boost **toma 5 V_PSU**, no `5V_BUS` (si no, en backup la gel se carga a
sí misma). IN− y OUT− del ZK-S4 no se unen. El USB de esa placa no es 5 V.

### Cortes (WDT / RESET / módem)

IRF4905 / IRF9540 no cierran bien a 5 V (quieren Vgs ≈ −10 V). Usar
**P-MOSFET logic AO3401** (SOT-23, Electrocomponentes): source = `5V_BUS`,
drain = carga, gate a GND = ON, gate a `5V_BUS` = OFF. Arranca en el banco
sin gel. Through-hole: IRLZ44N high-side con gate a 12 V de la gel (solo
si la gel está siempre conectada).

| Qué | Quién lo corta | Default |
|---|---|---|
| `5V_SYS` | WDT timeout **o** botón RESET del gabinete (one-shot ~2 s) | ON |
| `5V_MODEM` | ESP32 IO (después AT / RESET del A7672) | ON |
| Gel | Nadie | Siempre conectada |

Un pulso a `EN` del ESP32 **no** resetea el PHY LAN8720. Por eso el RESET
de gabinete corta `5V_SYS`, no el pin EN.

Durante una llamada hay que seguir pateando el WDT. Timeout 4–5 min
(> llamada + TTS).

### Por qué no van 2 bucks ni buck-boost

La fuente ya es 5 V 5 A. El XL4015 es el puente 12 V → 5 V cuando no hay
220. La gel vacía sigue en ~11 V: siempre por encima de 5 V.

---

## 2. WT32 — lab (no bornes de cliente)

Ethernet interno: IO0 CLK, IO16 power, IO18 MDIO, IO23 MDC.

```text
5V_SYS  →  WT32 5V
GND     →  WT32 GND
```

Programación (3.3 V):

```text
USB-Serial TX  →  RXD0
USB-Serial RX  →  TXD0
USB-Serial GND →  GND
IO0 a GND solo para flashear; después soltar y reset
```

---

## 3. I2C — IO32 SDA, IO33 SCL (lab + v1)

Todo a 3V3 del WT32 (riel `5V_SYS`).

| Equipo | Dir. | Rol |
|---|---|---|
| OLED SH1106 | 0x3C | Lab / adentro del gabinete |
| SHT31 | 0x44 | Humedad + temp ambiente |
| PCF8574 | 0x20–0x27 | 4 IN, 2 OUT, 2 LEDs de frente |

---

## 4. DS18B20 ×4 — IO14 (lab)

```text
3V3  →  VDD (las 4)
GND  →  GND
IO14 →  DATA (un bus, en cadena, no estrella)
4.7k entre DATA y 3V3, en la placa junto al WT32
```

No parásito. Cable tipo UTP, par DATA+GND.

---

## 5. ZMPT101B — IO36 (lab, 220 después)

```text
ZMPT OUT  →  IO36
ZMPT GND  →  GND
```

---

## 6. Bornes de campo — PCF8574 (v1)

Hoy el firmware solo usa P0 como botón de lab. En v1:

| PCF | I/O | Borne |
|---|---|---|
| P0 | IN | IN1 fuga de agua |
| P1 | IN | IN2 |
| P2 | IN | IN3 |
| P3 | IN | IN4 |
| P4 | OUT | OUT1 sirena (MOSFET/ULN + relé, **no** bobina al PCF) |
| P5 | OUT | OUT2 auxiliar (igual) |

Entradas: contacto seco a GND. El PCF se escribe 0xFF (pull-up).

Estos 4 IN + 2 OUT son **solo campo**. No van LEDs, RESET de gabinete ni reset del módem.

---

## 7. Frente del gabinete — LEDs + RESET (v1)

Tres LEDs y un botón, cable a un conector de la placa. No usar OUT1/OUT2 ni IN1–4.

| Frente | Color / tipo | Qué hace | Origen |
|---|---|---|---|
| PWR | LED verde | Hay `5V_SYS` | 3V3/5V_SYS + resistor, **sin** el ESP32 |
| NET | LED verde/azul | Ethernet o MQTT OK | PCF **P6** (sink) |
| ALARMA | LED rojo | Alarma abierta | PCF **P7** (sink) |
| RESET | Botón NA, **hundido** | Power cycle de `5V_SYS` ~2 s | Al supervisor / MOSFET, **sin** el ESP32 |

Hundido (agujero, clip o botón bajo) para que no lo dispare un palo de escoba.
**No silencia alarmas.** Es “reiniciá la caja”. Al volver, si el sensor sigue mal, la alarma se vuelve a armar.

No atarlo solo a `EN` del WT32: eso no resetea el PHY.

Conector de panel (propuesta, 5 pines):

```text
1  GND
2  LED_PWR
3  LED_NET      P6
4  LED_ALARMA   P7
5  BTN_RESET    NA a GND; el otro lado dispara el one-shot de 5V_SYS
```

El RJ45 del WT32 puede traer LED de link: se queda en el jack, no en el frente.

Opcional más adelante: LED LTE. No en v1.

Botón de **servicio** (llamada corta / OTA larga): táctil **adentro**, a **IO39** → GND.
No es el RESET del frente.

---

## 8. A7672

UART lab, 115200:

```text
A7672 TXD  →  WT32 IO5
A7672 RXD  →  WT32 IO17
GND comun
5V_MODEM   →  A7672 VCC
```

Control v1 (open-collector a GND; el módulo ya tiene pull-up). No >100 nF en estos pines. No bajar RESET y PWRKEY a la vez.

| A7672 | Pin | WT32 |
|---|---|---|
| RESET | 16 | IO4, pulso bajo ~2,5 s |
| PWRKEY | 1 | IO2, encendido / apagado |

`USIM_RST` es la SIM, no el módem.

Si RESET no alcanza: MOSFET corta `5V_MODEM` y después PWRKEY.

---

## 9. WDT externo + RESET (v1)

Alimentado de `5V_BUS` (no de `5V_SYS`). DIP de Liniers: **CD4541** (timeout
~4–5 min) + **NE555** (one-shot ~2 s) + **2N7000**.

```text
IO15 ──┤ ├── (acople, no DC) ── reset del CD4541     patada
                                 │
                                 timeout 4–5 min
                                 │
BTN_RESET (NA a GND) ────────────┴── trigger NE555
                                      │
                                      Q ~2 s HIGH
                                      ├── reset del CD4541   (no re-dispara al boot)
                                      └── 2N7000 → gate AO3401_SYS a 5V_BUS
                                            → 5V_SYS OFF ~2 s → ON

IO2 / lógica ESP ── 2N7000 → gate AO3401_MODEM a 5V_BUS
                      → 5V_MODEM OFF (solo si el ESP está vivo)
```

- Patada **acoplada por capacitor**: IO15 es strapping; no puede quedar a GND
  en el reset del ESP32.
- El one-shot **reinicia el 4541**: después de un RESET hay otros 4–5 min
  para bootear y volver a patear.
- El botón dispara el **mismo** one-shot. Funciona con el firmware muerto.
- No silencia alarmas. Al volver `5V_SYS`, si el sensor sigue mal, se rearma.

### MOSFET high-side (los dos rieles 5 V)

```text
5V_BUS ── S AO3401 D ── 5V_SYS o 5V_MODEM
              G
              │
              ├── 100k a GND              ON por defecto
              └── drain 2N7000 a 5V_BUS   OFF cuando el 2N7000 conduce
                    gate 2N7000 ← one-shot (SYS) o ESP (MODEM)
```

AO3401: Rds bajo a Vgs = −4,5 V, aguanta el pico de 2 A del A7672.

### EN del XL4015

```text
5V_PSU ── 10k ── gate 2N7000
                   drain ── EN del XL4015
                   source ── GND
EN del XL4015 ── 10k a VIN (12 V gel)     HIGH = buck ON
```

Hay fuente de 5 V → EN a GND → buck apagado. Sin 5 V_PSU → EN a 12 V → backup.

LVD opcional (después): si la gel < 11,2 V, forzar EN a GND.

---

## 10. GPIO WT32 — mapa

| GPIO | Uso |
|---|---|
| 0 | ETH CLK + boot |
| 2 | A7672 PWRKEY (v1) |
| 4 | A7672 RESET (v1) |
| 5 | UART módem RX |
| 14 | DS18B20 |
| 15 | WDT kick (v1) |
| 16, 18, 23 | Ethernet |
| 17 | UART módem TX |
| 32 | I2C SDA |
| 33 | I2C SCL |
| 36 | ZMPT |
| 39 | Botón de servicio (v1, opcional) |

IO2 / IO4 / IO15 / IO39: propuesta de PCB, no están en firmware.

---

## 11. Recuperación

1. AT (CFUN, hangup).
2. RESET del A7672 (~2,5 s).
3. Cortar `5V_MODEM` + PWRKEY.
4. Cortar `5V_SYS` (placa + PHY): WDT o **botón RESET del gabinete**.

---

## 12. Qué va al frente vs adentro

**Frente (gabinete):** LEDs PWR, NET, ALARMA. Botón RESET hundido. Bornes IN1–4, OUT1–2. Jack Ethernet. (SIM / SMA antena según mecánica.)

**Adentro:** WT32, A7672, OLED, SHT31, PCF, ZMPT, fuente 5 V 5 A, ZK-S4
(carga gel, provisorio), XL4015 (backup), WDT (4541+555), AO3401, botón de
servicio (IO39), USB-serial de fábrica.

### BOM alimentación (casas tipo Liniers / Electrocomponentes / Micro)

| Qué | Para |
|---|---|
| Fuente 5 V 5 A 220 V | Ya comprada. Riel principal |
| **ZK-S4** (AE, 80 W) | Boost 5 V → 13,7 V / 0,5 A. Revisar más chico |
| Gel 12 V 4 Ah | Backup ~4 h |
| XL4015 ×1 | Solo backup 12 V → 5,4 V |
| SB560 o MBR2045CT ×1 | Buck → 5V_BUS |
| AO3401 ×2 (SOT-23) | High-side `5V_SYS` y `5V_MODEM` |
| 2N7000 / BS170 ×3 | EN del buck, corte SYS, corte módem |
| CD4541 + NE555 | WDT 4–5 min + one-shot 2 s |
| Fusible 3–5 A | + de la gel |
| 1000–2200 µF / 10 V | `5V_BUS` y VCC del A7672 |

No 1N4007 en el camino de potencia. No IRF4905/IRF9540 para conmutar 5 V.
XL6009 de un pote no carga gel. LTC3780 es overkill de tamaño.
