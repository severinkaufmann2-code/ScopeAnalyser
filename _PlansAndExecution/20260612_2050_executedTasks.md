# Executed tasks — 2026-06-12 20:50 session

## Repeat(signal, n): tile a periodic slice for sharper FFT peaks

Problem: FFT peaks read imprecisely when the analysed range holds only a
few periods (coarse bin grid, smeared peaks). The signal is periodic, so
the user wanted "FFT from Xs to Ys with a number of rounds". Design
(user-approved): the Xs–Ys half already existed as Slice, so the missing
piece became a standalone composable Repeat(signal, n) — not an FFTWelch
argument — matching the Slice/Gate pipeline idiom:
FFT(Repeat(Slice(x, t0, t1), n)).

Semantics: copies continue the signal's own sample grid (copy m offset by
span + dt, so seam spacing == internal spacing); first copy keeps its
original timestamps; unit/origin (sourceStartNs) pass through; rejects
frequency-domain input, non-integer/zero n, and results > 50 M samples.
Honesty documented in the help text: repetition adds NO new information —
it puts the spectrum on an n× finer grid and narrows the peaks; the slice
must cover a WHOLE number of periods or seams add false harmonics; PeakHz
remains the direct tool for a precise single peak frequency. Watch out:
Slice bounds are inclusive, so one period of 10 Hz at 1 kHz is
Slice(x, 0.1, 0.199), not 0.2 (one extra sample = seam artefact).

Touched: FunctionRegistry.cpp (impl_Repeat + registration next to Slice),
AddChannelDialog.cpp (preview example: pulse ×3). +5 tests in
test_function_correctness.cpp (seam-true tiling, on-bin seam-free
spectrum 128×8 → exact bin energy, Slice→Repeat→PeakHz end-to-end,
origin/timestamp preservation, arg rejection). 217/217 green;
ScopeAnalyser app target relinked.
