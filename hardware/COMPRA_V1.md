# Listado de compra — CallOnFail v1.5.3

**Trabado con el esquemático rev 1.5.3.** No cambiar partes de alimentación sin
actualizar `docs/hardware/HARDWARE_V1.md` y `hardware/kicad/gen_sch.py` juntos.

Alineado a `cof-v1-bom.csv`. Un prototipo. En AE se piden packs: el proto usa
la columna **Usa**. Todo lo que se suelda a pata es THT (no SOT-23 suelto).
Los módulos (WT32, A7672, XY-SJVA, XL6019, LM66200, PCF8575, SHT31, OLED,
FT232, RS485, PC817, relé 2CH, water detector) ya vienen armados.

Gel **6 V 7 Ah** (NP7-6): comprar acá, no China. Fuente **5 V 5 A**: ya está.

## Módulos

| Comprar | Pedir | Usa | Para |
| ------- | ----- | --- | ---- |
| **XY-SJVA** o XY-SJVA-4 CC/CV 3 A 35 W, **dos potes** | 1 | 1 | Carga gel 6,85 V / 0,6 A desde `5V_PSU`. No ZK-S4 |
| Buck-boost auto **XL6019** (no XL6009 de un pote, no ZK-4KX) | 1 | 1 | Backup gel → **5,1 V** |
| **LM66200** dual ideal-diode (módulo pines 2,54 / tipo Adafruit) | 1–2 | **1** | OR PSU + XL6019 → `5V_BUS` |
| **PCF8575** módulo I2C (16 bit) | 1 | 1 | Campo, LEDs, buzzer, SYS_KILL, ETH_RST. VDD **3V3** |
| **Módulo 2 relés optoacoplado** 5 V | 1 | 1 | OUT1 / OUT2. Activo LOW típico |
| **EstarDyn PC817 2CH** (3,6–30 V) | 1 | 1 | Aísla campo IN1 + IN2 (no agua) |
| **Mini Water Detector Relay** 5/12/24 V | 1 | 1 | Fuga → contacto seco a PCF P00. Alimentar a **5 V** |
| **Buzzer activo 5 V** (2 patas) | 5–10 | **1** | +5V_SYS; − vía Q_BZ (P03→NPN) |
| **TTL↔RS485 auto** (BS, 3V3/5V) | 1 | 1 | Modbus HW. IO12 TX / IO39 RX |
| **FT232** (FTDI232 / FT232RL) breakout | 1 | 1 | Prog UART0. No 5 V USB al WT32 |
| **TPL5010** (módulo o DIP/SOIC breakout) | 1–2 | **1** | WDT ~10 min @ `5V_BUS`. DONE vía NPN |
| **74HCT123** DIP-16 (o HCT123 equivalente) | 2–5 | **1** | One-shot ~2 s → Q1 nuclear |
| SHT31 I2C | 1 | 1 | Humedad (si no está el del lab) |
| OLED 1.3" SH1106 | 1 | 1 | Si no está el del lab |
| DS18B20 waterproof | 4 | 4 | Temps en cadena |
| ZMPT101B | 1 | 1 | 220 V después; no conectar 220 hasta validar |

WT32-ETH01 y A7672SA-FASE: los del lab.

## Semiconductores

| Comprar | Pedir | Usa | Para |
| ------- | ----- | --- | ---- |
| **NDP6020P** TO-220 (P-FET logic). Alt AE: **IRF5305** | 5–10 | **1** | Q1 `5V_SYS` (+módem). **No** IRF4905 |
| **2N3904** NPN TO-92 | 10 | **3** | Q_EN (`BB_EN`) + Q_DONE (TPL DONE) + Q_BZ (buzzer) |
| **1N4148** | pack 50 | **1+** | ETH_RST (PCF P11) → PHY nRST (pad R43) |

## Pasivos y mecánica

| Comprar | Pedir | Usa | Para |
| ------- | ----- | --- | ---- |
| Electrolítico **2200 µF / 16 V** | 10 | 1 | `5V_BUS` C1 |
| Electrolítico **1000 µF / 16 V** | 10 | 1 | A7672 C2 (placa B) |
| Electrolítico **47 µF / 16 V** | 10 | 1 | 74HCT123 timing C17 |
| Electrolítico **10 µF** | 10 | 1 | `5V_SYS` C15 |
| Cerámico / film **2,2 µF** | pack | 1 | `/CLR` blank C18 |
| Cerámico **100 nF** paso 2,54 mm | pack | ver BOM | Desacoples + ADC + ZMPT |
| **REXT** TPL5010 **57,6 kΩ** 1% | 5 | 1 | Intervalo ~10 min (tabla TI) |
| **100 kΩ** 1% | pack | 2+ | HCT123 REXT + TRIG_N/`/CLR` PU |
| 10 kΩ, 15 kΩ, 47 kΩ, 4,7 kΩ, **1 kΩ**, 330 Ω, **1 MΩ** | packs | ver BOM | Gates, I2C, 1-Wire, LEDs, ADC gel, ZMPT, DONE PU |
| Fusible 5×20 **5 A** + porta | 5+1 | 1 | Gel+ |
| LED 5 mm verde / rojo / azul (+ LTE) | 20 c/u | **4** | PWR / NET / ALARMA / **LTE** |
| Pulsador panel NA hundido + táctiles 6×6 | 1+pack | **4** | RESET gabinete + servicio PCF + **BOOT** + **EN** |
| Bornera 5,08 mm | 1 set | — | IN/OUT / RS485 A-B |
| **Bornera 5,08** 4 polos ×2 | 1 set | 2 | J7 / J8 enlace 5 V A↔B. No cinta IDC |
| **Molex KK 2,54** 8p + housing ×2 (o bornera 8) | 1 set | 2 | J9 / J10 señales (IO15, SYS_KILL, BTN) |

## No comprar (para este diseño)

- 74HCT14 / 74HCT125
- CD4541BE (WDT = TPL5010)
- TLC555 / NE555 / 7555
- Segundo NDP6020P / Q3 / FET de módem (módem = `5V_SYS`)
- ULN2003 + relés sueltos / 1N4007 flyback (módulo opto)
- PCF8574 (usar **8575**)
- SB560 / Schottky de potencia (OR = LM66200)
- 2N7000
- ZK-S4, XL4015, XL6009 de un pote, ZK-4KX
- IRF4905 / IRF9540
- AO3401 / SOT-23 suelto
- TXS0102 / shifters I2C
- Cinta IDC-10

Detalle por referencia: `hardware/kicad/cof-v1-bom.csv`.
Especificación: `docs/hardware/HARDWARE_V1.md`.
