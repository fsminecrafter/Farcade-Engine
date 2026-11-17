# Game packages: `.fepak` v1

A `.fepak` is the file a game loads its content from: scenes, Lua scripts,
models, textures, sounds and shaders in one file, found by *virtual path*
(`scripts/spin.lua`). `engine/core/pak.{h,cpp}` reads and writes it,
`engine/core/vfs.{h,cpp}` is what the engine's loaders call. Everything is
little-endian.

```
offset 0      header (64 bytes)
              file data, each file aligned (below), in the order of the index
index_offset  index: entry_count entries of 40 bytes, then the string table
```

## Header (64 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | `char[8]` | magic `FEPAK\0\0\0` |
| 8 | `u32` | version (1) |
| 12 | `u32` | header size (64; readers skip to it, so a later version may grow the header) |
| 16 | `u64` | index offset |
| 24 | `u64` | index size (entries + string table) |
| 32 | `u32` | entry count |
| 36 | `u32` | header flags (0) |
| 40 | `u32` | chunk alignment (65536) |
| 44 | `u32` | CRC-32 of the index (entries + string table) |
| 48 | `u64` | total file size (a cut-off file is refused) |
| 56 | `u64` | reserved (0) |

## Index entries (40 bytes each, sorted by path)

| Offset | Type | Field |
|---|---|---|
| 0 | `u64` | data offset from the start of the file |
| 8 | `u64` | stored size (bytes in the package) |
| 16 | `u64` | raw size (bytes after decoding; equal to the stored size in v1) |
| 24 | `u32` | name offset in the string table |
| 28 | `u16` | name length |
| 30 | `u16` | flags: bits 0-3 how the data is stored (0 = raw, 1 = zstd, reserved), other bits 0 |
| 32 | `u32` | CRC-32 of the raw bytes |
| 36 | `u32` | reserved (0) |

Entries are sorted by plain byte order of the path, so a lookup is a binary
search. The string table follows the entries; names are not NUL terminated
and are at most 255 bytes.

## Paths

Virtual paths are project relative, use `/`, have no leading `/` and no
`./` or `..`. The exporter stores a file under the path the scene uses for
it (`scenes/main.fscene`, `scripts/spin.lua`, `models/ship.mof`) and adds
`project.fe` at the root. Paths are case sensitive.

## Alignment

Data of files of 64 KiB or more starts on a 64 KiB boundary (reads of one
chunk never straddle a block, and a later streaming loader can map or
prefetch whole chunks, plan C4.8); smaller files start on 16 bytes. The index
comes last, so a package can be written in one pass.

## Reading

`PakReader::open` takes a `PakSource` (a `read_at` callback and a size): a
file read with random access (`platform::open_read` / `read_at`), or a block
of memory. It checks the magic, the version, the total size, the index CRC
and that every entry lies inside the file. `read` returns the entry's bytes
in `malloc`'d memory (plus one zero byte, so text can be used as a C
string) after checking the entry's CRC; `read_range` reads part of an entry
for streaming. Entries with a storage kind other than raw are refused in v1.

Versions: a reader accepts every version up to its own and ignores reserved
fields; a change that old readers cannot handle gets a new version number.
Compression (zstd, per entry, with the stored size smaller than the raw size)
is planned for version 1 files: it only needs the flag and a decoder.

## The VFS

`vfs::read("scripts/spin.lua", &size)` looks in the mounted packages (the
last mounted first), then, when disk fallback is on, in plain files under the
project folder (`vfs::set_root`). The editor reads plain files; the player
turns the fallback off, so a game only sees its package. Full paths that
start with the project folder and paths with backslashes are normalized to
virtual ones first. `vfs::read_cb` fits the callbacks the renderer, audio
system and play session take (`read_asset`, `read_file`).
