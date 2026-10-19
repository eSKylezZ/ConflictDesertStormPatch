# DesertStormFix reference snapshot (not built)

The DesertStormFix (DS1) feature sources the other games' ports started from: `ds1-features/` (src/features as of
~2026-10-10; 47 of the 53 files equal commit `314c449`), `ds1-generated/` (patch tables, standard animations),
`patch_defs.py` (DS1 byte patches = `scripts/patch_defs.py` at `314c449`), `gen_standard_anims.py`.

The DS2, Vietnam and Global Storm repos each carried an identical copy under `reference/`; since the move into this
repo (2026-10-19) there is one, shared by `games/ds2`, `games/cv` and `games/cgs` (their PORTING.md link here). The
live DS1 code is the repo root's `src/` - port from that for anything newer.
