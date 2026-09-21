# Roadmap CallOnFail — funciones planificadas

Estado: **2026-09-21**.

Este documento lista funciones que **todavia NO estan implementadas** en el
repo pero que ya se muestran (calificadas como proximas) en el sitio publico
`web/`. Es la referencia para no confundir lo que existe hoy con lo que se va a
construir despues del core.

Regla: mientras una funcion figure aca, **no** se puede vender ni documentar
como capacidad presente. El sitio debe decir "proximamente" / "en desarrollo"
y este archivo es la fuente de verdad del estado.

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
- **Se muestra en:** `web/index.html` ("Historial y reporte PDF"),
  `web/frio/index.html` ("Reporte PDF por rango de fechas para auditoria").
- **Planificado:** si, despues del core. Requiere diseno de reporte por rango
  de fechas y formato para auditoria de cadena de frio.

---

## Como se muestra en el sitio

Las tres funciones se dejan visibles en el sitio con la palabra
**"proximamente"** y sin cambiar la estructura ni los estilos de las secciones.
Si alguna se implementa, actualizar este archivo y sacar el calificador del
sitio en el mismo cambio.
