# Claro Argentina (`722310`)

Validated on `cof-test`: WT32-ETH01 + SIMCom **A7672SA-FASE**, firmware
through **0.2.39**. Traces of 2026-09-06 unless noted.

Related: [VOICE_SMS.md](../VOICE_SMS.md), [modem docs](../modem/README.md).

## Identidad y bearer

| Dato | Valor |
|---|---|
| Operador | Claro Argentina |
| MCC/MNC | `722310` (`AT+COPS` numeric) |
| APN datos | `internet.claro.com.ar` / `clarogprs` / `clarogprs777` |
| SMSC (SIM) | `+5491115030500` — no pisar; un SMSC forzado ya rompió SMS |
| Idle radio | `AT+CNMP=2` (automático) → LTE **B4** (AWS), CSQ alto |
| Voz IMS | **No registra.** `AT+CAVIMS=1` / `AT+CIREG` → `ims_reg=0` |
| Voz usable | **CSFB**: LTE + `CREG=1`, sin IMS |
| 2G en este sitio | **No.** `AT+CNMP=13` (GSM only) → `NO SERVICE` |

Config en firmware: `include/cof_config.h` (`COF_MODEM_APN*`, `COF_MODEM_SMSC`).
Voz-céntrico: `AT+CEMODE=1`, `AT+CEVDP=3`, `AT+CAVIMS=1`.

SMS funciona cuando el módem está asentado (`CEREG=1`, CSQ alto, LTE B4).
Después de un CSFB hay que restaurar LTE + `CGSMS` / `CMGF` o el SMS queda
muerto.

## Cómo se arma una llamada (no es VoLTE)

El primer `ATD` sobre LTE viejo / sin CS listo suele devolver `NO CARRIER`.
Eso **no** es “falló la llamada”: el dominio CS no estaba listo.

Secuencia que sí funciona (0.2.27+):

1. Bajar TTS AMR (solo test call).
2. `AT+CFUN=4` → `AT+CFUN=1` (rebote RF).
3. Esperar `CREG` / radio / `CPAS` idle / CSQ.
4. Un solo `ATD+54…;` (punto y coma = voz).
5. Ignorar el `NO CARRIER` temprano de CSFB (~40 s) **hasta** que haya ringing.
6. `AT+CLCC=1` (URC, **sin** polls `AT+CLCC` / `AT+CPSI` durante la llamada).

`AT+CNMP=13` como primer intento no sirve en este sitio.

## Señal de “conectado” que miente

En cuanto hay ringback, el A7672 en Claro manda **todo esto a la vez**:

```text
+CLCC: 1,0,2,…     dialing
+CLCC: 1,0,3,…     alerting / ringing
VOICE CALL: BEGIN
+CLCC: 1,0,0,…     active
+COLP: "549…",161
```

`CLCC` 0, `BEGIN` y `+COLP` **no** significan que contestaron. Es asignación
temprana de TCH (ringback). Las tres pruebas “nadie tocó / rechazó /
contestó y esperó” son **idénticas** en el UART hasta que alguien cuelga de
verdad.

## Eventos (qué manda Claro)

Fechas en `-03`. Destino de lab `+5491168619589`. Firmware de las trazas:
**0.2.38** (sin play, para ver la red).

### Setup / ringing (todas las llamadas)

| URC / AT | Qué es |
|---|---|
| `ATD+54…;` → `OK` | El módem aceptó marcar, no que sonó |
| `+CLCC` stat **2** | Dialing |
| `+CLCC` stat **3** | Alerting → evento `Ringing` |
| `VOICE CALL: BEGIN` | Camino de voz CS arriba |
| `+CLCC` stat **0** | “Active” falso (ringback) |
| `+COLP:` | Línea conectada falsa |
| `NO CARRIER` en los primeros ~40 s **sin** ringing | CSFB arrancando; no colgar |

### No atiende (2026-09-06 13:52)

Nadie tocó el teléfono. ~65 s después del ring:

```text
>> AT+CLCC
<< +CLCC: 1,0,0,0,0,"+5491168619589",145
>> ATH
<< +CEER: "0 Unknown"
<< +CLCC … 6 / NO CARRIER / VOICE CALL: END
```

La llamada **sigue arriba** hasta que colgamos nosotros. No hay `BUSY`,
`NO ANSWER` de red, ni CEER 18/19. `CEER=0` es nuestro `ATH`.

**Evento:** `Call no answer`.

### Rechaza (2026-09-06 13:57)

El celular declinó. Traza **igual** a no atender: a los ~62 s `AT+CLCC`
sigue en stat **0**. No hay `BUSY` ni CEER 17/21.

El reject del otro lado (casi seguro VoLTE / SIP 486/603) **no** se traduce
al CSFB. El timbre local se apaga; del lado nuestro el canal sigue.

**Evento:** `Call no answer` (no hay señal de reject).
`Call rejected` solo si algún día aparece `BUSY` / CEER 17/21.

Esto no es un URC perdido: la prueba “atendí y corté a los 4 s” sí mandó
`CLCC` 6 al instante. El listener funciona.

### Contesta y espera a que cortemos (2026-09-06 14:00)

Misma traza que no-atender. Sin `CCMXPLAY` en 0.2.38 no hubo TTS. El
equipo esperó el timeout (~58 s, `END:000058`) y `ATH`. `CEER=0`.

**Evento en esa captura:** `Call no answer` (correcto para 0.2.38, que no
reproducía). En 0.2.39+ debería ser `Call done` al terminar el audio.

### Contesta y cuelga del otro lado (~4 s, 2026-09-06 14:04)

Única señal remota real que vimos:

```text
+CLCC 2 → 3 → 0 + BEGIN + COLP
+CLCC: 1,0,6,…
NO CARRIER
VOICE CALL: END: 000004
+CEER: "16 Normal call clearing"
```

**Evento:** `Call done` + `Remote hangup`.

`END:NNNNNN` es duración en segundos. `000004` = 4 s.

## Cómo marcar (firmware 0.2.39+)

| Situación | Cómo se sabe | Evento |
|---|---|---|
| Audio terminó | `+AUDIOSTATE: play stop` → 2 s → `ATH` | `Call done` |
| El otro colgó (había ringing) | `CLCC` 6 / `NO CARRIER` / `VOICE CALL:END` / CEER 16 | `Call done` |
| Timeout y `AT+CLCC` todavía lista la llamada | Nadie liberó el CS | `Call no answer` |
| `BUSY` o CEER 17/21/22 | Nunca visto en este lab | `Call rejected` |

No clasificar por segundos de audio. No tratar `CLCC` 0 / `BEGIN` / `+COLP`
como “contestó”.

## Audio en la llamada (0.2.39)

Claro no avisa el pickup, así que se reproduce **en el canal de ringback**
(el que ya está “active”):

1. 4 s de silencio después de `BEGIN` / `CLCC` 0 (atender y poner el oído).
2. `AT+CCMXPLAY="C:/tts.amr",1,0` (path remoto = al aire).
3. 2 s de silencio después de `play stop`.
4. `ATH`.

Si el otro cuelga antes, `CLCC` 6 gana y es `Call done`.

No hacer `flush` del UART ni `AT+CLCC`/`AT+CPSI` periódicamente: se comen
`BUSY` / `END` / `play stop`. El A7672 a veces manda `VOICE CALL:END:`
sin espacio.

## SMS

Cuando LTE está asentado: `CMGF=1`, SMSC de la SIM, `CMGS` → evento
`SMS sent`. Un CSFB que deja el radio en GSM/`NO SERVICE` rompe el SMS
hasta restaurar `CNMP=2` + attach.

## Cosas que no hay que “arreglar” sin otra prueba en vivo

- Forzar GSM (`CNMP=13`) como voz primaria en este sitio.
- Tratar IMS/`CAVIMS` como VoLTE listo (`ims_reg` sigue en 0).
- Inventar `Call rejected` por duración corta de ring.
- Polls AT durante la llamada “para ver si contestó”.
- Cambiar el SMSC compilado.
