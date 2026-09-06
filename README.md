# moNa2 and microball ZMK configuration

This repository manages two independent ZMK firmware configurations.

| Keyboard | Keymap | Firmware artifacts |
| --- | --- | --- |
| moNa2 | `config/mona2.keymap` | `mona2-left`, `mona2-right` |
| microball | `config/microball.keymap` | `microball-left`, `microball-right` |

The configurations intentionally use separate ZMK workspaces: moNa2 uses the
current configuration and microball keeps its compatible ZMK v0.2 environment
and discrete encoder-scroll behavior. Editing one keymap does not change the
other keyboard.

## Firmware builds

The `Build keyboards` workflow creates the four keyboard firmware files and a
`settings-reset` UF2. Each build also uploads a single `firmware` artifact
containing all five files. Flash the left and right UF2 that match the keyboard
being updated.

Both right-side firmware builds include the USB UART ZMK Studio RPC snippet.
moNa2 is also configured for BLE pairing and Studio unlocking in
`config/mona2_r.conf`.

## Keymap drawings

The `Draw ZMK Keymap` workflow produces `mona2.svg` and `microball.svg` in its
`drawings` artifact whenever either keymap or its layout JSON changes.

## COROPIT orientation

The moNa2 right-side overlay already enables `invert-x` and `invert-y` for
COROPIT. No additional overlay edit is required.
