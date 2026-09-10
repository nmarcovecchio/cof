# Hardware v1 — CallOnFail

**Retomar:** Telegram y email ya andan. Falta **probar el ciclo de alarma**.
Alimentación: fuente 5 V 5 A + gel **6 V 7 Ah** + **XY-SJVA** (3 A / 35 W
CC/CV, seteo 6,85 V / 0,6 A) + buck-boost 5 A de backup + WDT/RESET.

Placa: WT32-ETH01 + A7672SA-FASE. Perfil `cof-wt32-a7672-v1`.

Esquemático KiCad 10: `hardware/kicad/cof-v1.kicad_pro`
(hoja Alimentacion + hoja ESP32/modem/sensores). PDF: `hardware/kicad/cof-v1.pdf`.
BOM del proto: `hardware/kicad/cof-v1-bom.csv`. Listado de compra:
`hardware/COMPRA_V1.md` (mismo contenido que el pedido de abajo). Rev **1.2**
(todo THT: NDP6020P TO-220 + 74HCT125 DIP; sin SOT-23).

**Lab** = ya cableado y en firmware. **v1** = placa a diseñar; firmware de I/O, RESET, WDT y LEDs de gabinete todavía no.

No usar GPIO21/22 para I2C. No alimentar el WT32 por 5V y 3V3 a la vez.

---

## Qué lleva el producto


| Función                     | Cómo                                                            |
| --------------------------- | --------------------------------------------------------------- |
| Temp ×4 (máx. de venta)     | DS18B20 en un bus, ~15–20 m, cadena                             |
| Humedad + temp ambiente     | SHT31 en la placa                                               |
| 220 V                       | ZMPT101B (no conectar 220 hasta validar)                        |
| Fuga de agua                | IN1 (contacto seco)                                             |
| 1 entrada más               | IN2                                                             |
| 2 salidas de campo          | OUT1 / OUT2 relé                                                |
| Buzzer de gabinete          | PCF P3, independiente de los relés                              |
| LEDs de **gabinete**        | PWR, NET, ALARMA (frente; no los OUT de bornes)                 |
| Botón **RESET** de gabinete | Hundido; corta `5V_SYS` ~2 s (placa + PHY). No silencia alarmas |
| OLED                        | Servicio / lab, adentro, no en el frente                        |
| Backup 4 h                  | Gel 6 V 7 Ah (VRLA). LiFePO4 queda para después                 |


---

## 1. Alimentación (v1)

Riel de equipo: **5 V**. La fuente de 220 que ya está comprada es **5 V 5 A**.
Eso alimenta WT32 + A7672 + sensores. Un solo convertidor de backup
(**buck-boost**, no el XL4015). Los **3V3 salen del WT32**; no hay otro
regulador de 3V3.

Gel **6 V 7 Ah** (sellada, tipo NP7-6). Usar ~la mitad. Flote **6,8–6,9 V**,
no 7,3 V de “carga rápida”. La gel **no** va en paralelo con la fuente de
5 V. Se carga desde `5V_PSU` con **XY-SJVA** (buck-boost CC/CV 3 A / 35 W;
a 5 V de entrada ~15 W). Seteo **6,85 V / 0,6 A**, ~C/10. No ZK-S4 (80 W).

4 Ah a 6 V queda justo para 4 h. 7 Ah deja margen de llamada y Peukert.
No dos 6 V en serie: eso es volver a 12 V.

GND común. El supervisor (WDT + one-shot del RESET) vive en **`5V_BUS`**,
antes de los MOSFET de corte. Así funciona en el banco sin gel, y sigue
vivo cuando se corta `5V_SYS`.

```text
220 VAC
  └── Fuente 5V 5A
          ├── 5V_PSU
          │     ├── XY-SJVA CC/CV (6,85 V / 0,6 A)  [3 A / 35 W]
          │     │         └── Gel 6 V 7 Ah + fusible 3–5 A
          │     │               └── buck-boost (ajuste 5,1 V) ── NDP6020P Q10 ── 5V_BUS
          │     │                   (solo si se cortó la 220)
          │     ├── “hay PSU” → apaga el buck-boost (EN a GND)
          │     └── NDP6020P Q8 (Rds ~50 mΩ) ────────────────────────────── 5V_BUS
          │
5V_BUS  (+2200 µF + 100 nF)
  ├── Supervisor WDT + one-shot RESET     (siempre que haya 5V_BUS)
  │
  ├── NDP6020P  →  5V_SYS
  │     WT32 pin 5V
  │     LED PWR
  │     3V3 del WT32 → OLED, SHT31, PCF, DS18B20
  │
  └── NDP6020P  →  5V_MODEM
        A7672 VCC (placa de lab, entra 5 V)
        +1000 µF + 100 nF junto al módulo (placa B)
```

Hay 220: la fuente de 5 V lleva todo. El XY-SJVA deja la gel en flote a
6,85 V / 0,6 A. El buck-boost de backup está apagado.

Se corta la 220: se muere la fuente y el cargador. El EN del buck-boost se
suelta, Q10 conduce, la gel pasa a 5,1 V en el bus (Rds, no diodo). Q8
corta para que el bus no alimente la PSU ni el XY-SJVA. El capellón en
`5V_BUS` tapa el hueco. El ZMPT avisa “se cortó la luz”. Ethernet off,
LTE si hace falta, ~4 h.

El XY-SJVA **toma 5V_PSU**, no `5V_BUS` (si no, en backup la gel se carga a
sí misma). **No unir** las nets con un cable. OR: **Q8** (PSU) y **Q10**
(backup), ambos NDP6020P, source = `5V_BUS`, drain = la fuente correspondiente.
Hay PSU → Q9 ON → Q8 ON, EN=0 → Q11 OFF → Q10 OFF.
Sin PSU → Q8 OFF, EN=Gel+ → Q11 ON → Q10 ON.
~50 mΩ, ~0,1 W a 1,5 A, no 0,6 W de Schottky. El XY-SJVA no sirve de backup
(3 A / ~15 W a 5 V). LED verde “lleno” es ~0,1×I; en gel se ignora (flote a
6,85 V).

IN− y OUT−: con el tester, módulo apagado. Si están abiertos, **no** los
unes ni los tires los dos al GND del WT32 (el shunt de CC va en el −).
Gel− solo a OUT−. Si ya vienen cortocircuitados en la placa, GND común
está bien.

### Cableado de banco

```text
Fuente 5V+ ──── 5V_PSU ──── NDP6020P Q8 ──────────────── 5V_BUS
Fuente 5V− ──── GND
                 │
                 ├── XY-SJVA IN+    XY-SJVA IN− ── GND fuente (no puentear OUT−)
                 │         │
                 │         OUT+ ── fusible 3–5 A ── Gel+
                 │         OUT− ────────────────── Gel−  (ver nota IN−/OUT−)
                 │
                 │   Gel+ ── buck-boost VIN+
                 │   GND  ── VIN−
                 │           VOUT+ ── NDP6020P Q10 ──> 5V_BUS
                 │           VOUT− ── GND
                 │           EN ── 2N7000 Q11 (HIGH = Q10 ON)
                 │
                 └── 10k ── gate 2N7000 (Q5) ── 100k a GND
                              source GND
                              drain ── EN del buck-boost
                     Gel+ ── 10k ── EN     (HIGH = backup ON)


5V_BUS ── 2200 µF + 100 nF a GND
       ├── WDT + one-shot RESET
       ├── NDP6020P ── 5V_SYS    → WT32 5V, LED PWR, 3V3
       └── NDP6020P ── 5V_MODEM  → A7672 VCC +1000 µF (en placa B)
```

XY-SJVA: **CV 6,85 V / CC 0,6 A** (vacío, después gel). Buck-boost de
backup: **5,1 V** (antes 5,4 V para compensar el Schottky; ya no hay). Si
ese módulo no trae EN, el 2N7000 maneja un NDP6020P que corta gel+ al VIN.

### Cortes (WDT / RESET / módem)

IRF4905 / IRF9540 no cierran bien a 5 V (quieren Vgs ≈ −10 V). Usar
**P-MOSFET logic NDP6020P** (TO-220): source = `5V_BUS`, drain = carga,
gate a GND = ON, gate a `5V_BUS` = OFF. Pinout de frente G-D-S. Arranca
en el banco sin gel. No usar la gel 6 V como “12 V de gate” para un
P-MOSFET de 5 V: no alcanza Vgs. Si no hay NDP6020P, **IRF5305** TO-220
(peor Rds a −5 V; alcanza para el proto). **No** IRF4905 / IRF9540.


| Qué        | Quién lo corta                                             | Default           |
| ---------- | ---------------------------------------------------------- | ----------------- |
| `5V_SYS`   | WDT timeout **o** botón RESET del gabinete (one-shot ~2 s) | ON                |
| `5V_MODEM` | ESP32 IO (después AT / RESET del A7672)                    | ON                |
| Gel        | Nadie                                                      | Siempre conectada |


Un pulso a `EN` del ESP32 **no** resetea el PHY LAN8720. Por eso el RESET
de gabinete corta `5V_SYS`, no el pin EN.

Durante una llamada hay que seguir pateando el WDT. Timeout 4–5 min
(> llamada + TTS).

### Por qué 6 V y por qué buck-boost

6 V queda más cerca del riel de 5 V: carga 5 V → 6,85 V (menos salto que
13,7 V), bateria más chata, mismo tipo NP7-6 de alarma / luces de
emergencia. No 12 V.

La gel 6 V llena está ~6,8 V; vacía (corte) ~5,5 V. Eso **cruza** los 5,1 V
del backup. Un buck (XL4015) necesita ~1,5 V de cabeza: solo sirve con la
bateria llena. Un boost solo se queda corto cuando está llena. El puente
es un **buck-boost automático** a 5,1 V (módulo 5 A tipo **XL6019**, no
el XY-SJVA, no ZK-4KX). El **XL6009 de un pote es solo boost**: con la gel
llena (~6,8 V) no puede entregar 5,1 V. Si no trae EN, un NDP6020P corta el
+ de la gel al convertidor mientras haya `5V_PSU`.

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


| Equipo      | Dir.      | Rol                           |
| ----------- | --------- | ----------------------------- |
| OLED SH1106 | 0x3C      | Lab / adentro del gabinete    |
| SHT31       | 0x44      | Humedad + temp ambiente       |
| PCF8574     | 0x20–0x27 | 2 IN, 2 OUT, buzzer, 2 LEDs de frente |


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
ZMPT OUT  →  1 kΩ  → IO36
             └── 100 nF a GND
ZMPT GND  →  GND
```

---

## 6. Bornes de campo — PCF8574 (v1)

Hoy el firmware solo usa P0 como botón de lab. En v1:


| PCF | I/O | Qué                                                              |
| --- | --- | ---------------------------------------------------------------- |
| P0  | IN  | IN1 fuga (borne)                                                 |
| P1  | IN  | IN2 extra (borne)                                                |
| P2  | —   | Spare, 10 k a 3V3, sin borne                                     |
| P3  | OUT | **Buzzer** 5 V (ULN O3). Adentro. Regla aparte, no es OUT de campo |
| P4  | OUT | OUT1 relé (ULN O1)                                               |
| P5  | OUT | OUT2 relé (ULN O2)                                               |
| P6  | OUT | LED NET (sink)                                                   |
| P7  | OUT | LED ALARMA (sink)                                                |


Entradas: contacto seco a GND. **10 k a 3V3 en la placa** (R27 R28), no
confiar solo en el pull-up débil del PCF. El firmware igual escribe 0xFF.

Los 2 IN + 2 OUT de bornes son **campo**. El buzzer es P3. LEDs P6/P7 al
frente. No usar bornes para LEDs ni RESET.

---

## 7. Frente del gabinete — LEDs + RESET (v1)

Tres LEDs y un botón, cable a un conector de la placa. No usar OUT1/OUT2 ni IN1–2.


| Frente | Color / tipo          | Qué hace                     | Origen                                   |
| ------ | --------------------- | ---------------------------- | ---------------------------------------- |
| PWR    | LED verde             | Hay `5V_SYS`                 | 5V_SYS + resistor, **sin** el ESP32      |
| NET    | LED verde/azul        | Ethernet o MQTT OK           | PCF **P6** (sink)                        |
| ALARMA | LED rojo              | Alarma abierta               | PCF **P7** (sink)                        |
| RESET  | Botón NA, **hundido** | Power cycle de `5V_SYS` ~2 s | Al supervisor / MOSFET, **sin** el ESP32 |


Hundido (agujero, clip o botón bajo) para que no lo dispare un palo de escoba.
**No silencia alarmas.** Es “reiniciá la caja”. Al volver, si el sensor sigue mal, la alarma se vuelve a armar.

No atarlo solo a `EN` del WT32: eso no resetea el PHY.

Conector de panel (5 pines):

```text
1  GND
2  LED_PWR
3  LED_NET      P6
4  LED_ALARMA   P7
5  BTN_RESET    NA a GND; el otro lado dispara el one-shot de 5V_SYS
```

**Buzzer** adentro, PCF **P3** → ULN O3. Una regla puede pedirlo **sin** cerrar OUT1/OUT2.

El RJ45 del WT32 puede traer LED de link: se queda en el jack (además del NET de frente).

Opcional más adelante: LED LTE. No en v1.

Botón de **servicio** (llamada corta / OTA larga): táctil **adentro**, a **IO39** → GND,
con **10 k a 3V3** (IO39 no tiene pull interno).
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

Control v1 (open-collector a GND; el módulo ya tiene pull-up). **1 k en
serie** en RESET y PWRKEY. No >100 nF en estos pines. No bajar RESET y PWRKEY a la vez.


| A7672  | Pin | WT32                     |
| ------ | --- | ------------------------ |
| RESET  | 16  | IO4, pulso bajo ~2,5 s   |
| PWRKEY | 1   | IO2, encendido / apagado |


`USIM_RST` es la SIM, no el módem.

Si RESET no alcanza: MOSFET corta `5V_MODEM` y después PWRKEY.

---

## 9. WDT externo + RESET (v1)

Alimentado de `5V_BUS` (no de `5V_SYS`). DIP: **CD4541** (timeout ~4–5 min)
+ **TLC555** (CMOS, one-shot ~2 s). No NE555 bipolar (HIGH ≈ 3,7 V, no
apaga el NDP6020P). Vale 7555 / LMC555 / TS555, mismo encapsulado DIP-8.

```text
IO15 ──┤ ├── (acople, no DC) ── reset del CD4541     patada
                                 │
                                 timeout 4–5 min
                                 │
BTN_RESET (NA a GND) ────────────┴── trigger TLC555
                                      │
                                      Q ~2 s HIGH ≈ 5 V
                                      ├── reset del CD4541
                                      └── 1k ── gate Q1 NDP6020P (100k a GND)
                                            → 5V_SYS OFF ~2 s → ON

MODEM_CUT (GPIO 3,3 V, HIGH=corte) ── 1k + 100k PD
                      ── 74HCT125 DIP (VCC=5V_BUS, 1OE=GND)
                      ── gate Q3 NDP6020P (100k a GND)
                      → 5V_MODEM OFF
```

- Patada **acoplada por capacitor**: IO15 es strapping; no puede quedar a GND
en el reset del ESP32.
- El one-shot **reinicia el 4541**: después de un RESET hay otros 4–5 min
para bootear y volver a patear.
- El botón dispara el **mismo** one-shot. Funciona con el firmware muerto.
- No silencia alarmas. Al volver `5V_SYS`, si el sensor sigue mal, se rearma.

### MOSFET high-side (los dos rieles 5 V)

**`5V_SYS`:** el TLC555 sale a 5 V. 1 k al gate, 100 k a GND (default ON).
Idle LOW → ON. Pulso HIGH → gate a 5 V → OFF. Sin 2N7000.

**`5V_MODEM`:** el GPIO es 3,3 V. Un shifter de I2C (TXS/BSS138) no sirve
(open-drain, flojo). Un **buffer HCT a 5 V** sí: entrada TTL (3,3 V = HIGH),
salida 0/5 V al gate. **74HCT125** DIP-14 (un buffer: pin 1=OE, 2=A, 3=Y,
7=GND, 14=VCC; OE no usados 6/10/13 a VCC, A no usadas 4/9/12 a GND).
Reemplaza Q4+Q7. No SOT-23.

```text
5V_BUS ── S NDP6020P D ── 5V_SYS
              G ── 100k GND ── 1k ── TLC555 OUT

5V_BUS ── S NDP6020P D ── 5V_MODEM
              G ── 100k GND ── 1Y (pin 3) 74HCT125
                               1A (pin 2) ── 1k ── MODEM_CUT ── 100k GND
                               VCC pin 14 = 5V_BUS   1OE pin 1 = GND
```

Cinta suelta: A a GND vía 100 k → Y=0 → módem ON. No TXS0102 / módulos
“I2C 3.3↔5”. Eso no mueve un P-FET de potencia.

El HCT **no** reemplaza Q5/Q8/Q9/Q10/Q11: eso es corriente de riel, no un
GPIO.

NDP6020P: Rds ~50 mΩ a Vgs = −4,5 V, aguanta el pico de 2 A del A7672.

### EN del buck-boost

```text
5V_PSU ── 10k ── gate Q5 2N7000 ── 100k a GND
                   drain ── EN del buck-boost (o gate NDP6020P de corte)
                   source ── GND
EN ── 10k a VIN (6 V gel)     HIGH = backup ON
```

Hay fuente de 5 V → EN a GND → backup apagado. Sin 5V_PSU → EN a 6 V →
backup.

Usar la gel en un corte es normal. Lo que la mata es seguir chupando
cuando ya está vacía (menos de ~5,5 V, sulfato). El convertidor de backup
no corta solo: quiere 5,1 V hasta que no puede más.

### ADC gel (firmware) + LVD (hardware)

**ADC (v1):** Gel+ al WT32 **IO35** (ADC1, pin libre). No IO36 (ZMPT) ni
32/33 (I2C).

```text
Gel+ ── 47 kΩ ──┬── IO35
                └── 22 kΩ a GND
                └── 100 nF a GND
```

~2,2 V a 6,9 V; ~1,75 V a 5,5 V. Medir Gel+ vs GND del equipo. MQTT:
tensión, aviso p.ej. bajo 6,0 V. El firmware puede bajar EN bajo 5,5 V;
si el ESP está muerto, no alcanza.

**LVD (después, en `5V_BUS`):** comparador o TL431 + 2N7000 al mismo EN
del buck-boost. Gel bajo 5,5 V → EN a GND, sin el ESP. No hace falta
ADS1115.

---

## 10. GPIO WT32 — mapa


| GPIO       | Uso                              |
| ---------- | -------------------------------- |
| 0          | ETH CLK + boot                   |
| 2          | A7672 PWRKEY (v1)                |
| 4          | A7672 RESET (v1)                 |
| 5          | UART módem RX                    |
| 14         | DS18B20                          |
| 15         | WDT kick (v1)                    |
| 16, 18, 23 | Ethernet                         |
| 17         | UART módem TX                    |
| 32         | I2C SDA                          |
| 33         | I2C SCL                          |
| 35         | ADC gel (v1)                     |
| 36         | ZMPT                             |
| 39         | Botón de servicio (v1, opcional) |


IO2 / IO4 / IO15 / IO35 / IO39: propuesta de PCB, no están en firmware.
`MODEM_CUT` (J3): GPIO de salida TBD; no IO39, no IO2, no strapping. HIGH = corta `5V_MODEM`.

---

## 11. Cinta A ↔ B (IDC-10)

Conector polarizado. **GND en 1, 3 y 10** (un desliz de un pin pone 5 V contra GND, no contra GPIO). No enchufar con la fuente prendida.

| Pin | Net         | Dirección |
| --- | ----------- | --------- |
| 1   | GND         | —         |
| 2   | `5V_SYS`    | A → B     |
| 3   | GND         | —         |
| 4   | `5V_MODEM`  | A → B     |
| 5   | `GEL_ADC`   | A → B     |
| 6   | `IO15`      | B → A     |
| 7   | `MODEM_CUT` | B → A     |
| 8   | `LED_PWR`   | A → B     |
| 9   | `BTN_RESET` | B → A     |
| 10  | GND         | —         |

`5V_BUS`, `5V_PSU` y el WDT se quedan en A. `3V3` se queda en B (sale del WT32).

Si un pin de señal queda abierto: el equipo falla (WDT, ADC, corte de módem)
pero no se quema: gates con 100 k a GND **en A**, `MODEM_CUT` con pull-down
al HCT. Si falta **GND** y sigue habiendo 5 V, sí se puede romper el ESP.

---

## 12. Recuperación

1. AT (CFUN, hangup).
2. RESET del A7672 (~2,5 s).
3. Cortar `5V_MODEM` + PWRKEY.
4. Cortar `5V_SYS` (placa + PHY): WDT o **botón RESET del gabinete**.

---

## 13. Qué va al frente vs adentro

**Frente (gabinete):** LEDs PWR, NET, ALARMA. Botón RESET hundido. Bornes IN1–2, OUT1–2. Jack Ethernet. Buzzer adentro. (SIM / SMA antena según mecánica.)

**Adentro:** WT32, A7672, OLED, SHT31, PCF, ZMPT, fuente 5 V 5 A, XY-SJVA
(carga gel), buck-boost 5 A (backup), WDT (4541+555), NDP6020P, divisor gel
en IO35, botón de servicio (IO39), USB-serial de fábrica.

### Pedido AE (un prototipo)

Igual a `hardware/COMPRA_V1.md`. Alineado al BOM. Gel **6 V 7 Ah** (NP7-6):
acá, no China. Fuente **5 V 5 A**: ya comprada.


| Cant. pedido | Buscar | Usa el proto | Para |
| ------------ | ------ | ------------ | ---- |
| 1 | **XY-SJVA** (o XY-SJVA-4) CC/CV 3 A 35 W, **dos potes** | 1 | Carga gel **6,85 V / 0,6 A** desde `5V_PSU` |
| 1 | Buck-boost auto **XL6019** (no XL6009 de un pote, no ZK-4KX) | 1 | Backup gel → **5,1 V** |
| 5–10 | **NDP6020P** TO-220 (alt **IRF5305**) | **4** | Q1 SYS, Q3 módem, Q8 OR PSU, Q10 OR backup. No IRF4905 |
| pack 50 | **2N7000** TO-92 | **3** | Q5 EN, Q9, Q11. No inversor de módem |
| 5–10 | **74HCT125** DIP-14 | **1** | 3,3 V → 5 V al gate de Q3. No TXS/BSS138 |
| 5–10 | **CD4541BE** DIP-16 | 1 | WDT ~4–5 min |
| 10 | **TLC555** (7555 / LMC555) DIP-8 **CMOS** | 1 | One-shot ~2 s. **No NE555 bipolar** |
| pack | **1N4148** | **3** | D3 D4 D5 (WDT) |
| pack | **1N4007** | **2** | Flyback relés. No en el riel de 5 V |
| 4 | DS18B20 waterproof | 4 | Temps |
| 1 | SHT31 I2C | 1 | Humedad |
| 1 | PCF8574 módulo | 1 | 2 IN + 2 OUT + buzzer P3 + LEDs NET/ALARMA |
| 1 | OLED 1.3" SH1106 | 1 | Si no está el del lab |
| 1 | ULN2003 + 2 relés 5 V | 1 | OUT1 / OUT2 |
| 5–10 | **Buzzer activo 5 V** | **1** | PCF **P3** → ULN O3. Independiente de OUT1/OUT2 |
| 5+1 | Fusible 5×20 **5 A** + porta | 1 | Gel+ |
| 10 c/u | 1000 µF y 2200 µF / 16 V | 1+1 | `5V_BUS`, A7672 |
| 10 | 100 µF, 10 µF | 1+1 | 555 y `5V_SYS` |
| packs | 10 k, 100 k, 47 k, 22 k, 4k7, **1 k**, 330, **1 M**, 100 nF / 10 nF **paso 2,54** | ver BOM | |
| 20 c/u | LED 5 mm verde / rojo / azul | 3 | PWR / ALARMA / NET |
| 1 | Pulsador panel NA hundido + táctiles 6×6 | 2 | RESET + servicio |
| 1 set | Bornera 5,08 mm | — | IN/OUT |
| 1 set | **IDC-10** ×2 + cinta 10 hilos polarizada | 2 | J7 / J8 |


No ZK-S4. No XL4015. No XL6009 de un pote. No IRF4905/IRF9540. **No SB560**
(el OR es MOSFET). **No NE555 bipolar**. No 1N4007 en el riel de 5 V. No
shifter I2C para el módem. No SOT-23 (AO3401 / 74AHCT1G125): este proto es
THT. 100 nF / 10 nF en paso 2,54 mm, no 0805.

XY-SJVA: CV **6,85 V**, CC **0,6 A**, vacío primero. No unir IN−/OUT− si el
tester los ve abiertos. LED verde de “lleno” no vale para gel.

### Seguridad en el proto (rev 1.2)

- Gates de MOSFET definidos **en la placa que tiene el FET**, no del otro lado de la cinta.
- `MODEM_CUT` y WDT: 1 k serie + 100 k a GND. Default = riel ON. **R20 100 k** baja MR del 4541 (si no, queda en reset).
- Q5 (EN backup): 100 k a GND. Sin PSU el backup arranca, no queda a medias.
- Q8 + Q10 NDP6020P: OR de PSU y backup (Rds). Nunca puentear `5V_PSU` con `5V_BUS`.
- 100 nF junto a 4541, 555, PCF, WT32 5 V, A7672; 10 µF en `5V_SYS`; 1000 µF del módem **en B**.
- IO39 e IN1–2: pull-up 10 k a 3V3 en placa.
- RESET/PWRKEY del A7672: 1 k serie. ZMPT: 1 k + 100 nF.
- IDC polarizado, tres GND, no hot-plug. No 5 V y 3V3 a la vez en el WT32.