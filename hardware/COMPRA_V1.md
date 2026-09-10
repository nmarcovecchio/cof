# Listado de compra — CallOnFail v1

Alineado al esquemático `hardware/kicad/` rev 1.2 y a `cof-v1-bom.csv`.
Un prototipo. En AE se piden packs: el proto usa la columna **Usa**.
Todo lo que se suelda a pata es THT (no SOT-23). Los módulos (WT32, A7672,
XY-SJVA, XL6019, PCF, SHT31, OLED) ya vienen con SMD de fábrica.

Gel **6 V 7 Ah** (NP7-6): comprar acá, no China. Fuente **5 V 5 A**: ya está.

## Módulos

| Comprar | Pedir | Usa | Para |
| ------- | ----- | --- | ---- |
| **XY-SJVA** o XY-SJVA-4 CC/CV 3 A 35 W, **dos potes** | 1 | 1 | Carga gel 6,85 V / 0,6 A desde `5V_PSU`. No ZK-S4 |
| Buck-boost auto **XL6019** (no XL6009 de un pote, no ZK-4KX) | 1 | 1 | Backup gel → **5,1 V** |
| PCF8574 módulo I2C | 1 | 1 | 2 IN + 2 OUT + buzzer + LEDs NET/ALARMA |
| ULN2003 + **2 relés 5 V** | 1 | 1 | OUT1 / OUT2 |
| **Buzzer activo 5 V** (2 patas, no pasivo / no 3 V) | 5–10 | **1** | PCF **P3** → ULN O3. Independiente de los relés |
| SHT31 I2C | 1 | 1 | Humedad (si no está el del lab) |
| OLED 1.3" SH1106 | 1 | 1 | Si no está el del lab |
| DS18B20 waterproof | 4 | 4 | Temps en cadena |
| ZMPT101B | 1 | 1 | 220 V después; no conectar 220 hasta validar |

WT32-ETH01 y A7672SA-FASE: los del lab.

## Semiconductores

| Comprar | Pedir | Usa | Para |
| ------- | ----- | --- | ---- |
| **NDP6020P** TO-220 (P-FET logic, Rds a −4,5 V). Alt AE: **IRF5305** | 5–10 | **4** | Q1 `5V_SYS`, Q3 `5V_MODEM`, Q8 OR PSU, Q10 OR backup. Pinout G-D-S. **No** IRF4905 / IRF9540 |
| **2N7000** TO-92 | pack 50 | **3** | Q5 EN backup, Q9 gate Q8, Q11 gate Q10 |
| **74HCT125** DIP-14 | 5–10 | **1** | 3,3 V → 5 V al gate de Q3. Un buffer: 1=OE 2=A 3=Y 7=GND 14=VCC. **No** TXS / BSS138 / “I2C shifter” |
| **TLC555** DIP-8 CMOS (7555 / LMC555 / TS555) | 10 | **1** | One-shot RESET. **No NE555 bipolar** |
| **CD4541BE** DIP-16 | 5–10 | **1** | WDT ~4–5 min |
| **1N4148** | pack 50 | **3** | WDT (D3 D4 D5) |
| **1N4007** | pack 50 | **2** | Flyback relés D8 D9. **No** en el riel de 5 V |

## Pasivos y mecánica

| Comprar | Pedir | Usa | Para |
| ------- | ----- | --- | ---- |
| Electrolítico **2200 µF / 16 V** | 10 | 1 | `5V_BUS` C1 |
| Electrolítico **1000 µF / 16 V** | 10 | 1 | A7672 C2 (placa B) |
| Electrolítico **100 µF** | 10 | 1 | 555 C7 |
| Electrolítico **10 µF** | 10 | 1 | `5V_SYS` C15 |
| Cerámico **100 nF** paso 2,54 mm (no 0805) | pack | **11** | Desacoples + ADC + ZMPT + patada WDT |
| Cerámico **10 nF** paso 2,54 mm | pack | 1 | 555 pin 5 |
| 10 kΩ, 100 kΩ, 47 kΩ, 22 kΩ, 4,7 kΩ, **1 kΩ**, 330 Ω, **1 MΩ** | packs | ver BOM | WDT, gates, I2C, 1-Wire, LEDs, ADC gel |
| Fusible 5×20 **5 A** + porta | 5+1 | 1 | Gel+ |
| LED 5 mm verde / rojo / azul | 20 c/u | 3 | PWR / ALARMA / NET |
| Pulsador panel NA hundido + táctiles 6×6 | 1 | 2 | RESET gabinete + servicio IO39 |
| Bornera 5,08 mm | 1 set | — | IN/OUT campo |
| **IDC-10** hembra ×2 + cinta 10 hilos polarizada | 1 set | 2 | J7 / J8 entre placas |

## No comprar (para este diseño)

- SB560 / Schottky de potencia (el OR es Q8/Q10)
- NE555 bipolar
- ZK-S4, XL4015, XL6009 de un pote, ZK-4KX
- IRF4905 / IRF9540 (no cierran a 5 V)
- AO3401 / 74AHCT1G125 (SOT-23; este proto es todo THT)
- 1N4007 como diodo de 5 V (solo flyback de relé)
- TXS0102 / módulos I2C 3,3↔5 para el módem

Detalle por referencia: `hardware/kicad/cof-v1-bom.csv`.
Especificación: `docs/HARDWARE_V1.md`.
