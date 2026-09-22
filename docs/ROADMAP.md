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
- **Series:** se derivan de `cfg.sensors` del equipo. Cada sensor tiene un
  **alias** (`name`, lo que ve el operador) y un **`payload_key`** (la clave del
  frame del firmware donde esta el valor, ej. `humidity`, `temperature_1`,
  `zmpt_raw`). Ver `backend/app/telemetry_series.py`.
- **Sensor desconectado:** el alias es la identidad estable del sensor. Si un
  sensor deja de reportar, su fila sigue en la config, asi que **la linea y la
  columna del CSV siguen existiendo** - vacias en ese periodo - y cuando se
  reconecta la misma serie vuelve a llenarse. No hay que reconfigurar nada y
  ningun rango historico pierde una columna. La UI marca el sensor como
  "sin datos en el rango" cuando no reporto nada en el periodo pedido.
  El boton de quitar sensor solo lo saca de la config: **no borra historial**.
- **Linea de 220V:** el firmware publica `mains_voltage` en `null`
  (`firmware/src/mqtt_io.cpp`), asi que `mains_1` apunta a `zmpt_raw` y se
  grafica rotulado como "ADC crudo, sin calibrar". Cuando el firmware calcule
  RMS, alcanza con cambiar el `payload_key` del sensor a `mains_voltage`.
- **Backend:** `GET /devices/<uid>/telemetry.json` (grafico, con tope de rango y
  promedio por bucket) y `GET /devices/<uid>/telemetry.csv` (sin downsampling,
  exporta todo). Ambos requieren sesion.
- **Lo que sigue pendiente:** el PDF propiamente dicho, y la retencion de
  telemetria (ver `docs/ops/BACKLOG.md`).

---

## Como se muestra en el sitio

Las tres funciones se dejan visibles en el sitio con la palabra
**"proximamente"** y sin cambiar la estructura ni los estilos de las secciones.
Si alguna se implementa, actualizar este archivo y sacar el calificador del
sitio en el mismo cambio.
