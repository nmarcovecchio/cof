# CallOnFail firmware

Primer firmware base para CallOnFail usando:

- WT32-ETH01 / WT32-S1 con Ethernet.
- WiFi opcional por credenciales cargadas desde Serial.
- OLED I2C 1.3" SH1106.
- SHT31 por I2C.
- DS18B20 por OneWire.
- PCF8574 por I2C para boton/DIP switch.
- Modem A7672 por UART TTL.
- OTA por Ethernet desde `actual_version/manifest.json`.
- Audio de prueba descargado por Ethernet y cargado al modem con `AT+CFTRANRX`.

## Backend MVP con Docker

El repo incluye una base deployable para VPS:

- Flask + Gunicorn.
- PostgreSQL.
- Redis.
- Mosquitto MQTT interno.
- Worker MQTT Python.
- Caddy como reverse proxy.

La bitacora/configuracion persistente del VPS queda en:

```text
docs/VPS_CONFIG.md
```

El contrato inicial para configuracion de dispositivos queda en:

```text
docs/DEVICE_CONFIG_V1.md
```

### Deploy inicial en VPS

En el VPS, despues de instalar Docker:

```bash
sudo mkdir -p /opt/callonfail
sudo chown -R ubuntu:ubuntu /opt/callonfail
git clone https://github.com/nmarcovecchio/cof.git /opt/callonfail
cd /opt/callonfail
cp .env.example .env
nano .env
```

Si estas probando una rama antes de mergearla a `main`:

```bash
git clone -b NOMBRE_DE_RAMA https://github.com/nmarcovecchio/cof.git /opt/callonfail
```

Para una primera prueba sin dominio/TLS, dejar:

```text
APP_DOMAIN=:80
```

Cuando `app.callonfail.com.ar` apunte al VPS, cambiar:

```text
APP_DOMAIN=app.callonfail.com.ar
ACME_EMAIL=tu-email
```

Levantar servicios:

```bash
docker compose up -d --build
```

Ver estado:

```bash
docker compose ps
docker compose logs -f web
docker compose logs -f mqtt-worker
```

Probar desde el VPS:

```bash
curl http://localhost/health
curl http://localhost/api/status
```

Probar desde navegador:

```text
http://IP_DEL_VPS/
```

o, si ya configuraste DNS:

```text
https://app.callonfail.com.ar/
```

### Comandos Docker utiles

```bash
docker compose up -d
docker compose up -d --build
docker compose ps
docker compose logs -f
docker compose logs -f web
docker compose restart web
docker compose down
```

### MQTT MVP

Mosquitto escucha internamente en `1883` y Docker lo publica solo en
`127.0.0.1:1883`, para que no quede abierto a internet sin TLS.

Para probar desde el VPS:

```bash
docker compose exec mosquitto mosquitto_pub -h localhost -t devices/cof-test/telemetry -m '{"temperature":25.1}'
docker compose logs -f mqtt-worker
```

Antes de conectar equipos reales desde internet hay que agregar autenticacion y
TLS en `8883`.

### Prueba ESP32 -> VPS por MQTT

Para una prueba corta de laboratorio, se puede exponer MQTT sin TLS en `1883`.
No dejar esto asi para produccion.

En `.env` del VPS:

```text
MQTT_BIND_ADDRESS=0.0.0.0
```

Recrear servicios:

```bash
docker compose up -d --build
```

Abrir temporalmente el puerto en UFW:

```bash
sudo ufw allow 1883/tcp
```

Tambien abrir `1883/tcp` en el firewall de Lightsail/AWS.

En el ESP32, desde PlatformIO Monitor:

```text
mqtt mqtt.callonfail.com.ar 1883 cof-test
```

O usando la IP publica:

```text
mqtt IP_DEL_VPS 1883 cof-test
```

Publicar telemetria manual:

```text
pub
```

Ver logs en el VPS:

```bash
docker compose logs -f mqtt-worker
```

Luego refrescar:

```text
http://IP_DEL_VPS/dashboard
```

Para probar configuracion web -> ESP32 -> ACK:

```text
http://IP_DEL_VPS/devices/cof-test
```

Entrar a `Editar/publicar configuracion`, guardar el JSON y esperar que el
dispositivo reporte la misma version en `Config deseada/reportada`.

Cuando termine la prueba, cerrar `1883`:

```bash
sudo ufw delete allow 1883/tcp
```

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

## Comandos por Serial

Abrir PlatformIO Monitor a `115200`. El firmware acepta comandos para probar
hardware sin recompilar:

```text
h      ayuda
s      imprimir estado general
i      escanear bus I2C
t      leer sensores ahora
m      reinicializar modem
o      forzar chequeo manifest/OTA
a      forzar chequeo manifest/audio
c      hacer llamada de prueba si llamadas estan habilitadas
r      reiniciar ESP32
wifi SSID PASSWORD  guardar WiFi y conectar
wifi-clear          borrar WiFi guardado
wifi-status         ver estado WiFi
mqtt HOST PORT DEVICE_ID [USER PASSWORD]
mqtt-clear          borrar MQTT guardado
mqtt-status         ver estado MQTT
pub                 publicar telemetria ahora
AT...  enviar comando AT crudo al modem
```

Ejemplos:

```text
i
s
AT
AT+CPIN?
AT+CSQ
AT+CCMXPLAY=?
AT+CFTRANRX=?
```

Para cargar WiFi desde Serial:

```text
wifi MiRed MiPassword
```

El firmware guarda SSID/password en flash y vuelve a conectarse despues de un
reset. Para borrar:

```text
wifi-clear
```

Nota: por ahora el comando simple no soporta espacios en el SSID o password.

Para configurar MQTT desde Serial:

```text
mqtt IP_DEL_VPS 1883 cof-test
mqtt-status
pub
```

La build de laboratorio trae por defecto:

```text
MQTT host: mqtt.callonfail.com.ar
MQTT port: 1883
Device ID: cof-test
```

Si no hay otra configuracion MQTT guardada en el ESP32, intenta conectar ahi
automaticamente.

El firmware tambien habilita el watchdog interno del ESP32 con timeout amplio
para pruebas. Si el programa se cuelga, el ESP32 deberia reiniciarse solo.

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

## OTA por Ethernet o WiFi

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

Los equipos instalados descargaran el firmware por Ethernet o WiFi cuando vean
una version mayor en el manifest. Ethernet queda como red principal; WiFi sirve
como fallback y para pruebas.

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
