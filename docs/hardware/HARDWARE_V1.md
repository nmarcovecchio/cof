# Hardware v1 — CallOnFail

**Hardware trabado en rev 1.5** (2026-10-04). No rediseñar alimentación / OR /
MOSFET / enlace A↔B salvo bug real. Fuente de verdad: `gen_sch.py` + este doc +
`hardware/COMPRA_V1.md`. El PDF se genera local con `kicad-cli sch export pdf`;
no va en git.

**Software / lab:** Telegram y email ya andan. Falta **probar el ciclo de alarma**.
Firmware de I/O, RESET, WDT y LEDs de gabinete todavía no (driver **PCF8575**).

Placa: WT32-ETH01 + A7672SA-FASE. Perfil `cof-wt32-a7672-v1`.

Esquemático KiCad 10: `hardware/kicad/cof-v1.kicad_pro`
(hoja Alimentacion + hoja ESP32/modem/sensores).
BOM: `hardware/kicad/cof-v1-bom.csv`. Compra: `hardware/COMPRA_V1.md`. Rev **1.5.3**
(THT: **1×** NDP6020P + **2× 2N3904**; OR = **LM66200**; WDT = **TPL5010**@`5V_BUS`
+ NPN DONE; nuclear = **74HCT123** ~2 s → Q1; módem en `5V_SYS`; P12 = `SYS_KILL`↓;
EN = lab only; I/O = **PCF8575** @ 3V3; campo = PC817 + relé opto 2CH + water;
prog = **FT232** + BOOT/EN; RS485 auto IO12/IO39).
Proto en dos placas unidas por bornera / Molex (no cinta IDC); después, una sola placa.

**Lab** = ya cableado y en firmware. **v1** = placa a armar según este doc.

No usar GPIO21/22 para I2C. No alimentar el WT32 por 5V y 3V3 a la vez.

---

## Qué lleva el producto


| Función                     | Cómo                                                            |
| --------------------------- | --------------------------------------------------------------- |
| Temp ×4 (máx. de venta)     | DS18B20 en un bus, ~15–20 m, cadena                             |
| Humedad + temp ambiente     | SHT31 en la placa                                               |
| 220 V                       | ZMPT101B (no conectar 220 hasta validar)                        |
| Fuga de agua                | Water Detector contacto seco → PCF **P00** (sin PC817)          |
| Entradas de campo ×2        | IN1/IN2 vía PC817 2CH → PCF **P01** / **P13**                   |
| 2 salidas de campo          | Módulo 2 relés opto (PCF P04/P05)                               |
| Buzzer de gabinete          | PCF P03 → NPN → buzzer 5 V activo                               |
| LEDs de **gabinete**        | PWR, NET, ALARMA, **LTE** (frente)                              |
| Botón **RESET** de gabinete | Hundido; dispara one-shot ~2 s (corta `5V_SYS` + módem)         |
| RS485                       | Módulo TTL↔RS485 auto (IO12 TX / IO39 RX)                       |
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
          │     │               └── buck-boost (ajuste 5,1 V) ──┐
          │     │                   (EN=0 si hay PSU)            │
          │     └────────────────────────────────────────────── LM66200 ── 5V_BUS
          │                                                       (VIN1=PSU, VIN2=backup)
5V_BUS  (+2200 µF + 100 nF)
  ├── TPL5010 WDT (~10 min) + 74HCT123 one-shot (~2 s) → Q1
  │
  └── NDP6020P (Q1)  →  5V_SYS
        WT32 pin 5V
        A7672 VCC (+1000 µF + 100 nF en placa B)
        LED PWR
        3V3 del WT32 → OLED, SHT31, PCF, DS18B20
```

Hay 220: la fuente de 5 V lleva todo. El XY-SJVA deja la gel en flote a
6,85 V / 0,6 A. El buck-boost de backup está apagado (EN a GND vía **2N3904**).

Se corta la 220: se muere la fuente y el cargador. El EN del buck-boost se
suelta (pull-up a Gel+), el LM66200 pasa a VIN2, la gel a 5,1 V en el bus.
El capellón en `5V_BUS` tapa el hueco. El ZMPT avisa “se cortó la luz”.
Ethernet off, LTE si hace falta, ~4 h.

El XY-SJVA **toma 5V_PSU**, no `5V_BUS` (si no, en backup la gel se carga a
sí misma). **No unir** las nets con un cable. OR: módulo **LM66200** (dual
ideal-diode, ~2,5 A, pines 2,54). VIN1 = `5V_PSU`, VIN2 = XL6019, VOUT =
`5V_BUS`. EN del módulo a GND (siempre habilitado). El XY-SJVA no sirve de
backup (3 A / ~15 W a 5 V). LED verde “lleno” es ~0,1×I; en gel se ignora
(flote a 6,85 V).

IN− y OUT−: con el tester, módulo apagado. Si están abiertos, **no** los
unes ni los tires los dos al GND del WT32 (el shunt de CC va en el −).
Gel− solo a OUT−. Si ya vienen cortocircuitados en la placa, GND común
está bien.

### Cableado de banco

```text
Fuente 5V+ ──── 5V_PSU ──── LM66200 VIN1 ──┐
Fuente 5V− ──── GND                        ├── VOUT ── 5V_BUS
                 │                         │
                 ├── XY-SJVA IN+    XY-SJVA IN− ── GND fuente (no puentear OUT−)
                 │         │
                 │         OUT+ ── fusible 3–5 A ── Gel+
                 │         OUT− ────────────────── Gel−  (ver nota IN−/OUT−)
                 │
                 │   Gel+ ── buck-boost VIN+
                 │   GND  ── VIN−
                 │           VOUT+ ── LM66200 VIN2 ─┘
                 │           VOUT− ── GND
                 │           EN ── R2 10k a Gel+ ; Q_EN 2N3904 hunde EN si hay PSU
                 │
                 └── 10k ── PSU_DET ── 100k a GND ── base Q_EN (vía 10k)


5V_BUS ── 2200 µF + 100 nF a GND
       ├── TPL5010 + 74HCT123 → gate Q1 (~2 s SYS off)
       └── NDP6020P (Q1) ── 5V_SYS → WT32 5V, A7672 VCC, LED PWR, 3V3
```

XY-SJVA: **CV 6,85 V / CC 0,6 A** (vacío, después gel). Buck-boost de
backup: **5,1 V**. EN: pull-up a Gel+; con PSU el **2N3904** lo baja a ~0.

### Cortes (WDT / RESET / nuclear)

IRF4905 / IRF9540 no cierran bien a 5 V (quieren Vgs ≈ −10 V). Usar
**P-MOSFET logic NDP6020P** (TO-220): source = `5V_BUS`, drain = carga,
gate a GND = ON, gate a `5V_BUS` = OFF. Pinout de frente G-D-S. Arranca
en el banco sin gel. Si no hay NDP6020P, **IRF5305** TO-220. **No** IRF4905.


| Qué        | Quién lo corta                                             | Default           |
| ---------- | ---------------------------------------------------------- | ----------------- |
| `5V_SYS`   | 74HCT123 (~2 s): TPL timeout / P12 `SYS_KILL` / BTN RESET  | ON                |
| Módem      | Mismo Q1 (`5V_SYS`); soft = AT / RESET / PWRKEY            | ON                |
| Gel        | Nadie                                                      | Siempre conectada |


Un pulso a `EN` del ESP32 **no** resetea el PHY LAN8720. Por eso el nuclear
corta `5V_SYS` (ESP + PHY + módem). `EN` queda solo para lab (SW_EN).

Durante una llamada hay que seguir pateando el WDT. Timeout **~10 min**
(REXT 57,6 kΩ tabla TI; > llamada + TTS + margen).

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
| PCF8575     | 0x20–0x27 | 16 I/O: campo, buzzer, LEDs, SYS_KILL, ETH_RST. **VDD=3V3** |


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
ZMPT VCC  →  5V_SYS
ZMPT OUT  →  10 kΩ  → IO36
                  ├── 15 kΩ a GND
                  └── 100 nF a GND
ZMPT GND  →  GND
```

No conectar 220 V hasta validar el divisor en lab.

---

## 6. Bornes de campo — PCF8575 (v1.5)

VDD del PCF = **3V3** (no 5 V: I2C al ESP32). Firmware de gabinete aún no.

**Módulo 2 relés opto:** `VCC` (lógica/opto) a **3V3**, `JD-VCC` (bobinas) a
**5V_SYS**. Jumper **VCC–JD-VCC OFF** (aislado).


| PCF | I/O | Qué |
| --- | --- | --- |
| P00 | IN | Fuga agua: Water Detector NO→P00, COM→GND (sin opto) |
| P01 | IN | Campo IN1 vía PC817 ch1 |
| P02 | IN | Botón **servicio** (lab/mantenimiento; no RESET, no silencia) |
| P03 | OUT | Buzzer: →1k→ Q_BZ 2N3904 (coll. a buzzer−; + = 5V_SYS) |
| P04 | OUT | Relé módulo IN1 (activo LOW típico) |
| P05 | OUT | Relé módulo IN2 |
| P06 | OUT | LED NET (ánodo **3V3**, cátodo P06) |
| P07 | OUT | LED ALARMA (ánodo **3V3**, cátodo P07) |
| P10 | OUT | LED LTE (ánodo **3V3**, cátodo P10) |
| P11 | OUT | ETH_RST → 1N4148 → LAN8720 nRST (soldado en módulo) |
| P12 | OUT | SYS_KILL → TRIG_N (pulso **bajo** = nuclear ~2 s) |
| P13 | IN | Campo IN2 vía PC817 ch2 (10k PU) |
| P14–P17 | — | Spare NC |


Campo IN1/IN2 vía **PC817** (los 2 canales). Agua **no** usa opto (contacto
seco a P00). Pull-ups 10 k a 3V3 en lado MCU.

---

## 7. Frente del gabinete — LEDs + RESET (v1)

Tres LEDs y un botón, cable a un conector de la placa. No usar OUT1/OUT2 ni IN1–2.


| Frente | Color / tipo          | Qué hace                     | Origen |
| ------ | --------------------- | ---------------------------- | ------ |
| PWR    | LED verde             | Hay `5V_SYS`                 | 5V_SYS + R → GND (sin PCF) |
| NET    | LED verde/azul        | Ethernet o MQTT OK           | **3V3** + R → PCF **P06** |
| ALARMA | LED rojo              | Alarma abierta               | **3V3** + R → PCF **P07** |
| LTE    | LED (ámbar/azul)      | Enlace LTE / MQTT LTE        | **3V3** + R → PCF **P10** |
| RESET  | Botón NA, **hundido** | `5V_SYS` off mientras se mantiene | BTN → gate Q1 |


Hundido. **No silencia alarmas.**

Conector de panel (**6 pines**):

```text
1  GND
2  LED_PWR      ánodo (5V_SYS→330Ω en placa; cátodo del LED de frente a GND)
3  LED_NET      P06 (cátodo; ánodo en placa a 3V3→330Ω)
4  LED_ALARMA   P07 (cátodo; ánodo en placa a 3V3→330Ω)
5  LED_LTE      P10 (cátodo; ánodo en placa a 3V3→330Ω)
6  BTN_RESET    → TRIG_N (NA a GND; nuclear ~2 s)
```

**Buzzer** adentro: `5V_SYS` → buzzer+ ; buzzer− → collector **Q_BZ** 2N3904;
P03 → 1 kΩ → base; emitter GND. PCF nunca ve 5 V. Servicio: táctil → **PCF P02**.

Prog lab: **FT232** + **SW_BOOT** (IO0) + **SW_EN** (EN). No 5 V USB al WT32.

PHY soft-reset: PCF P11 → 1N4148 → nRST LAN8720 (hilo a pad R43 del WT32).

---

## 8. A7672

UART lab, 115200:

```text
A7672 TXD  →  WT32 IO5
A7672 RXD  →  WT32 IO17
GND comun
5V_SYS     →  A7672 VCC
```

Control v1 (open-collector a GND; el módulo ya tiene pull-up). **1 k en
serie** en RESET y PWRKEY. No >100 nF en estos pines. No bajar RESET y PWRKEY a la vez.


| A7672  | Pin | WT32                     |
| ------ | --- | ------------------------ |
| RESET  | 16  | IO4, pulso bajo ~2,5 s   |
| PWRKEY | 1   | IO2, encendido / apagado |


`USIM_RST` es la SIM, no el módem.

Si RESET soft no alcanza: `SYS_KILL` (nuclear) o botón gabinete; luego PWRKEY.

---

## 9. WDT externo + nuclear RESET (v1.5.3)

**TPL5010** a **`5V_BUS`** (siempre vivo con fuente/gel). Intervalo **~10 min**
vía REXT **57,6 kΩ** (tabla TI). `WAKE` NC. `RSTn` → net `TRIG_N` (OR open-drain
con P12 / BTN). Sin TLC555. **No** ata `EN`.

**DONE (nivel):** VDD=5 V exige DONE ≥ ~3,5 V. IO15 solo da 3,3 V → **NPN
open-drain** (`Q_DONE` 2N3904): colector = DONE + PU 10 k a `5V_BUS`; base ←
IO15 vía 1 k. Idle firmware: IO15 **alto** (DONE bajo). Pateo: pulso IO15
**bajo** (DONE sube = clear WDT).

**74HCT123** @ `5V_BUS`: trig en **A1** ← `TRIG_N` (flanco bajada; B1=VCC).
**Q** → net `HCT_Q` → 1 k → gate Q1 ~2 s (R 100 k + C 47 µF en RCx).
A1 y Q son nodos distintos (no se unen). Mitad 2: A2=VCC, B2=GND, `/CLR2`=GND.

**/CLR blank:** al enchufar, RC 100 k + 2,2 µF mantiene `/CLR` bajo ~220 ms
(> POR TPL ~120 ms) para no disparar el one-shot por el POR del TPL.

**RESET gabinete / P12:** bajan `TRIG_N` a GND → flanco en A → nuclear ~2 s.

```text
IO15 ──1k── Q_DONE ── DONE ── TPL5010@BUS ── RSTn ──┐
                                                    ├── TRIG_N ──► A1 74HCT123
SYS_KILL (P12 pulso bajo) ──────────────────────────┤              Q ──► HCT_Q ──1k── Q1 gate
BTN_RESET / SW1 ── GND ─────────────────────────────┘
/CLR1 ── RC blank (100k+2.2u a BUS/GND)
```

### MOSFET high-side

```text
5V_BUS ── S NDP6020P D ── 5V_SYS (= módem)
              G ── 100k GND ── 1k ← HCT_Q ← Q (74HCT123)
```

Solo **Q1** (sin Q3). OR rieles = LM66200.

### EN del buck-boost

```text
5V_PSU ── 10k ── PSU_DET ── 100k a GND ── 10k ── base Q_EN
Gel+ ── 10k ── BB_EN ── collector Q_EN (emitter GND)
```

Hay PSU → Q_EN ON → EN≈0 → backup off. Sin PSU → EN a Gel+ → backup on.

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

**LVD (después, en `5V_BUS`):** comparador o TL431 al mismo EN del buck-boost
(bajar EN sin el ESP). Gel bajo 5,5 V → backup off. No hace falta ADS1115.

---

## 10. GPIO WT32 — mapa (v1.5.3)

Header útil **lleno** (0 libres). `SYS_KILL` va por PCF P12.


| GPIO | Uso |
| ---- | --- |
| 0 | ETH REFCLK + SW_BOOT |
| 1 / 3 | UART0 ↔ FT232 |
| 2 | A7672 PWRKEY |
| 4 | A7672 RESET |
| 5 / 17 | UART módem RX / TX |
| 12 | RS485 TX |
| 14 | DS18B20 |
| 15 | TPL5010 DONE (vía NPN; idle alto, pateo bajo) |
| 16, 18, 23 | Ethernet (interno) |
| 32 / 33 | I2C SDA / SCL |
| 35 | ADC gel |
| 36 | ZMPT |
| 39 | RS485 RX |
| EN | SW_EN lab only |


`SYS_KILL`: PCF **P12** (pulso **bajo** = nuclear). Servicio: PCF **P02**.

---

## 11. Enlace A ↔ B (proto; después una placa)

Dos placas 9×15: A = fuente / WDT / MOSFET, B = electrónica. Se unen con
**bornera 5,08** (5 V) y **Molex KK 2,54** o más bornera (señales). **No
cinta IDC-10.** Cable a cable, pinout marcado en ambos lados. 5 V en
**0,75 mm²**. Cuando A y B anden, esas nets pasan a pistas en **una sola
placa** y J7–J10 desaparecen.

`5V_BUS` y `5V_PSU` se quedan en A. TPL5010 y 74HCT123 viven de `5V_BUS` en A
(no necesitan 3V3 de B). Pin 6 `3V3` queda por si hace falta. No pasar `GEL+`.

**J7 / J8 — 5 V (bornera 5,08)**

| Pin | Net         | Dirección |
| --- | ----------- | --------- |
| 1   | GND         | —         |
| 2   | `5V_SYS`    | A → B     |
| 3   | GND         | —         |
| 4   | `5V_SYS`    | A → B     |

**J9 / J10 — señales (Molex KK o bornera)**

| Pin | Net         | Dirección |
| --- | ----------- | --------- |
| 1   | `GEL_ADC`   | A → B     |
| 2   | `IO15`      | B → A     |
| 3   | `TRIG_N`    | B → A (P12 SYS_KILL) |
| 4   | `SPARE`     | —         |
| 5   | `TRIG_N`    | B → A (BTN panel) |
| 6   | `3V3`       | B → A     |
| 7   | `SPARE2`    | —         |
| 8   | GND         | —         |

Si un pin de señal queda abierto: el equipo falla (WDT, ADC, nuclear) pero no
se quema: gate Q1 con 100 k a GND **en A**; `TRIG_N` con PU 100 k a BUS. Si
falta **GND** y sigue habiendo 5 V, sí se puede romper el ESP.

---

## 12. Recuperación

1. AT (CFUN, hangup).
2. RESET del A7672 (~2,5 s).
3. PWRKEY cycle.
4. Nuclear `5V_SYS` (ESP + PHY + módem): TPL timeout, `SYS_KILL`, o **BTN RESET**.

---

## 13. Qué va al frente vs adentro

**Frente (gabinete):** LEDs PWR, NET, ALARMA. Botón RESET hundido. Bornes IN1–2, OUT1–2. Jack Ethernet. Buzzer adentro. (SIM / SMA antena según mecánica.)

**Adentro:** WT32, A7672, OLED, SHT31, PCF, ZMPT, fuente 5 V 5 A, XY-SJVA
(carga gel), buck-boost 5 A (backup), TPL5010, 74HCT123, NDP6020P×1, divisor
gel en IO35, botón de servicio (PCF P02), USB-serial FT232.

### Pedido AE (un prototipo)

Igual a `hardware/COMPRA_V1.md` (rev **1.5.3**). Gel **6 V 7 Ah** acá. Fuente **5 V 5 A**: ya comprada.

Piezas clave vs 1.4: **2N3904×2**, **TPL5010**, **74HCT123**, **PCF8575**, PC817
2CH, water detector, relé opto 2CH, RS485 auto, FT232, LED LTE. Salen: HCT14,
CD4541, TLC555, ULN2003, PCF8574, Q3/MODEM_CUT, relés sueltos.

No ZK-S4 / XL4015 / XL6009 1-pote / IRF4905 / cinta IDC / SB560 / 2N7000 /
HCT125 / TLC555 / NE555 / shifter I2C.

XY-SJVA: CV **6,85 V**, CC **0,6 A**, vacío primero. No unir IN−/OUT− si el
tester los ve abiertos. LED verde de “lleno” no vale para gel.

### Seguridad en el proto (rev 1.5.3)

- Gate Q1 definido **en A** (100 k a GND; idle ON).
- EN backup: R2 a Gel+; **Q_EN** baja EN si hay PSU.
- **SYS_KILL** (P12 pulso bajo) + BTN + TPL RSTn → `TRIG_N` → 74HCT123 → Q1.
- LEDs NET/ALARM/LTE a **3V3** (mismo dominio que PCF). Buzzer 5 V solo vía **Q_BZ**.
- **LM66200**: OR PSU+backup. No puentear `5V_PSU` con `5V_BUS`.
- TPL5010 a `5V_BUS`; DONE vía NPN OD; REXT 57,6 kΩ (~10 min); `/CLR` blank.
- PHY nRST: solo sink vía 1N4148; no pelear el RC del módulo.
- FT232: GND+TX+RX; **no** 5 V USB al WT32.
- Enlace proto: bornera / Molex, no IDC. No hot-plug. No 5 V y 3V3 a la vez en el WT32.