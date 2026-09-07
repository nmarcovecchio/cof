# Hardware v1 — CallOnFail

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

Batería: **gel 12 V ~4 Ah** (sellada). Más simple que LiFePO4: fuente de flote + buck a 5 V.
Usar ~la mitad de la capacidad. Cargador **perfil gel** (flote ~13,5–13,8 V; no 14,7 V de auto).

El supervisor vive en el riel de 12 V (fuente o gel). GND común.

```text
220 VAC
  → fuente / cargador gel (flote 13,5–13,8 V)
       ├─ gel 12 V 4 Ah
       ├─ Supervisor WDT
       ├─ MOSFET + buck  →  5V_SYS    WT32, OLED, SHT31, PCF, DS18B20
       └─ MOSFET + buck  →  5V_MODEM  solo A7672 VCC

ZMPT en la 220: avisa “se cortó la luz” (el 12 V sigue por la gel).
```

Se corta la 220 → el gel ya está en ese riel. Ethernet off, LTE si hace falta, ~4 h.

Si no hay patada de WDT ~3–5 min, o si alguien aprieta el **RESET del gabinete**:
cortar `5V_SYS` ~2 s y reponer (la gel no se desconecta). El PHY LAN8720 no tiene
reset por GPIO; un pulso a `EN` del ESP32 **no** alcanza.

Durante una llamada hay que seguir pateando. Timeout > llamada + TTS.

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

## 9. WDT externo (v1)

```text
IO15  →  patada (DONE / WDI del supervisor)
```

Patear con red sana y durante llamadas. IO15 es strapping: no dejarlo a GND en el reset del ESP32.

El botón RESET del gabinete entra al **mismo** corte de `5V_SYS` que el timeout del WDT. Así funciona aunque el firmware esté muerto.

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

**Adentro:** WT32, A7672, OLED, SHT31, PCF, ZMPT, WDT, MOSFETs, botón de servicio (IO39), USB-serial de fábrica.
