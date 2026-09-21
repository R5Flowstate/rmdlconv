# rmdlconv (R5Flowstate / S21)

Converts Source / Respawn studio models between versions. This fork is the
S21-bridge converter: it can emit **mdl_ v17** for the S21 client and **v10 /
v54** for the S3 dedicated server. Upstream rmdlconv only targeted S3 (v54 /
rmdl v10).

Upstream: [r-ex/rmdlconv](https://github.com/r-ex/rmdlconv).

## What this fork adds

- **v19.1 / v16 -> v17** compact client path (`-targetversion 17`): keeps the
  modern on-disk shape, shrinks seqdesc/animdesc to S21 sizes.
- **v12.2 (S10) -> v17** client rebuild (`-v122 -targetversion 17`).
- **v8 -> v17** upgrade (animation gate flag).
- Collision: preserve `unk4_v54` (per-query-class part map, including `-1`
  sentinels), modern->legacy surface-prop remap, bone followers, phySize,
  16-byte `bvhOffset` alignment.
- Per-LOD switch distances carried through instead of zero-filled.
- v17 placeholder sequences match S21 refs (`paramindex -1`, flags `0x80000`).

Legacy S3 paths (Portal 2 v49 / TF2 v53 -> v54) remain.

## Usage

Drag-and-drop a `.mdl` / `.rmdl`, or:

```
rmdlconv.exe -convertmodel <file-or-dir> -targetversion 17 -outputdir <out>
```

- `-targetversion 17` — S21 client (v17)
- `-targetversion 54` or `10` — S3 dedi (v54 / rmdl v10)
- `-v122` — treat input as v12.2
- `-nopause` — close the console when done
- `-outputdir` — output directory

Animation in the model file is limited; S21 animated props go through
R5-AnimConv (`-o 21`) and RePak aseq/arig.

## Building

Open `rmdlconv.sln` and build x64 Release.
