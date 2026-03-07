# KR-106 DSP Architecture

How the Ultramaster KR-106 models the Roland Juno-106 signal chain.

---

## Signal Flow

```
Per Voice (x6):
  Oscillators (Saw + Pulse + Sub + Noise)
    -> VCF (4-pole TPT ladder with OTA saturation)
    -> VCA (ADSR or Gate envelope)

Global:
  Sum of 6 voices
    -> HPF (4-position switch)
    -> Stereo Chorus (BBD emulation)
    -> Master Volume
    -> Stereo Output
```

---

## Oscillators (`Source/DSP/KR106Oscillators.h`)

All oscillators use **PolyBLEP** anti-aliasing to smooth discontinuities.

### Saw

Models the Juno's capacitor-ramp DCO. A current source charges a capacitor linearly,
then a comparator resets it. Two hardware characteristics are emulated:

- **Charge curvature** — finite output impedance causes the ramp to bow upward:
  `pos * (1 + 0.15 * (1 - pos))`. Steeper at the start, flatter near reset.
- **Discharge blip** — brief undershoot after reset (amplitude 0.1, decay 0.5/sample)
  as the capacitor briefly undershoots before the current source recovers.

### Pulse

Comparator on the curved saw ramp. The effective pulse width compensates for the
curvature so the crossing point matches a linear-phase expectation:
`effPW = pw / (1 + 0.15 * (1 - pw))`. Both rising and falling edges are PolyBLEP'd.

### Sub Oscillator

CD4013 flip-flop toggled at each saw reset, producing a square wave one octave below.
Transitions are PolyBLEP-smoothed, then passed through a passive RC lowpass
(`coeff = 0.45`, ~4.2 kHz at 44.1k) modeling the hardware's gentle rolloff.

### Noise

LCG PRNG with spectral-tilt lowpass (~8 kHz, coeff 0.7) to approximate the
pink-ish noise spectrum of the Juno's noise generator.

### Mixing

```
out = saw * 0.5 * sawGain
    + pulse * 0.5 * pulseGain
    + sub * 0.67 * subLevel * subGain
    + noise * noiseAmp
```

Waveform switch gains ramp at 1/64 per sample (~1.5 ms at 44.1k) to prevent clicks.

---

## VCF — 4-Pole Ladder Filter (`Source/DSP/KR106Voice.h`)

**Topology:** Four cascaded 1-pole TPT (Trapezoidal Permanent Topology) integrators
with OTA-saturated feedback, 2x oversampled. This models the IR3109 VCF chip.

### Why TPT?

The integrator states are coefficient-independent: cutoff can change every sample
without clicks or instability. This is critical for envelope and LFO modulation.

### 2x Oversampling

The filter runs at double the audio sample rate using a 12-coefficient allpass
polyphase halfband IIR (same algorithm and coefficients as Laurent de Soras' HIIR
library). The input is upsampled, two filter iterations run per audio sample, and
the output is downsampled. This extends the filter's clean operating range and
reduces aliased harmonics from the nonlinear stages, especially at high cutoff
and resonance.

### Per-Stage OTA Nonlinearity

The IR3109's four stages each contain an OTA differential pair. The transconductance
follows a tanh characteristic: `I_out = I_bias * tanh(V_diff / 2V_T)`. At large
signal swings this acts as a slew-rate limiter, causing signal-dependent cutoff
shift and subtle harmonic generation.

Each stage solves the implicit equation `y = s + g * tanh(x - y)` via one
Newton-Raphson iteration from the linear TPT estimate. This avoids the zipper
artifacts the explicit form produces at high cutoff + resonance. The tanh is
approximated with a Pade rational function:

```
tanh_approx(x) = x * (27 + x^2) / (27 + 9 * x^2)
```

### Q Compensation

The Juno-6's BA662 OTA-VCA feeds a portion of the input signal alongside the
resonance feedback, boosting drive at high Q. This counteracts the passband
volume drop inherent in ladder filters and pushes the OTA nonlinearities harder.
Modeled as a gentle quadratic input gain ramp:

```
comp = 1 + res^2 * 0.5
u = (input * comp - k * tanh(S)) / (1 + k * G)
```

### Resonance

Feedback gain `k = res * 4.5` exceeds unity at maximum, allowing self-oscillation.
Above normalized frequency 0.7, resonance tapers linearly toward 0.25 to reduce
aliased harmonics near Nyquist.

### Adaptive Thermal Noise

Real analog filters self-oscillate from thermal noise in the circuit. Digital filters
are silent at zero input, so we inject noise scaled inversely to the filter's state
energy:

```
noiseLevel = 1e-3 / (1 + stateEnergy * 1000)
```

High at startup (seeds oscillation quickly), fades to near-zero once the filter
is active. Each voice gets a deterministic LCG PRNG seeded by voice index.

### Filter Startup

When a note triggers with high resonance, `mS[0]` is seeded to approximate the
steady-state self-oscillation amplitude: `0.3 * max(res - 0.7, 0) / 0.3`. This
gives instant resonant character without waiting for noise to build up.

### Cutoff Calculation

Attempt to match the original Juno-106 VCF control law:

```
vcfFrq = log(sliderFreq) + offset
       + log(noteFreq / 32.703) * kbdTracking
       + log(Nyquist) * envelope * vcfEnv * 0.73
       + log(Nyquist) * lfo * vcfLfo * 0.2
       + 4.159 * bender * bendVcf
```

Result is exponentiated and clamped to `[20 Hz, 0.82 * Nyquist]`.

---

## ADSR Envelope (`Source/DSP/KR106Voice.h`)

Two selectable modes via the **ADSR Mode** switch:

### Juno-106 Mode (default)

| Stage   | Shape       | Notes |
|---------|-------------|-------|
| Attack  | Linear ramp | Does *not* clamp at 1.0 — overshoots ~2-3% at fast settings for percussive bite |
| Decay   | Exponential | Fixed rate toward 0, stops when crossing sustain level |
| Sustain | Tracked     | Ramps smoothly to new level on parameter change (up at 3x decay rate) |
| Release | Exponential | Separate multiplier from decay (independent coefficient) |

### Juno-6 Mode

| Stage   | Shape       | Notes |
|---------|-------------|-------|
| Attack  | Exponential RC | One-pole charge toward 1.5 (comparator threshold at 1.0), matching IR3R01 behavior |
| Decay   | Exponential | Same as 106 mode but with Juno-6 time range LUT |
| Sustain | Tracked     | Same as 106 mode |
| Release | Exponential | Juno-6 time range LUT |

The Juno-6 attack uses `mEnv += (1.5 - mEnv) * coeff` — an RC charging curve
that starts fast and decelerates toward the target, producing a rounder onset
than the Juno-106's linear ramp. Attack/decay/release times use separate
calibration LUTs for each mode.

**Time constants:** Calculated to reach -60 dB at the specified millisecond value.
Voice silences at -100 dB (1e-5).

**Gate mode:** Separate gate envelope with fixed ~1.5 ms ramp (1/32 per sample)
for the VCA Gate switch position.

**Per-voice tolerance:** Envelope timing varies +-8% per voice, modeling component
matching in the hardware.

---

## Per-Voice Variance (`Source/DSP/KR106Voice.h`)

Each of the 6 voices gets deterministic offsets seeded by voice index,
modeling the component tolerances of one physical Juno-106 unit:

| Parameter | Variance |
|-----------|----------|
| VCF cutoff | +-5% |
| Pitch | +-3 cents |
| Envelope timing | +-8% |
| VCA gain | +-0.5 dB |

Same offsets every session (deterministic seed), so a given unit sounds consistent.

---

## LFO (`Source/DSP/KR106LFO.h`)

**Waveform:** Triangle with cubic soft-clip at peaks:
```
tri = 1 - 4 * |phase - 0.5|
tri = tri * (1.5 - 0.5 * tri^2)
```

**Delay envelope:** RC exponential ramp (0-1.5 seconds). LFO output scales from
0 to full amplitude over the delay period.

**Modes:**
- **Auto (0):** Delay resets on first note, persists across legato playing.
- **Manual (1):** Delay resets on explicit trigger button press, cuts on release.

---

## HPF — 4-Position High-Pass Filter (`Source/DSP/KR106_DSP.h`)

Replicates the Juno-106's 4052 analog switch network.

| Position | Type | Frequency |
|----------|------|-----------|
| 0 (bottom) | Low-shelf boost | +10 dB below 150 Hz |
| 1 | Bypass (flat) | -- |
| 2 | 1-pole HPF | 240 Hz |
| 3 (top) | 1-pole HPF | 720 Hz |

All filters use TPT topology for smooth modulation-free operation.

---

## Chorus — BBD Emulation (`Source/DSP/KR106Chorus.h`)

Models the Juno-106's MN3009 bucket-brigade delay chorus.

### BBD Delay Line

- **256-stage emulation** with 4-point Hermite (Catmull-Rom) interpolation
- **Center delay:** 3.5 ms
- **Pre-filter:** 15 kHz lowpass (anti-aliasing before the delay)
- **Post-filter:** 15 kHz lowpass (reconstruction)
- **BBD bandwidth filter:** Single-pole TPT lowpass with cutoff = `256 / (4 * delaySec)`,
  modeling the frequency response imposed by the BBD's clock-sampled operation
- **Charge saturation:** Linear below +-0.7, soft-compressed above:
  `sign * (0.7 + excess / (1 + 2 * excess))`

### Chorus Modes

| Mode | Rate | Depth | Notes |
|------|------|-------|-------|
| Off | -- | -- | Dry bypass |
| I | 0.513 Hz | +-0.5 ms | Subtle, classic |
| II | 0.863 Hz | +-1.1 ms | Wider, more animated |
| I+II | Both | Both | Independent taps at both rates |

**LFO:** Triangle wave. Modes I and II use 180-degree phase offset between L/R taps.
Mode I+II runs both rates independently.

**Dry/wet mix:** `1.2 * (0.4 * dry + 0.6 * wet)` — makeup gain compensates comb
filter energy loss.

---

## Arpeggiator (`Source/DSP/KR106Arpeggiator.h`)

### Modes

| Mode | Behavior |
|------|----------|
| Up | Ascending through held notes |
| Up/Down | Bounces — peak and trough notes play once (no repeat) |
| Down | Descending through held notes |

### Range

| Setting | Octaves |
|---------|---------|
| 0 | 1 (held notes only) |
| 1 | 2 |
| 2 | 3 (full Juno range) |

**Rate:** Configurable from 60-960 steps per minute.

**Timing:** Phase accumulator per sample. When phase crosses 1.0, the previous arp
note is released and the next note triggers. First note triggers immediately.

**Held notes** are kept in a sorted vector. Notes exceeding MIDI 127 when transposed
by octave range are excluded from the sequence.

---

## Portamento & Unison (`Source/DSP/KR106Voice.h`, `Source/DSP/KR106_DSP.h`)

### Modes

| Mode | Voices | Portamento |
|------|--------|------------|
| Poly (0) | 6 independent | Off |
| Poly+Porta (1) | 6 independent | On (per-voice glide) |
| Unison (2) | All 6 on one note | On (all glide together) |

**Glide time:** Cubic mapping `knob^3 * 3 seconds` — concentrates fine control
at the short end of the range.

**Per-sample smoothing:** `portaCoeff = exp(-1 / (time * sampleRate))`

In unison mode, all 6 voices are triggered directly (bypassing the voice allocator)
at the same pitch. No volume compensation is applied — the Juno-106 hardware is
louder in unison mode.

---

## PWM Routing

| Mode | Source | Formula |
|------|--------|---------|
| LFO | Triangle LFO | `pwmSlider * (lfo + 1) * 0.5` |
| Manual | Direct | `pwmSlider` |
| Envelope | ADSR output | `pwmSlider * envelope` |

Result is scaled to the range [0.52, 0.98] to stay within musically useful
pulse widths and avoid silence at the extremes.

---

## File Reference

| File | Role |
|------|------|
| `Source/DSP/KR106_DSP.h` | Top-level orchestrator, HPF, signal routing |
| `Source/DSP/KR106_DSP_SetParam.h` | Parameter dispatch, ADSR mode switching, LUT lookups |
| `Source/DSP/KR106Voice.h` | Per-voice: VCF, ADSR, oscillator mixing, portamento, variance |
| `Source/DSP/KR106Oscillators.h` | PolyBLEP saw, pulse, sub, noise generators |
| `Source/DSP/KR106Chorus.h` | BBD chorus with Hermite interpolation |
| `Source/DSP/KR106LFO.h` | Global triangle LFO with delay envelope |
| `Source/DSP/KR106Arpeggiator.h` | Note sequencer with Up/Down/Up-Down modes |
