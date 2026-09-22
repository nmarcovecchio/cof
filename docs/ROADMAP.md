# Roadmap CallOnFail — funciones planificadas

Estado: **2026-09-21**.

Este documento lista funciones que **todavia NO estan implementadas** en el
repo pero que ya se muestran (calificadas como proximas) en el sitio publico
`web/`. Es la referencia para no confundir lo que existe hoy con lo que se va a
construir despues del core.

Regla: mientras una funcion figure aca, **no** se puede vender ni documentar
como capacidad presente. El sitio debe decir "proximamente" / "en desarrollo"
y este archivo es la fuente de verdad del estado.

Este archivo es **solo producto**. Los bugs, la deuda tecnica y el hardening de
proceso (OTA, TLS, alarmas) viven en `docs/ops/BACKLOG.md`.

Hoy el producto si tiene: sensores (temp, humedad, agua, luz/tension),
contactos secos digitales, rele, LTE + bateria, y avisos por llamada / SMS /
Telegram / email con escalamiento (`docs/ops/NOTIFICATIONS.md`).

---

## 1. Modbus RTU/TCP — NO implementado

- **Estado:** planificado, todavia no implementado. Cero codigo de Modbus en
  `firmware/` y en `backend/`.
- **Que existe en su lugar:** solo entradas **digitales** por contacto seco
  (PLC, rele o bornera de alarma) y rele de salida. No hay stack Modbus, ni
  maestro, ni esclavo, ni lectura de registros.
- **Se muestra en:** `web/index.html` ("Modbus RTU/TCP"),
  `web/ot/index.html` ("Lectura Modbus RTU/TCP segun el sitio").
- **Planificado:** si, despues de cerrar el core (instalacion, alarmas y
  escalamiento por llamada). Requiere definir perfiles de registros por sitio.

## 2. SNMP (Zabbix / PRTG) — NO implementado

- **Estado:** planificado, todavia no implementado. Cero agentes, traps o MIBs
  en el repo; `firmware/` y `backend/` no exponen SNMP.
- **Que existe en su lugar:** el equipo reporta telemetria por MQTT y las
  alarmas salen por los canales propios (llamada / SMS / Telegram / email).
  Un NMS externo **no** puede monitorearlo hoy.
- **Se muestra en:** `web/index.html` ("SNMP — Opcional: Zabbix, PRTG o lo que
  ya tengas").
- **Planificado:** si, despues del core. A definir si es agente en el equipo o
  puente SNMP del lado backend.

## 3. Reporte PDF (historial por rango de fechas) — NO implementado

- **Estado:** planificado, todavia no implementado. No hay generacion de PDF
  en `backend/` (ni libreria, ni endpoint, ni boton de export en las
  plantillas).
- **Que existe en su lugar:** la telemetria y el ciclo de avisos se guardan en
  PostgreSQL (`Telemetry`, `Event`) y se consultan en la web (Alarmas y el
  detalle del equipo). Hay historial en pantalla, pero **no** exportable a PDF.
  Desde 2026-09-22 el detalle del equipo tiene **grafico interactivo por rango
  de fechas** y **export CSV** linea a linea (ver mas abajo), pero el PDF sigue
  pendiente.
- **Se muestra en:** `web/index.html` ("Historial y reporte PDF"),
  `web/frio/index.html` ("Reporte PDF por rango de fechas para auditoria").
- **Planificado:** si, despues del core. Requiere diseno de reporte por rango
  de fechas y formato para auditoria de cadena de frio.

---

## Historial de telemetria: grafico y export CSV — implementado 2026-09-22

Ya **no** es un pendiente de roadmap; se deja documentado porque el sitio sigue
prometiendo el reporte PDF.

- **Que hay:** en `/devices/<uid>` la card de Telemetria tiene un grafico
  (Chart.js, CDN) con selector de rango (24 h / 7 d / 30 d o fechas a mano) y un
  boton **Exportar CSV** que baja cada muestra del rango.
- **Series:** salen de los **alias con vigencia** (`SensorWindow`), no de la
  config del equipo. Cada ventana dice que el sensor enchufado en `sensor_id`
  se llamo "Camara A" desde enero hasta septiembre, y "Camara B" despues, leyendo
  `payload_key` todo el tiempo. Ver `backend/app/telemetry_series.py`.
- **Reasignar un sensor (camara A -> camara B):** desde la config, boton
  **Reasignar** en la fila de vigencia. Cierra la ventana anterior y abre una
  nueva con el alias nuevo. Un rango que cruza el cambio devuelve **dos series**
  ("Camara A" y "Camara B"), cada una con datos solo en su tramo, y **nunca** se
  mezclan. Un rango anterior al cambio solo muestra "Camara A"; uno posterior,
  solo "Camara B".
- **Cerrar un sensor:** boton **Cerrar** cuando ya no se va a registrar mas.
  Deja de acumular y deja de graficarse, pero **el historial queda** y sigue
  visible en los rangos en que estuvo activo. Es reversible solo recreando la
  config; el form no reabre una ventana cerrada.
- **Sensor desconectado (sin cerrar):** su serie sigue existiendo y el CSV
  mantiene la columna, vacia en el periodo sin datos. La UI lo marca como
  "sin datos en el rango". Cuando se reconecta, la misma linea vuelve a
  llenarse.
- **Linea de 220V:** el firmware publica `mains_voltage` en `null`
  (`firmware/src/mqtt_io.cpp`), asi que la ventana de `mains_1` apunta a
  `zmpt_raw` y se grafica rotulado como "ADC crudo, sin calibrar". Cuando el
  firmware calcule RMS, alcanza con reasignar el `payload_key` a
  `mains_voltage`.
- **Backend:** `GET /devices/<uid>/telemetry.json` (grafico, con tope de rango y
  promedio por bucket) y `GET /devices/<uid>/telemetry.csv` (sin downsampling,
  exporta todo). Cierre y reasignacion por `POST`. Todo requiere sesion.
- **Migracion:** `ensure_sensor_windows()` en `init_db.py` crea una ventana
  abierta por sensor configurado en los equipos que no tienen ninguna,
  arrancando en la primera telemetria guardada. Es idempotente.
- **Lo que sigue pendiente:** el PDF, la retencion de telemetria, y el
  **borrado de historial por alias** (que depende de estas ventanas: borrar el
  historial de "Camara A" = borrar el crudo dentro de `[starts_at, ends_at)`,
  que es lo unico que lo separa del historial de "Camara B"). Ver
  `docs/ops/BACKLOG.md`.

---

## Como se muestra en el sitio

Las tres funciones se dejan visibles en el sitio con la palabra
**"proximamente"** y sin cambiar la estructura ni los estilos de las secciones.
Si alguna se implementa, actualizar este archivo y sacar el calificador del
sitio en el mismo cambio.
