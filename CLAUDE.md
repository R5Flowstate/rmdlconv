# rmdlconv — agent notes

Human overview: `README.md`.

Emits **S21 client v17** and **S3 v10 / v54**.

## Build

`rmdlconv.sln` / `src/rmdlconv.vcxproj`, Release x64, toolset **v145**.

Output: `bin/Release/rmdlconv.exe`.

## CLI

Folder flags take a **directory**. `-convertmodel` takes one file.

| flag | path |
|------|------|
| `-targetversion 17` | S21 client (mdl_ v17, 228B hdr, rev4 VG) |
| `-targetversion 54` or `10` | S3 dedi (IDST v54 / rmdl v10) |
| `-v17 <src> <out>` | batch S21 **source** (v160/v17) → dedi v10 + phy + collision |
| `-v122 <src> <out>` | batch v12.2 source → **dedi v10** unless you add `-targetversion 17` |
| `-v122 … -targetversion 17` | S10/TF2-era → S21 client v17 |
| `-v8 … -targetversion 17` | S3 v8 → S21 v17 |
| `-v49 … -targetversion 17` | Portal 2 / TF2 `.mdl` → S21 v17 |
| `-v191 <src> <out>` | v19.1 → v17 compact (seqdesc/animdesc shrink) |
| `-nopause` | no console pause |
| `-outputdir` | with `-convertmodel` |

Peek magic/`version` at +4 before choosing a path. Prefer the `.mdl` when both
a `.mdl` and a stripped `.rmdl` exist.

## Output contract (client v17)

228-byte header, split bone header/data, rev4 `.vg` + `.vg_static`, `.phy`
alongside. Name inside the header is `mdl/.../*.rmdl`.

Placeholder local seq `ref`:

| field | required |
|-------|----------|
| activity | `0xFFFF` |
| paramindex[0]/[1] | `-1` / `-1` |
| flags | bit `0x80000` |

`paramindex 0,0` is the `[POSE-PARAM-GUARD]` T-pose class. Rebuild the exe
after pulling; a stale binary rewrites `0,0`.

`studiohdr.flags & 0x80000` is required for virtual-model / animrig bind.
Derived as `numbones > 1 && !STATIC_PROP`. Do not copy the stock lighting bits.

## Collision / phy

- Preserve `unk4_v54` (per-query-class part map) **verbatim**, including `-1`
  sentinels. ~20% of props are `(-1,0)` (walk-through, bullet-solid). Forcing
  `(0,0)` invents dedi walls.
- Modern → legacy surface-prop remap is in the converter. Do not run old
  Python surfprop splicers on new batches.
- Bone followers + `phySize` must be written. Close the `.rmdl` stream
  **before** the phy patch or `phySize` races to 0.
- **`.phy` for dedi staging comes from `converted/`, never the raw extract.**
  S21 phy is a 4-byte header; S3 wants 20-byte `phyheader_t`. Size must
  equal `studiohdr.phySize`.
- `bvhOffset` on v17 must be 16-byte aligned (SSE2 walk).
- Do not pass `-autogenbvh` unless a specific prop must gain collision.

## Do not

- Expect animation inside the `.rmdl`. S21 anims are aseq/arig via
  R5-AnimConv + RePak. Bind a stock S21 `$animrigs`; do not pack a second
  copy of a `common.rpak` rig.
- Ship client VG layout that is not per-LOD interleaved with
  `meshIndex == 0` per LOD. Wrong layout heap-corrupts on streamed load.
