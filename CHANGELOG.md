# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-10-13

### Added

- Hides the model of the sheathed **left-hand** item instead of displaying it on
  the character's back. Covered stances: two-handing the right-hand weapon,
  one-handing a paired weapon in the right hand, and two-handing a paired weapon.
  Items whose left-hand stance is actually in use (blocking, left-hand attacks,
  casting) are untouched.
- `xinput1_3.dll` **mod loader shell**: mirrors the complete export table of the
  real system DLL (ordinals 1–8, same names) and forwards every call to it, then
  loads this mod's core plus any further DLL mods listed in
  `[loader] ExtraMods`. `ExtraModsDelayMs` chooses between loading at import
  resolution (earliest possible) and after a configurable delay.
- Live configuration through `HideSheathedLeft.ini`: re-read every few seconds, so
  `HideLeftHang` and `HidePairedOffhand` can be toggled without restarting the
  game. Original parameter values are snapshotted so switching an option off
  restores vanilla behaviour immediately.
- `build.ps1` (MSVC x64, no third-party dependencies).
- `docs/MECHANISM.md`: the reverse-engineering write-up — parameter row layout,
  how the attachment cluster is chosen, why an invalid dummy poly means "do not
  render", which plausible-looking fields do nothing, how the table is located
  without version-specific offsets, and the measurements behind each claim.
- `tools/`: the Python helpers used to find and prove the mechanism (live
  parameter-table access, window capture, pixel diffing, table-owner probe).

### Known limitations

- The parameter table is shared by every character, so an NPC that enters the same
  stance would also lose the model. In practice DS3 enemies hold their weapons
  continuously and rarely enter it. Strictly player-only behaviour requires either
  hooking the engine's attachment decision or editing the player-only animation
  set — see [`docs/MECHANISM.md` §8](docs/MECHANISM.md#8-making-it-strictly-player-only).
- `right_1..3` is rewritten for all weapons; for weapons without models 1–3 this is
  a no-op. Set `HidePairedOffhand=0` to isolate any suspicious behaviour.
- Verified on Dark Souls III 1.15.0 (Steam, 64-bit) only.

[1.0.0]: https://github.com/xiangchi7/HideSheathedLeft-for-DS3/releases/tag/v1.0.0
