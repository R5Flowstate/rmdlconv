# rmdlconv ( R5Flowstate/S21 )

Converts Source / Respawn studio models between versions. This fork emits
**mdl_ v17** for the Season 21 client and **v10 / v54** for the S3 dedicated
server.

Agents view included: CLAUDE.md

## Usage

```
rmdlconv -convertmodel <file> [-targetversion <17|54|10>] [-outputdir <dir>]
rmdlconv <-v8|-v122|-v49|-v191|-v17> <srcDir> <outDir> [-targetversion 17]
```

```
# S21 client (mdl_ v17)
rmdlconv -v122 <src> <out> -targetversion 17

# S3 dedi (v10 + phy + collision)
rmdlconv -v17 <src> <out>

# v19.1 -> v17 compact
rmdlconv -v191 <src> <out> -targetversion 17
```

Folder flags take a directory; `-convertmodel` takes one file.

Upstream: [r-ex/rmdlconv](https://github.com/r-ex/rmdlconv)
