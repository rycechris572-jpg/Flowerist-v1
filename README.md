# PIB V1.0 — Windows Geode

Fresh standalone prototype, no SONWARE code or assets.

## What it actually does
- Hooks `PlayerObject::pushButton` and `releaseButton` and calls the original functions.
- Stores up to 256 recent input events in a fixed-size ring buffer (no allocation per input).
- Estimates the next press time from a smoothed recent press interval.
- Resets the engine on level init and restart; disables capture on quit.

**Not yet implemented:** input injection, obstacle prediction, GUI, settings, or automation. The estimate is observational only; it never changes gameplay.

## Build
1. Create a NEW GitHub repository and upload the **contents** of this folder, including `.github/workflows/build.yml`.
2. In GitHub Actions, run **Build PIB Windows** (or push a commit).
3. Download the `PIB-V1-Windows` artifact and find the `.geode` file.
4. Install it using Geode on Windows.

The `geode` and `gd.win` values in `mod.json` are initial targets; update them to the versions supported by your installed Geode and Geometry Dash if the build reports a version mismatch.

## Important
This source has not been compiled against the Geode SDK in this environment. If GitHub Actions reports a compile error, use the **first actual error** from the log to correct the API signature. A green build is not a gameplay test.
