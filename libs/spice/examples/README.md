# ws::spice — example decks

Each `.cir` file is a self-contained SPICE deck runnable with the CLI:

```sh
./build/linux/bin/spice run <example>.cir          # CSV to stdout
./build/linux/bin/spice run <example>.cir -o out.csv  # CSV to file
```

On Windows: `.\build\spice-debug\bin\spice.exe run <example>.cir`

## M3 — the diode (Shockley + pnjlim)

| File | Analysis | What it shows |
|---|---|---|
| `diode_op.cir` | `.op` | Diode + series R; Vd ≈ 0.69 V, I ≈ 4.3 mA (the Lambert-W closed form) |
| `diode_iv.cir` | `.dc` | Diode I-V curve (0 → 0.8 V); the exponential knee at ~0.6 V |
| `diode_rectifier.cir` | `.tran` | Half-wave rectifier + RC filter; cap charges to ~Vpeak−Vd |
| `diode_stiff.cir` | `.op` | Diode at 100 V — pnjlim prevents exp() overflow; converges to ~0.7 V |

## M4a — the BJT (Ebers-Moll transport model)

| File | Analysis | What it shows |
|---|---|---|
| `bjt_op.cir` | `.op` | NPN forward-active; Ic = Is·exp(Vbe/Vt) ≈ 57 µA, β = 100 |
| `bjt_iv.cir` | `.dc` | BJT Ic vs Vbe — the 60 mV/decade exponential |
| `bjt_amplifier.cir` | `.op` | Common-emitter with emitter degeneration; proper quiescent bias |
| `bjt_pnp.cir` | `.op` | PNP — polarity flips, same magnitudes |

## M4b — junction capacitance (Cjo / Cje / Cjc)

| File | Analysis | What it shows |
|---|---|---|
| `diode_ac_cjo.cir` | `.ac` | Reverse-biased diode as a cap; RC low-pass cutoff at fc = 1/(2πRC) |
| `bjt_ac_cje.cir` | `.ac` | BJT Cje cap; base low-pass with the source resistance |

## ngspice cross-validation

The `.op` examples produce values that match ngspice 39 to 5+ significant
figures. To compare:

```sh
ngspice -b <deck_with_control_block>.cir    # ngspice
./build/linux/bin/spice run <deck>.cir       # ws::spice
```

(ngspice needs a `.control`/`.endc` wrapper to run in batch mode; the
ws::spice decks use `.op`/`.dc`/`.tran`/`.ac` cards directly.)
