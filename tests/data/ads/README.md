# ADS wire-format fixtures

Captured from a live TwinCAT 3 (4026) PLC on 2026-08-23 with `ads_dt_probe`
(see `tools/ads_dt_probe/`), through an SSH tunnel to the TwinCAT VM's AMS
router. Raw, unmodified ADS replies:

| file                 | index group | contents                                  |
|----------------------|-------------|-------------------------------------------|
| `ads_uploadinfo.bin` | `0xF00F`    | 24 B `AdsSymbolUploadInfo2`               |
| `ads_symbols.bin`    | `0xF00B`    | 160 symbol entries, 17240 B                 |
| `ads_datatypes.bin`  | `0xF00E`    | 106 data-type entries, 64656 B             |

These exist because the data-type table's wire layout is **not** described by
the Beckhoff headers this project vendors — no `AdsDatatypeEntry`, no `ADST_*`
codes. A parser written from documentation alone could only be tested against
its own assumptions, so it is tested against these bytes instead.

The capture confirmed the entry layout: eight `uint32` (entryLength, version,
hashValue, typeHashValue, size, offs, dataType, flags) then five `uint16`
(nameLength, typeLength, commentLength, arrayDim, subItems) — 42 bytes — then
the three NUL-terminated strings, then `arrayDim` × (`int32` lower bound,
`uint32` count), then `subItems` nested entries. Parsing consumes exactly
12008 bytes and yields exactly the 106 entries the PLC reported.

The capture includes a `Tc2_MC2.AXIS_REF` (`MAIN.testAxis`) and a small user
structure (`MAIN.testStruct`), which is what exercises the interesting cases:
nested structures, an `ARRAY [0..127] OF DWORD`, and 200-odd `BIT` members —
those last are addressed at index group `0x4041` with the offset in BITS, not
bytes, so recording one at a byte offset silently reads a neighbouring byte.

Nothing here is secret: PLC symbol names and TwinCAT's own library types.
