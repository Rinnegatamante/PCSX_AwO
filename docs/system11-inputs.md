# System 11 inputs

`libpcsxcore/arcade/system11_inputs.inc` is generated from the six input layouts declared by the pinned MAME source `src/mame/namco/namcos11.cpp` at commit `deb2f54fe7300e7a192690318beb860159bfd52a`. The generator resolves `PORT_INCLUDE` and applies `PORT_MODIFY` in source order, replacing only the bits changed by a child layout. If a source declaration or input type is not understood, generation fails rather than silently using an incomplete map.

The 36 System 11 sets in `scripts/system11_boards.json` reference these six layouts:

| MAME input layout | Sets |
| --- | --- |
| `namcos11` | `dunkmnia`, `dunkmniajc`, `xevi3dg`, `xevi3dga`, `xevi3dgj`, `danceyes`, `danceyesu`, `danceyesj`, `fambowl`, `starswep`, `starswepj` |
| `tekken` | `tekken`, `tekkenb`, `tekkenac`, `tekkenjb`, `tekken2`, `tekken2b`, `tekken2ud`, `tekken2ub`, `tekken2jc`, `tekken2jb`, `tekken2a`, `tekken2ua`, `primglex` |
| `souledge` | `souledge`, `souledgeuc`, `souledgea`, `souledgeua`, `souledgeja` |
| `myangel3` | `myangel3` |
| `ptblank2ua` | `ptblank2a`, `ptblank2b`, `ptblank2c`, `ptblank2ua`, `gunbarla` |
| `pocketrc` | `pocketrc` |

Each binding row names that MAME input layout, a stable host control name, whether it addresses the ADC/gun stream, the port index, and the source mask. Digital indices match `Namcos11Board` order: `PLAYER4` 0, `SWITCH` 1, `PLAYER1` 2, `PLAYER2` 3. ADC channels occupy analog indices 0–7. Controls retain MAME active-low masks and player assignments, including the extra Tekken/Soul Edge buttons, My Angel 3 button remapping, Pocket Racer view button, all four coin inputs where a layout enables them, and the `dip1_test`/`dip2_freeze` toggles. A DIP toggle clears its source mask when activated, matching the source's active-low `On` setting.

The defaults table stores raw source port power-on values. Ordinary active-low ports default to `0xff`; Pocket Racer's paddle and pedal defaults are `0x80` and `0x00`. Point Blank 2's 16-bit gun defaults are retained at analog indices 8–11 (`GUN1X`, `GUN1Y`, `GUN2X`, `GUN2Y`) as metadata; these are not boolean bindings. Paddle, pedal, and light-gun coordinates likewise remain raw analog values rather than being coerced into pressed/released controls.

Regenerate from the adjacent MAME checkout with:

```sh
python3 scripts/generate_system11_inputs.py
```

The script checks the local MAME checkout against the pinned commit before writing. `python3 -m unittest tests.test_system11_inputs -v` verifies source expansion, exact exceptional mappings and defaults, generated-file reproducibility, and fail-closed parsing. The generated declarations drive the native System 11 input API and boolean CSV controls. Raw analog coordinates keep these defaults; configurable analog CSV input remains pending.
