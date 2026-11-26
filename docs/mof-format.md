# The `.mof` model format, version 1.0

`.mof` ("model file") is Farcade Engine's interchange and import format for
3D models: one self-contained file per model holding meshes, materials,
embedded textures, a skeleton and the node hierarchy. Tools write it (the
`mofconv` converter imports glTF 2.0 and OBJ), the editor and the asset
cooker read it. It is designed so that an independent reader can be written
from this document alone, and so that a reader can trust nothing in the file:
every rule below is checkable, and `engine/mof/mof_read.cpp` checks all of
them.

Reference implementation:

| File | What |
|---|---|
| `engine/mof/mof.h` | the structs of this document, reader and writer API |
| `engine/mof/mof_read.cpp` | validating, zero-copy reader (freestanding) |
| `engine/mof/mof_write.cpp` | writer (freestanding, allocator hooks) |
| `engine/mof/mof_import.h`, `import_*.cpp` | glTF / OBJ importers (host libc) |
| `tools/mofconv` | `mofconv` CLI, `mofgen` sample generator |

## 1. Conventions

- **Byte order:** little-endian for every integer and float.
- **Types:** `u8 u16 u32 u64` unsigned, `i32` two's complement, `f32` IEEE-754
  binary32. All fields are naturally aligned inside their records.
- **Alignment:** every chunk starts at a file offset that is a multiple of
  16, and every offset into a data chunk (vertex streams, index arrays,
  texture payloads) is a multiple of 16. Readers that map the file at a
  16-byte aligned address can therefore use all arrays in place. Padding
  bytes should be written as zero and must be ignored by readers.
- **"None" index:** unsigned index fields use `0xFFFFFFFF` (`MOF_NONE`) for
  "no reference"; signed parent and mesh fields use `-1`.
- **Strings:** u32 byte offsets into the string table (section 4.1). Offset 0 is
  the empty string.
- **Coordinate system (same as glTF 2.0):** right-handed, +Y up, +Z towards
  the viewer, units are metres. Front faces are counter-clockwise.
- **Texture coordinates:** origin at the top-left of the image, +u right,
  +v down (glTF convention). The OBJ importer flips v (`v' = 1 - v`).
- **Matrices:** 4x4 `f32[16]`, column-major: element (row r, column c) is
  `m[c * 4 + r]`; translation is `m[12], m[13], m[14]`. Vectors are columns
  (`p' = M p`).
- **Rotations:** unit quaternions stored `x, y, z, w`.
- **Local transforms (TRS):** `M = T * R * S` (scale first, then rotation,
  then translation), as in glTF.
- **Tangents:** `f32[4]`, xyz = unit tangent along +u, w = +1 or -1.
  `bitangent = cross(normal, tangent.xyz) * w` points towards image-up
  (decreasing v), i.e. +Y of a tangent-space normal map (glTF convention).
- **Colour spaces:** factors are linear. Textures in the base colour and
  emissive slots are sRGB-encoded; normal, metallic-roughness and occlusion
  textures are linear. The colour space is a property of the slot, not of
  the texture.

## 2. File layout

```
offset 0      header (64 bytes)
table_offset  chunk table: chunk_count entries of 32 bytes
...           chunks, each at a 16-byte aligned offset, in any order
file_size     end of file
```

Chunks may appear in any order; the writer puts the bulk data first and the
string table last. Gaps between chunks are allowed (and ignored).

### 2.1 Header (64 bytes, at offset 0)

| Offset | Type | Field | Rule |
|---:|---|---|---|
| 0 | u8[4] | `magic` | `'M' 'O' 'F' 0x00` |
| 4 | u16 | `version_major` | must be 1 |
| 6 | u16 | `version_minor` | any value (section 6) |
| 8 | u32 | `flags` | reserved, written 0, unknown bits ignored |
| 12 | u32 | `chunk_count` | <= 256 |
| 16 | u32 | `header_size` | >= 64, multiple of 16 (64 in 1.0) |
| 20 | u32 | `chunk_entry_size` | must be 32 in major version 1 |
| 24 | u64 | `table_offset` | multiple of 16, >= `header_size` |
| 32 | u64 | `file_size` | total size of the file in bytes |
| 40 | u8[24] | `reserved` | written 0, ignored |

`file_size` must be >= `header_size` and must not exceed the number of bytes
the reader has. Bytes after `file_size` (a file embedded in a larger buffer)
are not part of the file and are ignored. The chunk table
`[table_offset, table_offset + 32 * chunk_count)` must lie inside
`[0, file_size)`.

### 2.2 Chunk table entry (32 bytes)

| Offset | Type | Field | Rule |
|---:|---|---|---|
| 0 | u32 | `type` | fourcc, first character in the low byte (`'M' \| 'E'<<8 \| 'S'<<16 \| 'H'<<24`) |
| 4 | u32 | `flags` | bit 0 `REQUIRED`: a reader that does not know `type` must reject the file. Other bits reserved |
| 8 | u64 | `offset` | multiple of 16, >= end of the chunk table |
| 16 | u64 | `size` | bytes; `offset + size <= file_size` (checked without overflow) |
| 24 | u32 | `count` | number of records for record chunks, byte size for `STRS`, 0 for blob chunks |
| 28 | u32 | `reserved` | written 0, ignored |

Table rules:

- Two chunks with non-zero size must not overlap.
- A known chunk type may appear at most once. Unknown types may repeat.
- Unknown chunk types without the `REQUIRED` flag are skipped.
- For record chunks, `size` must equal `count * record_size` exactly.
- A chunk of a type the model does not need may be absent; an absent record
  chunk means zero records, an absent blob chunk means zero bytes.

### 2.3 Chunk types (version 1.0)

| Type | Kind | Record size | Content |
|---|---|---:|---|
| `STRS` | bytes | - | string table |
| `MESH` | records | 128 | `mof_mesh[]` |
| `SUBM` | records | 16 | `mof_submesh[]` (shared by all meshes) |
| `VERT` | blob | - | vertex streams of all meshes |
| `INDX` | blob | - | index arrays of all meshes |
| `MATL` | records | 96 | `mof_material[]` |
| `TEXT` | records | 64 | `mof_texture[]` |
| `TEXD` | blob | - | texture payloads |
| `SKEL` | records | 128 | `mof_bone[]` |
| `NODE` | records | 64 | `mof_node[]` |

`ANIM` is reserved for a future minor version (keyframed bone animation).

## 3. Overflow-safe range rule

Whenever this document says "`[off, off + n * stride)` lies inside a chunk of
`size` bytes", a reader must evaluate it as

```
off % 16 == 0  &&  off <= size  &&  n * stride <= size - off
```

with 64-bit unsigned arithmetic (`n` is at most 2^32 - 1 and `stride` at most
16, so the product cannot overflow). `off + n * stride` must never be
computed before these checks.

## 4. Chunks

### 4.1 `STRS`: string table

A byte array of `size` bytes, `count == size`, `1 <= size < 2^32`. The first
byte and the last byte must be 0. A string is referenced by the byte offset of
its first character and runs to the next 0 byte; because the table ends in 0,
every in-range offset yields a terminated string. A string reference is valid
if it is 0 or `< size`. If `STRS` is absent, only offset 0 is valid. Strings
are UTF-8; names need not be unique. Writers deduplicate identical strings.

### 4.2 `MESH`: meshes (128 bytes each)

| Offset | Type | Field |
|---:|---|---|
| 0 | u32 | `name` (string) |
| 4 | u32 | `vertex_count` |
| 8 | u32 | `index_count` (triangle list) |
| 12 | u32 | `index_format`: 0 = u16, 1 = u32 |
| 16 | u32 | `stream_mask` (below) |
| 20 | u32 | `weight_format`: 0 = `f32[4]`, 1 = unorm16 `u16[4]` (value / 65535) |
| 24 | u32 | `first_submesh` (into `SUBM`) |
| 28 | u32 | `submesh_count` |
| 32 | u32 | `off_position` (VERT) |
| 36 | u32 | `off_normal` (VERT) |
| 40 | u32 | `off_tangent` (VERT) |
| 44 | u32 | `off_uv0` (VERT) |
| 48 | u32 | `off_uv1` (VERT) |
| 52 | u32 | `off_color` (VERT) |
| 56 | u32 | `off_joints` (VERT) |
| 60 | u32 | `off_weights` (VERT) |
| 64 | u32 | `off_indices` (INDX) |
| 68 | f32[3] | `bounds_min` (AABB, mesh space) |
| 80 | f32[3] | `bounds_max` |
| 92 | f32[3] | `sphere_center` (bounding sphere) |
| 104 | f32 | `sphere_radius` |
| 108 | u32[5] | reserved |

`stream_mask` bits; the position stream is always present:

| Bit | Stream | Element | Stride |
|---|---|---|---:|
| - | position | `f32[3]` | 12 |
| 0 `NORMAL` | normal | `f32[3]` unit | 12 |
| 1 `TANGENT` | tangent | `f32[4]` (section 1) | 16 |
| 2 `UV0` | texcoord set 0 | `f32[2]` | 8 |
| 3 `UV1` | texcoord set 1 | `f32[2]` | 8 |
| 4 `COLOR` | vertex colour | `u8[4]` RGBA unorm, linear (like glTF `COLOR_0`) | 4 |
| 5 `SKIN` | joints + weights | `u16[4]` + weights per `weight_format` | 8 + 16 or 8 |

Unknown bits are ignored (a later minor version may add streams). The offset
of a stream whose bit is clear is meaningless (written 0). Streams are
separate arrays (structure of arrays), each `vertex_count` elements long.
Joint indices are indices into the `SKEL` bone array; a vertex is skinned with
`sum(weight[i] * bone_matrix[joint[i]])` where
`bone_matrix = bone_world * inverse_bind`. Weights should sum to 1; unused
influences have weight 0.

Validation, for each mesh:

- `name` valid; `index_format <= 1`; `weight_format <= 1`;
  `index_count % 3 == 0`.
- Every present stream (position always) lies inside `VERT` (section 3) with its
  stride.
- If `SKIN` is set: the skeleton has at least one bone, and every one of the
  `4 * vertex_count` joint indices is `< bone_count`.
- The index array (`index_count` elements of 2 or 4 bytes at `off_indices`)
  lies inside `INDX`, and every index is `< vertex_count`.
- `first_submesh <= submesh_total` and
  `submesh_count <= submesh_total - first_submesh`.
- All 10 bounds floats are finite, `sphere_radius >= 0`, and
  `bounds_min[a] <= bounds_max[a]` for each axis.

A mesh may have `vertex_count == 0` (an empty mesh keeps mesh indices stable).
Index arrays of different meshes are independent: indices are relative to the
mesh's own vertex streams.

### 4.3 `SUBM`: submeshes (16 bytes each)

| Offset | Type | Field |
|---:|---|---|
| 0 | u32 | `first_index` (into the owning mesh's index array) |
| 4 | u32 | `index_count` |
| 8 | u32 | `material` (material index or `MOF_NONE`) |
| 12 | u32 | reserved |

Each mesh owns `submesh_count` consecutive records starting at
`first_submesh`. Validation per referenced submesh:
`first_index <= mesh.index_count`,
`index_count <= mesh.index_count - first_index`, `index_count % 3 == 0`,
`material == MOF_NONE || material < material_count`. Submeshes normally cover
the index array without overlap, but readers must not rely on it. A mesh
without submeshes is valid (nothing to draw with a material).

### 4.4 `VERT`, `INDX`, `TEXD`: blobs

Raw bytes referenced by offsets from `MESH` and `TEXT` records. `count` is 0.
Data of different meshes or textures may share bytes (the writer stores
identical texture payloads once).

### 4.5 `MATL`: materials (96 bytes each)

PBR metallic-roughness, the glTF 2.0 material model.

| Offset | Type | Field |
|---:|---|---|
| 0 | u32 | `name` |
| 4 | u32 | `flags`: bit 0 double-sided; bits 1-2 alpha mode (0 opaque, 1 mask, 2 blend; 3 invalid) |
| 8 | f32 | `alpha_cutoff` (mask mode) |
| 12 | f32 | `metallic` factor |
| 16 | f32 | `roughness` factor |
| 20 | f32 | `normal_scale` |
| 24 | f32 | `occlusion_strength` |
| 28 | f32[4] | `base_color` factor (linear RGBA) |
| 44 | f32[3] | `emissive` factor (linear RGB, may exceed 1) |
| 56 | texref[5] | texture slots |

`texref` is 8 bytes: `u32 texture` (index or `MOF_NONE`), `u32 uv_set` (0 or
1). Slots in order: 0 base colour (sRGB, multiplied by the factor), 1 normal
(tangent space, linear), 2 metallic-roughness (G = roughness, B = metallic,
linear), 3 occlusion (R, linear), 4 emissive (sRGB). Unknown flag bits are
ignored.

Validation: `name` valid; alpha mode <= 2; each slot's texture is `MOF_NONE`
or `< texture_count`; each `uv_set <= 1`; all float fields finite.

### 4.6 `TEXT`: textures (64 bytes each)

| Offset | Type | Field |
|---:|---|---|
| 0 | u32 | `name` |
| 4 | u32 | `codec`: 0 raw RGBA8, 1 PNG, 2 JPEG |
| 8 | u32 | `width` |
| 12 | u32 | `height` |
| 16 | u32 | `mip_count` |
| 20 | u32 | `wrap_s`: 0 repeat, 1 clamp to edge, 2 mirrored repeat |
| 24 | u32 | `wrap_t` |
| 28 | u32 | `min_filter`: 0 nearest, 1 linear |
| 32 | u32 | `mag_filter`: 0 nearest, 1 linear |
| 36 | u32 | `mip_filter`: 0 none, 1 nearest, 2 linear |
| 40 | u32 | `flags` (reserved, 0) |
| 44 | u32 | reserved |
| 48 | u64 | `data_offset` (into `TEXD`) |
| 56 | u64 | `data_size` |

Payload by codec:

- **Raw RGBA8:** `mip_count` levels, largest first, tightly packed, rows top
  to bottom, 4 bytes per pixel (R, G, B, A). Level `i` is
  `max(1, width >> i)` by `max(1, height >> i)` pixels. This is where an
  optional precomputed mip chain lives; `mip_count == 1` means only the base
  level (consumers generate mips if they want them). Rules:
  `1 <= width, height <= 16384`;
  `1 <= mip_count <= 1 + floor(log2(max(width, height)))` (and <= 15);
  `data_size` equals the sum of all level sizes exactly (computed in 64 bits).
- **PNG / JPEG:** the original file bytes, unmodified. `mip_count` must be 1.
  `width`/`height` are informational (taken from the image header by the
  importer; 0 = unknown), each <= 16384. The payload must start with the PNG
  signature `89 50 4E 47 0D 0A 1A 0A` (at least 8 bytes) or with `FF D8 FF`
  (JPEG, at least 4 bytes). Decoding is the consumer's job and must itself be
  robust: the reader does not decode images.

Validation: `name` valid; codec, wrap and filter values in range;
`data_offset % 16 == 0`; `[data_offset, data_offset + data_size)` inside
`TEXD` (both u64, checked as `data_offset <= size && data_size <= size -
data_offset`); the codec rules above.

### 4.7 `SKEL`: skeleton (128 bytes per bone)

One skeleton per file, shared by all skinned meshes.

| Offset | Type | Field |
|---:|---|---|
| 0 | u32 | `name` |
| 4 | i32 | `parent`: -1 for a root, otherwise the index of the parent bone |
| 8 | u32 | `node`: index of the node this bone corresponds to, or `MOF_NONE` |
| 12 | u32 | reserved |
| 16 | f32[3] | `translation` (local bind pose) |
| 28 | f32[4] | `rotation` (quaternion x y z w) |
| 44 | f32[3] | `scale` |
| 56 | f32[16] | `inverse_bind` (column-major) |
| 120 | u32[2] | reserved |

Bones are ordered parents first: `parent == -1 || 0 <= parent < own index`.
This makes cycles impossible and lets consumers compute world transforms in
one forward pass. The local bind transform is relative to the parent bone; for
a root bone it is relative to model space (the importer folds the transforms of
non-bone ancestors into it). `inverse_bind` maps model space to the bone's bind
space.

Validation: `bone_count <= 65536` (joint indices are u16); `name` valid;
parent rule above; `node == MOF_NONE || node < node_count`; all 26 floats
finite.

### 4.8 `NODE`: node hierarchy (64 bytes each)

The scene graph of the model, so multi-part models keep their layout.

| Offset | Type | Field |
|---:|---|---|
| 0 | u32 | `name` |
| 4 | i32 | `parent`: -1 root, else index of the parent node |
| 8 | i32 | `mesh`: mesh index or -1 |
| 12 | u32 | `flags`: bit 0 skinned (the mesh is deformed by `SKEL`) |
| 16 | f32[3] | `translation` (local, relative to parent) |
| 28 | f32[4] | `rotation` |
| 44 | f32[3] | `scale` |
| 56 | u32[2] | reserved |

Validation: `name` valid; `parent == -1 || 0 <= parent < own index`;
`mesh == -1 || 0 <= mesh < mesh_count`; if skinned, `bone_count > 0`; all 10
floats finite. Unknown flag bits are ignored. Several roots are allowed. The
skinned flag does not guarantee that the mesh has a `SKIN` stream (the reader
does not cross-check it): consumers skin only meshes whose `stream_mask` has
`SKIN` and draw others rigidly. A skinned node's own transform does not move the mesh (skinning uses bone
matrices), as in glTF.

## 5. Validation summary

A conforming reader rejects the file unless all of these hold, in addition to
the per-chunk rules of section 4:

1. The buffer holds at least 64 bytes; magic, major version, `header_size`,
   `chunk_entry_size`, `file_size`, `chunk_count <= 256` and `table_offset`
   are as in section 2.1.
2. Every chunk entry: aligned offset after the table, range inside the file,
   no overlap, no duplicate known type, no unknown `REQUIRED` type.
3. Record chunk sizes match their counts exactly; `STRS` starts and ends with 0.
4. Every string reference, index, parent, joint and range is checked as
   described, using the overflow-safe rule of section 3.

The reference reader additionally requires the buffer start to be 16-byte
aligned (so the records and streams can be used in place). Checking costs time
linear in the file size (each index and joint is visited once); there are no
quadratic paths except the chunk overlap test over at most 256 entries.

Things a reader does **not** need to check (they cannot cause out-of-bounds
access): vertex positions, normals, UVs, colours and weights may hold any bit
pattern, quaternions need not be normalised, submeshes may overlap, image
payloads are not decoded.

### 5.1 Size limits

Mesh stream and index offsets are u32, so the `VERT` and `INDX` chunks are
limited to 4 GiB; the reference writer limits whole files to 2 GiB - 1 and
refuses larger models. Texture payloads use u64 offsets and sizes, but raw
textures are bounded by 16384 x 16384 pixels and imported encoded images by
2 GiB. At most 256 chunks and 65536 bones.

## 6. Versioning

- **Major version** changes break compatibility; a version 1 reader rejects
  any other major version.
- **Minor versions** only add: new chunk types, new `stream_mask` bits, new
  flag bits, or meaning for fields that are reserved (written 0) in 1.0.
  Record sizes and the meaning of existing fields never change within a major
  version. A reader therefore accepts any minor version, ignores unknown chunk
  types, bits and reserved fields, and still reads everything it knows.
- A new chunk that changes how existing data must be interpreted (so that
  ignoring it would give wrong results) is written with the `REQUIRED` flag;
  older readers then reject the file instead of misreading it.
- `header_size` may grow in a later minor version; readers locate the chunk
  table through `table_offset`, never by assuming it follows the header.

## 7. Selective reading

Every texture and every mesh can be located and read on its own through the
chunk table, without parsing any other chunk:

1. Read the first 64 bytes. `table_offset + 32 * chunk_count` is the size of
   the "head"; read that many bytes from offset 0 and validate the header and
   table (`mof_head_size()`, `mof_find_chunk()` in the reference reader do
   both).
2. **Texture i:** read 64 bytes at `TEXT.offset + 64 * i`, check
   `i < TEXT.count`, check its fields (section 4.6), then read `data_size`
   bytes at `TEXD.offset + data_offset`.
3. **Mesh i:** read 128 bytes at `MESH.offset + 128 * i`, check the stream and
   index ranges against `VERT.size` / `INDX.size` (section 4.2), then read the
   streams it needs from `VERT` and its indices from `INDX`. Its submeshes are
   `submesh_count` 16-byte records at `SUBM.offset + 16 * first_submesh`.
   Index and joint values must be range-checked after reading, as in 4.2.
4. Names need the string table (`STRS`), which is small.

No chunk contains offsets that only make sense after another record chunk has
been parsed, so a tool can extract one texture without touching any mesh data,
and vice versa.

## 8. Relation to cooked, streamable assets

`.mof` is an interchange format. Games do not load `.mof` files directly: the
asset cooker reads them (with the validating reader) and builds the run-time
formats that the engine streams for maps, objects and textures. The cooker
derives from a `.mof`:

- **Mesh LOD pages:** simplified levels of detail per mesh, split into
  fixed-size, independently loadable pages (vertex and index data quantised
  and laid out for the renderer), with per-page bounds taken from or refined
  beyond the `.mof` bounds.
- **Mip chains:** complete mip chains for every texture, generated from the
  base level (or taken from a raw RGBA8 texture's precomputed chain when
  `mip_count > 1`), with the colour space given by the material slot.
- **Texture tiles:** mip levels cut into tiles for virtual/streamed texturing,
  so only the visible part of a large texture is resident.
- **Skeleton and node data** in the engine's run-time layout (bones are
  already parent-first, so no sorting is needed), and material parameter
  blocks referencing the cooked textures.

Because `.mof` keeps the original PNG/JPEG bytes, full precision floats and
the full scene graph, cooking can be redone with new settings without going
back to the source files. Section 7 lets the cooker process textures and
meshes one at a time with bounded memory.

## 9. Writer (reference implementation)

`mof_write(const mof_wdesc*, mof_buffer*, err, errlen)` builds a file image
from an in-memory description (`mof_wmesh`, `mof_wmaterial`, `mof_wtexture`,
`mof_wbone`, `mof_wnode`, plus optional opaque extra chunks) into a growable
buffer whose memory comes from a caller-supplied `realloc`-style hook. It
computes the mesh bounds (AABB, and a sphere centred on the AABB centre whose
radius reaches the farthest vertex), picks 16-bit indices when
`vertex_count <= 65536` (unless `force_index32`), interns and deduplicates
names, shares identical texture payloads, aligns everything to 16 bytes and
writes `STRS` last. It rejects non-finite positions, out-of-range indices and
incomplete skin data, and finally re-validates the result with `mof_open` (can
be switched off with `skip_verify`), so a successful write is always a valid
file. On any failure, including allocation failure, the buffer is released and
nothing leaks.

## 10. Importers (`mofconv`)

`mofconv input.(gltf|glb|obj) output.mof [--info]` and `mofconv --info
file.mof`. Options for big assets (used by `tools/worldgen/build.py`,
docs/example-world.md), applied in this order:

| Option | What it does |
| --- | --- |
| `--alpha PART=FILE` | Merges a grey alpha map into the base-colour texture of the materials whose name contains PART (stored as PNG), and makes them alpha MASK (cutoff 0.5) and double-sided. Repeatable |
| `--max-texture N` | Downsizes textures larger than N a side (box filter): opaque ones as JPEG, ones with alpha as PNG |
| `--split` | One file per root node with a mesh (`out_0.mof`, `out_1.mof` ...), moved to the origin, with only the materials and textures it uses. For packs that lay their variants side by side |
| `--max-triangles N` | Reduces each file to about N triangles (`engine/mof/simplify.h`: meshoptimizer's simplifier, keeping UV seams and part borders, falling back to the sloppy simplifier) |
| `--lods N1,N2` | Also writes `out_lod1.mof`, `out_lod2.mof` ... with about N1, N2 triangles (foliage LOD, docs/terrain.md) | The importer library (`engine/mof/mof_import.h`) uses libc only
(no POSIX calls) so it can later run on Minimal-OS.

**glTF 2.0** (via cgltf, after `cgltf_validate`):

- Each glTF mesh becomes one `.mof` mesh; each triangle primitive becomes a
  submesh (strips and fans are converted to lists; points, lines, Draco
  primitives are skipped with a warning). The stream set of a mesh is the
  union over its primitives; primitives missing a stream get zeros (UVs),
  white (colour) or generated data (normals, tangents).
- Attributes: POSITION, NORMAL, TANGENT, TEXCOORD_0/1, COLOR_0 (any
  component type, converted to RGBA8), JOINTS_0/WEIGHTS_0 (more than 4
  influences are dropped with a warning). Normalised and sparse accessors are
  handled by cgltf.
- Materials: metallic-roughness factors and all five texture slots, alpha
  mode/cutoff, double-sided, `KHR_materials_emissive_strength` (folded into the
  emissive factor). `KHR_materials_pbrSpecularGlossiness` is approximated.
  Texture coordinate sets other than 0 and 1 fall back to 0 with a warning.
- Images: from buffer views (`.glb`), base64 data URIs, or external files
  (relative to the model, percent-decoded). PNG and JPEG bytes are stored
  unchanged; other formats that stb_image can decode become raw RGBA8.
  Unreadable images and textures without a PNG/JPEG source (WebP, Basis) are
  dropped with a warning and the material slots that used them are cleared.
- Skins: the union of all skins' joints becomes the bone array, sorted
  parents first. Each mesh's skin-local `JOINTS_0` values are remapped to bone
  indices and the inverse bind matrices are reordered with the bones (first
  skin wins for a joint used by several skins). A mesh drawn with several
  different skins uses the first one (warning).
- Nodes: all glTF nodes, reordered parents first (by depth, then by original
  index); matrices are decomposed into TRS.

**OBJ + MTL** (via fast_obj): one mesh, one submesh per material, one root
node. Vertices are unique (position, texcoord, normal) triples; polygons are
fan-triangulated; v is flipped. MTL: `Kd` + `d` -> base colour (`d < 1`
selects blend mode), `Ke` -> emissive, `roughness = 1 - sqrt(Ns / 1000)`
(Blender's convention), metallic 0; `map_Kd`, `map_Ke`, `map_bump`/`bump`
-> base colour, emissive, normal textures. Faces before the first `usemtl`
are assigned to the first MTL material (a fast_obj limitation).

**Generated data:**

- *Normals* (when missing): area weighted, the sum of the unnormalised face
  cross products of all triangles using the vertex, normalised; degenerate
  vertices get (0, 1, 0). For OBJ, file normals win where present.
- *Tangents* (only when the material has a normal map and the file has no
  TANGENT): a simple per-triangle method (Lengyel), not MikkTSpace. Per
  triangle, tangent and bitangent directions are derived from the UV
  gradients (with v flipped to image-up) and summed per vertex; the tangent
  is Gram-Schmidt orthogonalised against the normal, and w is the sign of
  `dot(cross(n, t), bitangent)`. Vertices with degenerate UVs get an
  arbitrary unit vector perpendicular to the normal.

## 11. Test coverage

`make test` runs `mof_tests` (round trip of a model using every feature,
forward compatibility, selective reading, ~100 targeted corruptions,
truncation at every byte, 20000 fixed-seed mutations plus 3000 per sample
file with every accepted mutant fully walked, writer error and
allocation-failure paths, sample files) and `mof_import_tests` (generated
skinned and textured `.gltf` and `.glb`, OBJ + MTL, error cases, 500 mutated
`.glb` files). `make test-mof-asan` runs both with AddressSanitizer and
UndefinedBehaviorSanitizer. `make mof-samples` regenerates
`assets/samples/*.mof`; a test fails if the committed samples are stale.
