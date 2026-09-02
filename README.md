# CallOnFail firmware

Primer firmware base para CallOnFail usando:

- WT32-ETH01 / WT32-S1 con Ethernet.
- OLED I2C 1.3" SH1106.
- SHT31 por I2C.
- DS18B20 por OneWire.
- PCF8574 por I2C para boton/DIP switch.
- Modem A7672 por UART TTL.
- OTA por Ethernet desde `actual_version/manifest.json`.
- Audio de prueba descargado por Ethernet y cargado al modem con `AT+CFTRANRX`.

## IDE recomendado

Usa **Visual Studio Code + PlatformIO**.

1. Instalar VS Code.
2. Instalar la extension "PlatformIO IDE".
3. Abrir esta carpeta del repo en VS Code.
4. Editar `include/cof_config.h`.
5. Conectar el programador USB-Serial.
6. En PlatformIO, ejecutar:
   - `Build`
   - `Upload`
   - `Monitor`

Alternativa por consola:

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

## Configuracion antes de llamar

Por seguridad, las llamadas estan deshabilitadas por defecto.

En `include/cof_config.h`:

```cpp
#define COF_ENABLE_CALLS 0
#define COF_PHONE_NUMBER "+549XXXXXXXXXX"
```

Para probar llamadas:

```cpp
#define COF_ENABLE_CALLS 1
#define COF_PHONE_NUMBER "+549TU_NUMERO"
```

Tambien se puede publicar el numero en `actual_version/manifest.json`, pero para la
primera prueba conviene dejarlo fijo en el firmware.

## Pines WT32

### Alimentacion

Usar una sola entrada de alimentacion en el WT32:

```text
Fuente PC 5V  -> WT32 5V
Fuente PC GND -> WT32 GND
```

No alimentar el WT32 por 5V y 3V3 al mismo tiempo.

### Programacion inicial

Usar el header de programacion/debug:

```text
USB-Serial TX  -> WT32 RXD0
USB-Serial RX  -> WT32 TXD0
USB-Serial GND -> WT32 GND
WT32 IO0       -> GND solo para subir firmware
```

Secuencia:

1. Poner `IO0` a GND.
2. Reset/EN.
3. Subir firmware.
4. Sacar `IO0` de GND.
5. Reset/EN.

El USB-Serial debe usar logica de 3.3V.

### I2C

```text
WT32 IO32 / CFG    -> SDA
WT32 IO33 / 485_EN -> SCL
```

Conectar al mismo bus:

```text
OLED SH1106 VCC -> 3V3
OLED SH1106 GND -> GND
OLED SH1106 SDA -> IO32
OLED SH1106 SCL -> IO33

SHT31 VCC -> 3V3
SHT31 GND -> GND
SHT31 SDA -> IO32
SHT31 SCL -> IO33

PCF8574 VCC -> 3V3
PCF8574 GND -> GND
PCF8574 SDA -> IO32
PCF8574 SCL -> IO33
```

Boton de prueba:

```text
PCF8574 P0 -> boton -> GND
```

Pulsacion corta: llamada de prueba.

Pulsacion larga, mas de 3 segundos: forzar chequeo OTA/manifest.

### DS18B20

```text
DS18B20 VDD  -> 3V3
DS18B20 GND  -> GND
DS18B20 DATA -> WT32 IO14
4.7k entre DATA y 3V3
```

### ZMPT101B

Para mas adelante:

```text
ZMPT101B OUT -> WT32 IO36
```

No conectar 220V hasta validar el resto del sistema.

### A7672

Alimentar el modem con 5V de la fuente de PC:

```text
Fuente PC 5V  -> A7672 VCC
Fuente PC GND -> A7672 GND
WT32 GND      -> A7672 GND comun
```

UART:

```text
A7672 TXD -> WT32 RXD / GPIO5
A7672 RXD -> WT32 TXD / GPIO17
```

Velocidad inicial:

```text
115200
```

El firmware prueba:

```text
AT
ATE0
ATI
AT+CPIN?
AT+CSQ
AT+CCMXPLAY=?
AT+CFTRANRX=?
```

## OTA por Ethernet

El ESP32 consulta:

```text
https://raw.githubusercontent.com/nmarcovecchio/cof/main/actual_version/manifest.json
```

Para publicar una nueva version:

1. Actualizar `COF_FIRMWARE_VERSION` en `include/cof_config.h`.
2. Compilar:

   ```bash
   pio run
   ```

3. Copiar el binario:

   ```bash
   cp .pio/build/wt32-eth01/firmware.bin actual_version/firmware.bin
   ```

4. Actualizar `actual_version/manifest.json` con la nueva version.
5. Commit + push.

Los equipos instalados descargaran el firmware por Ethernet cuando vean una version
mayor en el manifest.

## Audio de llamada

El archivo inicial es:

```text
actual_version/audio/cof_test.wav
```

Formato:

```text
WAV PCM, 8000 Hz, mono, 16-bit
```

El firmware lo sube al modem como:

```text
AT+CFTRANRX="C:/cof_test.wav",<bytes>
```

Y durante la llamada lo reproduce hacia el remoto:

```text
AT+CCMXPLAY="C:/cof_test.wav",1,0
```

## Pantalla OLED

La pantalla muestra:

- Version de firmware.
- IP Ethernet.
- Temperatura/humedad SHT31.
- Temperatura DS18B20.
- ADC crudo del ZMPT101B.
- Estado modem/LTE.
- Estado actual: OTA, llamada, audio, errores.

## Si el OLED queda apagado

Checklist rapido:

1. Despues de subir firmware, sacar `IO0` de GND y resetear. Si `IO0`
   queda a GND, el WT32 queda en modo programacion y no corre el programa.
2. Confirmar que el OLED este conectado a los pines configurados:

   ```text
   OLED SDA -> WT32 IO32 / CFG
   OLED SCL -> WT32 IO33 / 485_EN
   OLED VCC -> WT32 3V3
   OLED GND -> WT32 GND
   ```

3. No usar GPIO21/GPIO22: en este firmware el bus I2C esta en IO32/IO33.
4. Probar invertir SDA/SCL si hay duda de serigrafia.
5. Abrir Monitor a 115200 y buscar:

   ```text
   [i2c] scan SDA=32 SCL=33
   [i2c] found device at 0x3C
   ```

   o:

   ```text
   [i2c] no devices found
   ```

6. Si no aparece ningun dispositivo I2C, revisar alimentacion, GND comun,
   SDA/SCL y soldaduras del pin header.
