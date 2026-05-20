# zmk-input-processor-threshold

A [ZMK](https://zmk.dev) input processor that blocks pointing device output until enough movement has accumulated *within a recent time window* to confirm intentional input. Accidental contact — a finger resting on or brushing the device, or steady low-rate jitter — is silently discarded. Counts decay over the window, so movement must be sustained at a real rate to break through; once silent, the device re-blocks automatically.

> **v1.1 — breaking:** `param2` changed from `idle_ms` (reset accumulator after a full silence gap) to `window_ms` (accumulator decays continuously). Sustained low-rate jitter no longer leaks through, and re-block happens as the accumulator drains rather than via a separate idle timer. Same syntax, different behavior — review your value. Pin to `revision: v1.0` for the old semantics. See [CHANGELOG.md](CHANGELOG.md).

## Getting Started

### `config/west.yml`

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    # --- copy from here ---
    - name: bunnyspa
      url-base: https://github.com/bunnyspa
    # --- to here ---
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    # --- copy from here ---
    - name: zmk-input-processor-threshold
      remote: bunnyspa
      revision: main
    # --- to here ---
  self:
    path: config
```

### `<keyboard>.conf`

```ini
CONFIG_ZMK_POINTING=y
CONFIG_ZMK_INPUT_PROCESSOR_THRESHOLD=y
```

Optionally, add the following when using a trackpad — blocked by default. Change to `=n` to disable.

```ini
CONFIG_ZMK_INPUT_PROCESSOR_THRESHOLD_BLOCK_BUTTONS=y
CONFIG_ZMK_INPUT_PROCESSOR_THRESHOLD_BLOCK_SCROLL=y
```

### `<keyboard>.overlay` or `<keyboard>.dtsi`

`pointing_device_listener` and `&pointing_device` are placeholders. For example, if you are using the [zmk-pmw3610-driver](https://github.com/badjeff/zmk-pmw3610-driver), replace them with `trackball_listener` and `&trackball`.

```c
#include <input/processors/threshold.dtsi>

/ {
    pointing_device_listener {
        compatible = "zmk,input-listener";
        device = <&pointing_device>;
        input-processors = <&threshold 100 2000>;
    };
};
```

If `pointing_device_listener` is already defined by your board or shield (e.g. in a file you don't own and can't edit directly), override it instead:

```c
#include <input/processors/threshold.dtsi>

&pointing_device_listener {
    input-processors = <&threshold 100 2000>;
};
```

## Parameters

`<&threshold param1 param2>`

**`param1` — threshold (raw sensor counts)**

The values below are calculated and may not reflect real-world sensor behavior. Use them as a starting point and adjust to feel.

<table>
<thead>
<tr><th rowspan="2" align="center">Sensor CPI</th><th colspan="3" align="center">34mm trackball</th><th colspan="3" align="center">40mm trackball</th><th colspan="2" align="center">trackpad</th></tr>
<tr><th align="center">5°</th><th align="center">10°</th><th align="center">15°</th><th align="center">5°</th><th align="center">10°</th><th align="center">15°</th><th align="center">3mm</th><th align="center">5mm</th></tr>
</thead>
<tbody>
<tr><td align="center">400</td><td align="center">23</td><td align="center">47</td><td align="center">70</td><td align="center">27</td><td align="center">55</td><td align="center">82</td><td align="center">47</td><td align="center">79</td></tr>
<tr><td align="center">600</td><td align="center">35</td><td align="center">70</td><td align="center">105</td><td align="center">41</td><td align="center">82</td><td align="center">124</td><td align="center">71</td><td align="center">118</td></tr>
<tr><td align="center">800</td><td align="center">47</td><td align="center">93</td><td align="center">140</td><td align="center">55</td><td align="center">110</td><td align="center">165</td><td align="center">95</td><td align="center">157</td></tr>
<tr><td align="center">1200</td><td align="center">70</td><td align="center">140</td><td align="center">210</td><td align="center">82</td><td align="center">165</td><td align="center">247</td><td align="center">142</td><td align="center">236</td></tr>
<tr><td align="center">1600</td><td align="center">93</td><td align="center">187</td><td align="center">280</td><td align="center">110</td><td align="center">220</td><td align="center">330</td><td align="center">189</td><td align="center">315</td></tr>
</tbody>
</table>

**`param2` — window-ms**

The time window (in milliseconds) over which counts accumulate. Accumulated counts decay at a rate of `threshold / window_ms` per millisecond, so movement must sustain that rate to cross the threshold. When movement stops, the accumulator drains and the device re-blocks within `window_ms` to `2 × window_ms`, depending on how much was banked during the gesture (the accumulator is capped at `2 × threshold`).

- Increase if real movement gets cut off, or if the block re-arms too quickly during normal use.
- Decrease if accidental touches or low-rate jitter slip through, or if the device takes too long to re-block after you lift your hand.

## Chaining with other processors

Place `threshold` first. Processors after it only receive events once intentional movement is confirmed — accidental contact is silently dropped before it reaches them.

If you need a processor to activate on any touch regardless of the threshold block, place it before `threshold`.

```c
input-processors = <
    // processors here see all events, including accidental contact
    &threshold 100 2000
    // processors here only see intentional movement
>;
```

A common setup with `zip_temp_layer`, where layer 2 is a mouse click layer that activates only during intentional movement:

```c
input-processors = <
    &threshold 100 2000
    &zip_temp_layer 2 2000
    &zip_xy_scaler 1 2
>;
```

## Notes

Tested with a trackball (Charybdis Mini). Not tested with a trackpad — please open an issue if you encounter any bugs.

