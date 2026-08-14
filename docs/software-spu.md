# Software SPU backends

`PsyX_SPUAL_ConfigureRenderer` selects one of four renderers. The three
software modes share one SPU RAM/register/voice state model.

| Value | Mode | Description | Native DSP rate | Recommended output |
| ---: | --- | --- | ---: | ---: |
| 0 | Legacy | Original PsyCross OpenAL implementation; default | OpenAL/device dependent | Device dependent |
| 1 | Authentic | Emulates the original PSX SPU algorithms, arithmetic, and quirks | 44.1 kHz | 44.1 kHz |
| 2 | High Precision | Preserves the original PSX DSP algorithms and character using modern numerical precision | 176.4 kHz | 176.4 kHz |
| 3 | Modern | Recreates the intended result using contemporary high-quality DSP designs | 352.8 kHz | 176.4 or 352.8 kHz |

Software output is restricted to direct members of the 44.1 kHz family:
44.1, 88.2, 176.4, and 352.8 kHz. Authentic and High Precision do not render
natively at 352.8 kHz; Modern can be decimated to any lower listed rate.

## Processing comparison

| Feature | Legacy | Authentic | High Precision | Modern |
| --- | --- | --- | --- | --- |
| Primary goal | Preserve existing PsyCross behavior | Reproduce the documented PSX digital signal path | Preserve PSX DSP character without low-width arithmetic loss | Reconstruct the intended sound with contemporary DSP |
| Voice model | Original per-voice OpenAL implementation | Shared 24-voice software SPU | Same shared SPU state | Same shared SPU state |
| SPU ADPCM | Original PsyCross decoder | Integer PSX predictor and saturation behavior | Same integer decode before high-precision processing | Same integer decode before high-precision processing |
| Pitch interpolation | OpenAL source resampling | Original 512-entry PSX Gaussian table with integer per-term shifts | Same unnormalized PSX Gaussian table using `float64` multiply/accumulate | 64-tap, 1024-phase adaptive windowed-sinc |
| Pitch modulation | Original PsyCross behavior | Documented PMON relation between adjacent voices | Same PMON timing and relation | Same PMON timing with adaptive reconstruction |
| Noise | Original PsyCross behavior | Shared PSX-style noise generator | Same sequence and timing | Same sequence and timing |
| ADSR | Original PsyCross behavior | Sample-clocked PSX envelope rules | Same timing with high-precision gain application | Same timing with high-precision gain application |
| Voice volume and sweeps | Original PsyCross behavior | Signed PSX direct-volume and sweep rules | Same control law with high-precision multiplication | Same control law with high-precision multiplication |
| Mixing | OpenAL mixer | Ordered integer voice/CD/reverb summation with PSX-style saturation stages | `float64` accumulation without incidental fixed-point clipping | `float64` high-rate accumulation |
| Reverb | Original OpenAL EFX path | PSX RAM-backed fixed-point reverb network | Same PSX topology, delays, and coefficient mapping in `float64` | Modern high-rate feedback-delay network mapped from PsyQ controls |
| Decoded CD/XA ingress | Existing application-specific legacy path | PSX zigzag conversion to 44.1 kHz | Currently uses the same zigzag conversion as Authentic | 128-tap rational sinc conversion to 352.8 kHz |
| Output conversion | OpenAL/device behavior | Signed 16-bit native output; direct-family rate expansion supported | High-precision output with direct-family decimation | High-precision output with direct-family decimation |
| Determinism | Depends on OpenAL implementation | Deterministic integer renderer | Deterministic for a matching build/toolchain | Deterministic for a matching build/toolchain |

## Implemented software-visible behavior

The software SPU implements:

- 512 KiB SPU RAM and 24 voices
- SPU ADPCM decoding
- Gaussian interpolation, ADSR, signed volume, and volume sweeps
- Pitch modulation, noise generation, key state, and ENDX
- Decoded CD/XA input
- The PSX RAM-backed reverb algorithm in Authentic
- High Precision and Modern renderers over the same register and voice state

It does not currently implement:

- SPU IRQ behavior or DMA/FIFO cycle timing
- The four hardware capture buffers in the first 4 KiB of SPU RAM

Authentic describes the implemented documented digital path; it is not a claim
of fully verified silicon equivalence. Public hardware documentation leaves
some edge behavior ambiguous, including simultaneous key-on/key-off ordering
and portions of XA interpolation timing.
