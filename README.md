# c5div-rx: reduced C5VRX-5 firmware for Raspberry Pi diversity capture

Receiver firmware for the **Seeed Studio XIAO ESP32-C5** that receives analog
5.8 GHz FPV video and outputs it as a 6-bit composite video bus. It is meant to
be captured **digitally** by a Raspberry Pi (see [c5div](../README.md), a
dual-receiver diversity decoder). It also still drives the original resistor
DAC if you fit one.

> **Attribution.** This project adapts the C5VRX-5 receiver firmware from
> [C5VRX](https://github.com/Twotoz/C5VRX) by Twotoz and the C5VRX
> contributors. The receiver core is taken unchanged: RF/PHY front end,
> MODEM_DIAG IQ capture, the BitScrambler FM demodulators (D32-R2 default),
> Direct Gain/LEVEL AGC, calibration, DC servo, AFC, profiles and the verified
> transport restart. Visit the [official C5VRX website](https://twotoz.github.io/C5VRX/)
> for the original project and its Discord invite. See the source and license
> notices for the applicable terms. **This derivative is not an official C5VRX
> release** and is not endorsed by the C5VRX authors.
>
> Source: `Twotoz/C5VRX` commit `8b60fef0f43f43a5305d941568c5975386867fc0`
> (2026-11-16), branch `main`. This tree is a fork of that history (branch
> `c5div-lite`); `git diff 8b60fef` shows every change.

## Flight safety

This firmware inherits the experimental status of C5VRX-5. Bugs, signal loss
or receive-chain changes can cause sudden loss of video. Do not rely on it as
your only means of keeping safe control of an aircraft. Test every setup on the
ground first with the propellers removed, and fly only in a controlled area.
The GPL-3.0 warranty disclaimer (sections 15–17 of `LICENSE`) applies.

## What is different from C5VRX-5

| | C5VRX-5 | c5div-rx |
|---|---|---|
| Controls | on-screen menu, USB console, web console | **one button**, nothing else |
| Channels | 6 bands × 8 (48) and a channel scanner | **raceband R1–R8** only |
| DAC bit 2 | D6/GPIO11 (or D3 with *Clock Pin D10* ON) | **always D3/GPIO7**, no clock export |
| Boot screen | logo + loading bar (menu raster) | flat blanking level for 3–4 s, same timing |
| Calibration screens | on-screen status | lines in the USB log (`STATUS_HOLD …`) |
| EXPERIMENTAL items | menu A/B controls, stored in NVS | fixed on the C5VRX-5 baseline |
| Decoder / gain / filter choice | menu, restored from NVS | C5VRX-5 defaults every boot (D32-R2, LEVEL, digital mode 12, FILTER AUTO) |
| OUTPUT level | menu, restored from NVS | 1.0× every boot |
| Flight log, field diagnostics, bench labs, DAC test, IQ snapshot | yes | removed |

The receiver behaviour that matters for the picture is unchanged.

### Button

The XIAO **BOOT** button, or an external button from **D10** to **GND** (internal
pull-up). Both act the same.

| Action | Effect |
|---|---|
| press | next raceband channel: R1 → R2 → … → R8 → R1 |
| hold 2 s | back to **R1** |
| hold 8 s | one-time **filter calibration**: video stops for about 30 s (the C5VRX-5 *CALIBRATE > QUICK*) |

The channel is saved and comes back after a power cycle.

**Diversity pairs:** wire one button to D10 of *both* receivers (one wire to
each D10, a common GND). Both see every press. If one ever misses a press,
hold 2 s and both go back to R1 together.

| Channel | R1 | R2 | R3 | R4 | R5 | R6 | R7 | R8 |
|---|---|---|---|---|---|---|---|---|
| MHz | 5658 | 5695 | 5732 | 5769 | 5806 | 5843 | 5880 | 5917 |

R8 is above Espressif's documented 5 GHz range for the C5. It is tuned the same
way as in C5VRX-5 and is not characterised.

### Pins (XIAO ESP32-C5)

| Pin | Use |
|---|---|
| D4, D5, **D3**, D7, D8, D9 | video bus bit 0 (LSB) … bit 5 (MSB). Sync tip = code 0, blanking = code 20 |
| D10 | optional external button to GND |
| D6 | unused (C5VRX-5 bit 2 without Clock Pin D10) |
| D0, D1, D2 | do not use: internal IQ routing (C5VRX-5) |

The bus updates at 20 MS/s (C5VRX-5's TX40 cadence, each code held for two
40 MHz clocks). For an analog output, wire C5VRX-5's resistor DAC with the 2 kΩ
branch on **D3** (the *Clock Pin D10* wiring in the C5VRX README).

## Build

The exact toolchain is required: C5VRX-5 uses PHY internals of ESP-IDF
**v6.0.2** and checks the PHY library's SHA-256. The script builds inside the
official container, so no local ESP-IDF is needed:

```
tools/build-container.sh        # podman or docker; ~1 min
# -> build/c5div_rx.bin, build/c5div_rx_merged.bin (whole flash image, offset 0x0)
```

The build fails loudly if the PHY pin does not match
(`Unverified C5 PHY: … disabled`). The CI workflow checks for that too.

## Flash

**First time, or from any other firmware:** hold BOOT while plugging in USB,
then:

```
pip install esptool
esptool.py --chip esp32c5 -p /dev/ttyACM0 write_flash 0x0 build/c5div_rx_merged.bin
```

**Updating c5div-rx (or C5VRX-5) while it runs:** use C5VRX's flash tool from
an ESP-IDF shell (or with `esptool` and `pyserial` installed). It sends the
`` ` `` download key first, because esptool's own USB reset does not reach the ROM
loader on a running image:

```
python3 tools/flash.py /dev/ttyACM0 --build build
```

## Status output

On USB (115200, USB-Serial-JTAG), every 2 s:

```
STATUS channel=R1 freq=5658 afc_locked=1 video=1 standard=AUTO detected=PAL signal=74 hold=0 ...
```

`hold=1` means TX is held: during the boot window or a calibration.

## Tests

```
tools/host-tests.sh    # C host tests of the receiver core + source-level invariants
```

The tests come from C5VRX-5's CI. Tests of removed modules are dropped, and
the boot-order, settings, calibration and sampling-phase tests are adapted to
this fork. Three research-model tests (`test_iq_lane_fusion`, `test_p64`,
`test_picture_sync`) also need `numba`.

## Repository layout

- `main/`: firmware. New files are `status.c` (firmware id, download key,
  status line) and `txhold.c` (TX ownership without the menu). Modified files
  carry a `c5div-rx` note where they differ.
- `research/`, `docs/`: unchanged from C5VRX-5. `docs/` describes the original
  firmware, including features removed here.
- `tools/`: `build-container.sh`, `host-tests.sh` (new); `flash.py`,
  `enter_download.py`, `capture_iq_snapshot.py`, `dco_preset.py`,
  `gain_map_preset.py` (C5VRX-5).
- Removed from this fork: `web/` (flasher), `legacy/`, `hardware/`,
  `hardware_experiments/`, the C5VRX branding images and the C5VRX release
  workflows.

## License and branding

GPL-3.0-only (`LICENSE`), as C5VRX. The C5VRX name, logo and visual identity
are **not** covered by the GPL (`assets/BRANDING.md`). This fork carries no
C5VRX branding. Names like "C5VRX-5" appear only to describe where code comes
from.
