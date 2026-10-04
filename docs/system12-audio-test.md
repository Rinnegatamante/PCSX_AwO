# System 12 C352 audio audit

The pinned MAME source is `src/mame/namco/namcos12.cpp` and `src/devices/sound/c352.cpp` at commit `deb2f54fe7300e7a192690318beb860159bfd52a`. The Soul Calibur C revision (`soulclbrac`) declares a 16 MiB `c352` region. Its sample ROM occupies the first 8 MiB; the remaining region is zero-filled. The MAME device runs at `25,401,600 / 288 = 88,200` samples per second. Its four internal outputs are mixed as `front-left, front-right, rear-left, rear-right`; the board routes only front channels 0 and 1 to a two-channel speaker at unity. The comments explicitly leave channels 2 and 3 disconnected because the second DAC is absent.

The native adapter uses the same clock and divider, same sample region bytes, and same front-only channel selection when writing stereo PCM16 WAV. Its System 12 H8 map covers `0x280000–0x287fff`: aligned 16-bit accesses call the C352 word-register API at `(address - 0x280000) / 2`; even-address byte accesses select the high byte (`mem_mask=0xff00`) and odd-address accesses select the low byte (`0x00ff`). A key-execute write at offset `0x202` only has effect for a full `0xffff` word write, like MAME. The native H8 map returns the word high byte at even addresses and low byte at odd addresses. It writes only C352 front L/R into its stereo WAV.

The C352 engine code paths agree on the register table, key-on/off timing, PCM8 and mu-law decode, 16-bit accumulator/interpolation, per-channel one-step volume ramp, looping/reverse behavior, phase flags, noise recurrence, four-channel mix shift, and signed 16-bit output conversion. This audit found no source-evidenced gain, routing, or clipping error in those paths. MAME's speaker graph has unity gain. MAME's stream output is routed through its sound mixer and resampled to the host WAV rate; the native WAV records C352 output directly at 88.2 kHz. These WAVs therefore are not expected to be byte-identical.

## Repeatable capture

With the local MAME executable, prepared `soulclbrac` region directory, ROM zip, and native harness present, run:

```sh
python3 tests/check_system12_audio.py --seconds 5 --output artifacts/system12/audio-reference
```

The script uses the same no-host-input replay in each emulator, writes MAME WAV at 48 kHz and native WAV at 88.2 kHz, creates a unique MAME NVRAM/config directory for each invocation, validates active stereo PCM16 output, then prints and saves `audio-comparison.json`. It does not fetch ROMs. `--seconds` can be increased for longer captures.

## Captured comparison

A five-second local capture produced 240,001 MAME frames at 48 kHz and 440,999 native frames at 88.2 kHz. MAME front-channel RMS was 217.665/220.071 with peaks 1415/1485; native RMS was 230.905/233.439 with peaks 3012/3077. Neither capture clipped. After a simple linear resample of native output to 48 kHz, unadjusted Pearson correlation over the capture was 0.7674 left and 0.7653 right, with native/MAME RMS ratio 1.0608/1.0607. This is useful waveform evidence that the same early sample playback is active; resampling and board-time differences prevent a bit-exact interpretation.

A longer 60-second no-input capture had similar full-run RMS (MAME 303.37/301.78; native 302.61/300.09), but after the first few seconds sample correlation falls close to zero. Both play a silent interval around seconds 10–20. The games do not remain in matched sound-command state over that duration: the native System 12 report still identifies its PSX timing as the console model, rather than a measured match to MAME's CXD8661R scheduling. Treat long-run waveform divergence as a guest-timing/state limitation, not evidence by itself of a C352 mixing defect. This audit validates active C352 playback and output routing, not full game audio equivalence or game compatibility.
