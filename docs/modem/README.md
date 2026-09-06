# A7672 modem docs (lab)

Local copies for CallOnFail voice work. Do not guess AT behavior — read these first.

Validated module: **A7672SA-FASE** (LatAm: LTE B4/B28, GSM 850/1900).

| File | What it is |
|---|---|
| `A76XX_Series_AT_Command_Manual_V1.12.pdf` | AT commands for A7670/A7672 (call, CLCC, CEER, CCMXPLAY) |
| `A7672X_Series_Hardware_Design_V1.04.pdf` | Pins, audio, UART, power |
| `A7672X_Product_Brief.pdf` | Band / variant matrix |

## Call-control pages to read first (AT manual)

- `ATD` / `ATH` / `AT+CHUP` / `AT+CVHU` — originate and hang up
- `AT+CLCC` / `AT+CLCC=1` — call state URCs (`0` active, `3` alerting, `6` disconnect)
- `AT+CEER` — 3GPP 24.008 release cause (`16` normal, `17` busy, `19` no answer, `21` rejected)
- `VOICE CALL: BEGIN` / `VOICE CALL: END:` — SIMCom URCs (END may have no space)
- `AT+CCMXPLAY` / `AT+CCMXSTOP` / `+AUDIOSTATE` — file playback into the call
- `BUSY` / `NO CARRIER` / `NO ANSWER` — standard result codes

On Claro CSFB (`IMS=0`) this module reports BEGIN / CLCC `0` during ringback.
That is early TCH, not pickup. Event table: [../operators/claro-ar.md](../operators/claro-ar.md).
