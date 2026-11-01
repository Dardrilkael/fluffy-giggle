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
| `bjt_early.cir` | `.dc` | Early effect (Vaf=50): Ic(Vce) is a ramp with slope Ic0/Vaf — the finite output resistance ro = Vaf/Ic |

## M4b — junction capacitance (Cjo / Cje / Cjc)

| File | Analysis | What it shows |
|---|---|---|
| `diode_ac_cjo.cir` | `.ac` | Reverse-biased diode as a cap; RC low-pass cutoff at fc = 1/(2πRC) |
| `bjt_ac_cje.cir` | `.ac` | BJT Cje cap; base low-pass with the source resistance |

## M4c — the MOSFET (level 1, Shichman-Hodges)

| File | Analysis | What it shows |
|---|---|---|
| `mos_saturation.cir` | `.op` | NMOS saturation: Id = ½·Kp·W/L·(Vgs−Vth)²·(1+λVds) = 11.0 µA (matches ngspice 39 exactly) |
| `mos_iv.cir` | `.dc` | Id vs Vds — the triode-to-saturation knee |
| `mos_cutoff.cir` | `.op` | Vgs < Vth — Id ≈ 0 |

## M4d — the behavioral B source (dual-number AD)

| File | Analysis | What it shows |
|---|---|---|
| `b_multiplier.cir` | `.dc` | `V=2*v(in)` — an exact gain-of-2 buffer at every sweep point |
| `b_exp.cir` | `.op` | `I=10f*(exp(v(a)/25.85m)-1)` — the B source as a diode; converges to the Lambert-W point |
| `b_tanh_limit.cir` | `.tran` | `I=1m*tanh(v(p,n)/0.5)` — a soft limiter over a differential ref, `time`-swept |

## M4e — the lossless transmission line (method of characteristics)

| File | Analysis | What it shows |
|---|---|---|
| `t_matched_delay.cir` | `.tran` | Rs = RL = Z0: the 1 ns ramp reappears at the far end exactly 2 ns later, no reflections |
| `t_open_reflect.cir` | `.tran` | Open far end: the wave doubles at the open, the reflection returns at 2·Td and settles to the DC wire (v = V, i = 0) |
| `t_ringing.cir` | `.tran` | RL = 3·Z0 with a matched source: one round trip to the DC wire value, no ringing |
| `t_quarter_wave.cir` | `.ac` | Open 1 µs line: input short at odd quarter-waves, open at half-waves |

## ngspice cross-validation

The `.op` examples produce values that match ngspice 39 to 5+ significant
figures. To compare:

```sh
ngspice -b <deck_with_control_block>.cir    # ngspice
./build/linux/bin/spice run <deck>.cir       # ws::spice
```

(ngspice needs a `.control`/`.endc` wrapper to run in batch mode; the
ws::spice decks use `.op`/`.dc`/`.tran`/`.ac` cards directly.)
