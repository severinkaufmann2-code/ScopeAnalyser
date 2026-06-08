# TwinCAT Scope View `.svdx` format — reverse-engineering notes

Reverse-engineered from real exports and verified byte-exact against TwinCAT's
own CSV exports of the same recordings (`tests/data/TestScope.svdx` + the BIG
`040626_…svdx` / `TestScope2.csv` pair). This documents the read path; write
is not implemented.

## Container

```
offset 0x00  u64   field A  (varies by export variant; not relied on)
offset 0x08  u64   field B  (varies by export variant; not relied on)
offset 0x10  u32   blockCount
offset 0x14  blockCount × { u64 offset; u64 length; u32 index }   // block index
...                data blocks (each at its index offset/length)
end                XML footer: <ScopeProject AssemblyName="TwinCAT.Measurement.Scope.API.Model">
```

The two header variants (small "newer" file vs large file beginning with magic
`c7 78 9b 26`) store the xml offset/length differently, so the reader does NOT
trust fields A/B. Instead:

- **Blocks**: read `blockCount` at `0x10` and the 20-byte index records at
  `0x14`. The blocks tile the binary region; the last block ends at the start
  of the XML footer.
- **XML footer**: locate by searching the tail for `<ScopeProject`.

## Data block

```
"01.00.00.40"             11 ASCII bytes (version tag)
u64  startFILETIME        block start (100 ns ticks since 1601)
u64  duration
u64  segStartFILETIME
u64  segEndFILETIME        → trim samples past this
... payload sub-header + a min/max "overview" layer (skippable) ...
... full-resolution sub-segments (may start ~100s of KB in, behind overview) ...
```

### Full-resolution sub-segments

```
repeat:
  u64  subsegFILETIME
  u32  sampleCount
  sampleCount × { u32 tsTicks; value }     // value = sizeOf(dataType) bytes
```

- `tsTicks` is the offset from `subsegFILETIME` in **100 ns** units.
- Absolute sample time = `subsegFILETIME + tsTicks`, converted to ns-since-Unix
  epoch (`(filetime − 116444736000000000) × 100`).
- `value` width depends on the channel data type: BIT/BOOL → 1, REAL64 → 8, etc.
  The same `[ts][value]` record layout is used for every type.

The sub-segment chain is found by scanning the block for the first offset whose
`[FILETIME][count][count×rec]` structure validates and repeats. The value size
can be auto-detected (try 8/4/2/1) when the XML type is missing — e.g. a
truncated export with no footer.

## Channel metadata (XML footer)

Recorded channels live in the DataPool as `*Acquisition` elements (e.g.
`<AdsAcquisition>`), each carrying `<Name>`, `<DataType>` (REAL64, BIT, …),
`<SymbolName>`, `<BaseSampleTime>`, `<ScaleFactor>`, `<AmsNetId>`, and an
`<AcquisitionGUID>`. The decorative `<Channel>` elements elsewhere are chart
styling, not data.

## Verified

- `TestScope.svdx` (BIT, 10 ms, 355 samples toggling 0/1) → decodes to exactly
  the values in `TestScope.csv`.
- `040626_…svdx` (REAL64, e.g. `ActVelo` at 2 ms over ~208 s = 104 064 samples)
  → the decoded sequence matches `TestScope2.csv` byte-exact (2000/2000 checked).

## Known gaps / TODO

- **Block → channel mapping** for big multi-channel files is not solved: the
  example has 282 `*Acquisition` elements vs 141 data blocks, and the
  `AcquisitionGUID` does not appear in the block bytes, so names are currently
  positional/approximate. Values decode correctly regardless.
- **Oversampled "8×" blocks** (e.g. a 0.25 ms channel alongside 2 ms ones) use
  the larger block size; the `[ts][value]` decode handles them but this wasn't
  cross-checked sample-for-sample.
- **`TestScope2.svdx` was truncated** (data stops at ~456 MB of 648 MB, no XML
  footer) — incomplete export/copy; use a fully-written file.
- **Write/export** is unimplemented and would need validation by re-opening in
  TwinCAT Scope.
