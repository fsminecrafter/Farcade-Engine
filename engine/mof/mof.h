// .mof - Farcade Engine model interchange format (version 1.x).
//
// One self-contained little-endian file per model: meshes, materials, embedded
// textures, skeleton and node hierarchy. See docs/mof-format.md for the byte
// level specification; this header is the C++ mirror of it.
//
// This header and mof_read.cpp / mof_write.cpp use only <stdint.h> and
// <stddef.h>: no C++ standard library, no libc calls except
// memcpy / memset / memcmp, so they also build for Minimal-OS (freestanding).
#pragma once
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------- constants

#define MOF_VERSION_MAJOR 1
#define MOF_VERSION_MINOR 0

#define MOF_FOURCC(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

enum : uint32_t {
    MOF_CHUNK_STRINGS   = MOF_FOURCC('S', 'T', 'R', 'S'),  // string table (bytes)
    MOF_CHUNK_MESHES    = MOF_FOURCC('M', 'E', 'S', 'H'),  // mof_mesh[]
    MOF_CHUNK_SUBMESHES = MOF_FOURCC('S', 'U', 'B', 'M'),  // mof_submesh[]
    MOF_CHUNK_VERTICES  = MOF_FOURCC('V', 'E', 'R', 'T'),  // vertex stream bytes
    MOF_CHUNK_INDICES   = MOF_FOURCC('I', 'N', 'D', 'X'),  // index bytes
    MOF_CHUNK_MATERIALS = MOF_FOURCC('M', 'A', 'T', 'L'),  // mof_material[]
    MOF_CHUNK_TEXTURES  = MOF_FOURCC('T', 'E', 'X', 'T'),  // mof_texture[]
    MOF_CHUNK_TEXDATA   = MOF_FOURCC('T', 'E', 'X', 'D'),  // texture payload bytes
    MOF_CHUNK_SKELETON  = MOF_FOURCC('S', 'K', 'E', 'L'),  // mof_bone[]
    MOF_CHUNK_NODES     = MOF_FOURCC('N', 'O', 'D', 'E'),  // mof_node[]
};

enum : uint32_t { MOF_CHUNK_FLAG_REQUIRED = 1u };  // reader must understand this chunk type

#define MOF_NONE 0xFFFFFFFFu  // "no index" in unsigned index fields (parents use int32 -1)

enum : uint32_t {  // mof_mesh::stream_mask (position is always present)
    MOF_STREAM_NORMAL  = 1u << 0,
    MOF_STREAM_TANGENT = 1u << 1,
    MOF_STREAM_UV0     = 1u << 2,
    MOF_STREAM_UV1     = 1u << 3,
    MOF_STREAM_COLOR   = 1u << 4,
    MOF_STREAM_SKIN    = 1u << 5,  // joints (4 x u16) + weights
};
enum : uint32_t { MOF_INDEX_U16 = 0, MOF_INDEX_U32 = 1 };
enum : uint32_t { MOF_WEIGHTS_F32 = 0, MOF_WEIGHTS_UNORM16 = 1 };

enum : uint32_t { MOF_ALPHA_OPAQUE = 0, MOF_ALPHA_MASK = 1, MOF_ALPHA_BLEND = 2 };
enum : uint32_t { MOF_MATERIAL_DOUBLE_SIDED = 1u, MOF_MATERIAL_ALPHA_SHIFT = 1, MOF_MATERIAL_ALPHA_MASK = 3u << 1 };

enum : uint32_t {  // texture slots of mof_material::tex
    MOF_SLOT_BASE_COLOR = 0,  // sRGB
    MOF_SLOT_NORMAL = 1,
    MOF_SLOT_METAL_ROUGH = 2,  // G = roughness, B = metallic
    MOF_SLOT_OCCLUSION = 3,    // R
    MOF_SLOT_EMISSIVE = 4,     // sRGB
    MOF_SLOT_COUNT = 5,
};

enum : uint32_t { MOF_TEX_RAW_RGBA8 = 0, MOF_TEX_PNG = 1, MOF_TEX_JPEG = 2 };
enum : uint32_t { MOF_WRAP_REPEAT = 0, MOF_WRAP_CLAMP = 1, MOF_WRAP_MIRROR = 2 };
enum : uint32_t { MOF_FILTER_NEAREST = 0, MOF_FILTER_LINEAR = 1 };
enum : uint32_t { MOF_MIP_NONE = 0, MOF_MIP_NEAREST = 1, MOF_MIP_LINEAR = 2 };

enum : uint32_t { MOF_NODE_SKINNED = 1u };

// Validation limits (reader rejects files beyond them).
enum : uint32_t {
    MOF_MAX_CHUNKS = 256,
    MOF_MAX_BONES = 65536,  // joint indices are uint16
    MOF_MAX_TEXTURE_DIM = 16384,
    MOF_MAX_MIPS = 15,
};

// ------------------------------------------------------------ on-disk records
// All fields little-endian, naturally aligned, sizes fixed per major version.
// "reserved" fields are written as zero and ignored by readers.

struct mof_header {  // 64 bytes at file offset 0
    char magic[4];             // "MOF\0"
    uint16_t version_major;    // 1
    uint16_t version_minor;    // 0
    uint32_t flags;            // reserved (0); unknown bits ignored
    uint32_t chunk_count;
    uint32_t header_size;      // >= 64
    uint32_t chunk_entry_size; // 32 (fixed in major version 1)
    uint64_t table_offset;     // multiple of 16, >= header_size
    uint64_t file_size;        // total bytes of the file
    uint8_t reserved[24];
};

struct mof_chunk_entry {  // 32 bytes
    uint32_t type;
    uint32_t flags;
    uint64_t offset;  // from file start, multiple of 16
    uint64_t size;    // bytes
    uint32_t count;   // records for record chunks (size == count * record size), bytes for STRS, 0 for blobs
    uint32_t reserved;
};

struct mof_mesh {  // 128 bytes
    uint32_t name;          // string offset
    uint32_t vertex_count;
    uint32_t index_count;   // triangle list, multiple of 3
    uint32_t index_format;  // MOF_INDEX_*
    uint32_t stream_mask;   // MOF_STREAM_*
    uint32_t weight_format; // MOF_WEIGHTS_*
    uint32_t first_submesh; // into the global submesh array
    uint32_t submesh_count;
    // Byte offsets, 16-aligned. position/normal/tangent/uv0/uv1/color/joints/weights
    // are relative to the VERT chunk, off_indices to the INDX chunk. 0 when absent.
    uint32_t off_position;  // float[3] * vertex_count
    uint32_t off_normal;    // float[3]
    uint32_t off_tangent;   // float[4] (xyz + handedness w = +-1)
    uint32_t off_uv0;       // float[2]
    uint32_t off_uv1;       // float[2]
    uint32_t off_color;     // uint8[4] RGBA, linear unorm (like glTF COLOR_0)
    uint32_t off_joints;    // uint16[4]
    uint32_t off_weights;   // float[4] or uint16[4] (unorm16)
    uint32_t off_indices;   // uint16 or uint32 * index_count
    float bounds_min[3];
    float bounds_max[3];
    float sphere_center[3];
    float sphere_radius;
    uint32_t reserved[5];
};

struct mof_submesh {  // 16 bytes
    uint32_t first_index;  // into the mesh's index array
    uint32_t index_count;  // multiple of 3
    uint32_t material;     // material index or MOF_NONE
    uint32_t reserved;
};

struct mof_texref {  // 8 bytes
    uint32_t texture;  // texture index or MOF_NONE
    uint32_t uv_set;   // 0 or 1
};

struct mof_material {  // 96 bytes
    uint32_t name;
    uint32_t flags;  // MOF_MATERIAL_DOUBLE_SIDED | alpha mode << MOF_MATERIAL_ALPHA_SHIFT
    float alpha_cutoff;
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    float base_color[4];  // linear RGBA factor
    float emissive[3];    // linear RGB factor
    mof_texref tex[MOF_SLOT_COUNT];
};

struct mof_texture {  // 64 bytes
    uint32_t name;
    uint32_t codec;  // MOF_TEX_*
    uint32_t width;  // raw: required; PNG/JPEG: informational (0 = unknown)
    uint32_t height;
    uint32_t mip_count;  // raw: 1..15 (tightly packed, largest first); encoded: 1
    uint32_t wrap_s;
    uint32_t wrap_t;
    uint32_t min_filter;
    uint32_t mag_filter;
    uint32_t mip_filter;
    uint32_t flags;  // reserved
    uint32_t reserved;
    uint64_t data_offset;  // relative to TEXD chunk, multiple of 16
    uint64_t data_size;
};

struct mof_bone {  // 128 bytes
    uint32_t name;
    int32_t parent;  // -1 root, else < own index
    uint32_t node;   // node index this bone corresponds to, or MOF_NONE
    uint32_t reserved0;
    float translation[3];
    float rotation[4];  // quaternion x y z w
    float scale[3];
    float inverse_bind[16];  // column-major 4x4: m[col * 4 + row]
    uint32_t reserved1[2];
};

struct mof_node {  // 64 bytes
    uint32_t name;
    int32_t parent;  // -1 root, else < own index
    int32_t mesh;    // mesh index or -1
    uint32_t flags;  // MOF_NODE_*
    float translation[3];
    float rotation[4];
    float scale[3];
    uint32_t reserved[2];
};

static_assert(sizeof(mof_header) == 64, "mof_header");
static_assert(sizeof(mof_chunk_entry) == 32, "mof_chunk_entry");
static_assert(sizeof(mof_mesh) == 128, "mof_mesh");
static_assert(sizeof(mof_submesh) == 16, "mof_submesh");
static_assert(sizeof(mof_material) == 96, "mof_material");
static_assert(sizeof(mof_texture) == 64, "mof_texture");
static_assert(sizeof(mof_bone) == 128, "mof_bone");
static_assert(sizeof(mof_node) == 64, "mof_node");

// ------------------------------------------------------------------- reader

// Zero-copy view of a validated file. Every pointer points into the buffer
// given to mof_open, which must stay alive and unmodified while the view is used.
struct mof_file {
    const uint8_t* data;
    size_t size;  // header.file_size
    uint16_t version_major, version_minor;
    uint32_t flags;

    uint32_t chunk_count;
    const mof_chunk_entry* chunks;  // all chunks, known and unknown

    const char* strings;
    uint32_t strings_size;

    uint32_t mesh_count;      const mof_mesh* meshes;
    uint32_t submesh_count;   const mof_submesh* submeshes;
    uint32_t material_count;  const mof_material* materials;
    uint32_t texture_count;   const mof_texture* textures;
    uint32_t bone_count;      const mof_bone* bones;
    uint32_t node_count;      const mof_node* nodes;

    const uint8_t* vertex_data;  uint64_t vertex_data_size;
    const uint8_t* index_data;   uint64_t index_data_size;
    const uint8_t* texture_data; uint64_t texture_data_size;
};

// Validates `data` completely and fills `out`. `data` must be 16-byte aligned
// (the format's alignment guarantees need an aligned base). On failure returns
// false, leaves `out` zeroed and writes a message to `err` (may be null).
// Never reads outside [data, data + size), whatever the content.
bool mof_open(const void* data, size_t size, mof_file* out, char* err, size_t errlen);

// Name lookup; offsets in a validated file are always in range. Returns "" for
// offset 0 or anything out of range.
const char* mof_string(const mof_file* f, uint32_t offset);

// Stream views (null when the stream is absent). Counts: mesh->vertex_count.
const float*    mof_mesh_positions(const mof_file* f, const mof_mesh* m);  // 3 floats per vertex
const float*    mof_mesh_normals(const mof_file* f, const mof_mesh* m);    // 3
const float*    mof_mesh_tangents(const mof_file* f, const mof_mesh* m);   // 4
const float*    mof_mesh_uv0(const mof_file* f, const mof_mesh* m);        // 2
const float*    mof_mesh_uv1(const mof_file* f, const mof_mesh* m);        // 2
const uint8_t*  mof_mesh_colors(const mof_file* f, const mof_mesh* m);     // 4 bytes RGBA
const uint16_t* mof_mesh_joints(const mof_file* f, const mof_mesh* m);     // 4
const void*     mof_mesh_weights(const mof_file* f, const mof_mesh* m);    // float[4] or uint16[4] per m->weight_format
const void*     mof_mesh_indices(const mof_file* f, const mof_mesh* m);    // uint16 or uint32 per m->index_format
uint32_t        mof_mesh_index(const mof_file* f, const mof_mesh* m, uint32_t i);  // format-independent
// Texture payload bytes (size in *size); null if the texture is out of range.
const uint8_t*  mof_texture_bytes(const mof_file* f, uint32_t texture, uint64_t* size);

// ---- partial access (cook tools, streaming preprocessors)
//
// Every texture and every mesh can be located from the chunk table alone, so a
// tool can read one of them from disk without loading the rest of the file:
//   1. read the first 64 bytes, n = mof_head_size(); read the first n bytes;
//   2. mof_find_chunk() for the record chunk (TEXT or MESH) and its data chunk
//      (TEXD, or VERT / INDX); read record i at entry.offset + i * record size;
//   3. the payload is at data_entry.offset + record offset (see docs/mof-format.md,
//      "Selective reading", for the range checks a tool must do on that record).
// Both functions validate the header and the complete chunk table exactly like
// mof_open does and need a 16-byte aligned buffer; they never read past `size`.

// Bytes from the start of the file that hold the header and the whole chunk
// table; 0 if `head` (at least 64 bytes) is not a valid .mof header.
uint64_t mof_head_size(const void* head, size_t size);
// Looks up a chunk by type in a buffer holding at least mof_head_size() bytes.
bool mof_find_chunk(const void* head, size_t head_size, uint32_t type, mof_chunk_entry* out, char* err, size_t errlen);

// ------------------------------------------------------------------- writer

// Allocator hook. realloc_fn(user, ptr, old_size, new_size): new_size == 0
// frees ptr (returns null); ptr == null allocates; returns null on failure.
struct mof_alloc {
    void* (*realloc_fn)(void* user, void* ptr, size_t old_size, size_t new_size);
    void* user;
};

// Growable output buffer (the file image). Initialise with mof_buffer_init.
struct mof_buffer {
    uint8_t* data;
    size_t size;
    size_t capacity;
    mof_alloc alloc;
};
void mof_buffer_init(mof_buffer* b, mof_alloc alloc);
void mof_buffer_free(mof_buffer* b);

// In-memory description. All pointers are borrowed and only read by mof_write.
// Use the mof_w*_default helpers for sane initial values.
struct mof_wsubmesh {
    uint32_t first_index;
    uint32_t index_count;
    int32_t material;  // -1 none
};

struct mof_wmesh {
    const char* name;
    uint32_t vertex_count;
    const float* positions;   // required, 3 per vertex
    const float* normals;     // 3, optional
    const float* tangents;    // 4, optional
    const float* uv0;         // 2, optional
    const float* uv1;         // 2, optional
    const uint8_t* colors;    // 4, optional
    const uint16_t* joints;   // 4, optional (needs weights_f32 or weights_u16)
    const float* weights_f32; // 4
    const uint16_t* weights_u16; // 4, unorm16 (used if weights_f32 is null)
    uint32_t index_count;
    const uint32_t* indices;  // triangle list
    uint32_t force_index32;   // nonzero: always write 32-bit indices
    const mof_wsubmesh* submeshes;  // none given -> one submesh covering all indices
    uint32_t submesh_count;
};

struct mof_wtexref { int32_t texture; uint32_t uv_set; };  // texture -1: none

struct mof_wmaterial {
    const char* name;
    float base_color[4];
    float emissive[3];
    float metallic, roughness;
    float normal_scale, occlusion_strength;
    uint32_t alpha_mode;  // MOF_ALPHA_*
    float alpha_cutoff;
    uint32_t double_sided;
    mof_wtexref tex[MOF_SLOT_COUNT];
};

struct mof_wtexture {
    const char* name;
    uint32_t codec;  // MOF_TEX_*
    uint32_t width, height, mip_count;
    const uint8_t* data;
    uint64_t data_size;  // textures with identical (data, data_size) share storage
    uint32_t wrap_s, wrap_t, min_filter, mag_filter, mip_filter;
};

struct mof_wbone {
    const char* name;
    int32_t parent;
    int32_t node;  // -1 none
    float translation[3], rotation[4], scale[3];
    float inverse_bind[16];
};

struct mof_wnode {
    const char* name;
    int32_t parent;
    int32_t mesh;  // -1 none
    uint32_t skinned;
    float translation[3], rotation[4], scale[3];
};

struct mof_wchunk {  // opaque extra chunk (unknown to readers of this version: skipped by them)
    uint32_t type, flags;
    const void* data;
    uint64_t size;
};

struct mof_wdesc {
    mof_alloc alloc;  // required
    uint32_t mesh_count;     const mof_wmesh* meshes;
    uint32_t material_count; const mof_wmaterial* materials;
    uint32_t texture_count;  const mof_wtexture* textures;
    uint32_t bone_count;     const mof_wbone* bones;
    uint32_t node_count;     const mof_wnode* nodes;
    uint32_t extra_chunk_count; const mof_wchunk* extra_chunks;
    uint32_t minor_version;  // 0 = MOF_VERSION_MINOR; lets tests emit other minors
    uint32_t skip_verify;    // nonzero: do not re-validate the output with mof_open
};

void mof_wmesh_default(mof_wmesh* m);
void mof_wmaterial_default(mof_wmaterial* m);
void mof_wtexture_default(mof_wtexture* t);
void mof_wbone_default(mof_wbone* b);
void mof_wnode_default(mof_wnode* n);

// Builds the file image into `out` (cleared first). Computes mesh bounds,
// picks 16- or 32-bit indices, interns names, aligns everything to 16 bytes.
// Unless desc->skip_verify, the result is re-opened with mof_open, so a true
// return guarantees a file every conforming reader accepts. On failure `out`
// is freed/empty and `err` gets a message.
bool mof_write(const mof_wdesc* desc, mof_buffer* out, char* err, size_t errlen);
