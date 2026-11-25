# CoreAnimation's `metallib_url`, and what is actually in `default.pipelinelib`

Status: static analysis only (no QEMU boot in this session). The URL/assert question was
settled empirically by the Lead's boot instrumentation; the static evidence below agrees with
it. Everything else (the LPPM container, `pipelines.desc`, the AIR corpus) is from reading the
20F66 assets and the shared cache.

Assets (read-only):

- `.../sysimage/rootfs/System/Library/Frameworks/QuartzCore.framework/` — `Info.plist`,
  `default.pipelinelib/{functions,pipelines.desc}`, `_CodeSignature/CodeResources`
  (`<key>files</key>` and `<key>files2</key>` are empty dictionaries).
- `.../sysimage/20F66__iPhone10,3_6/dyld_shared_cache_arm64` (+ 42 subcaches), arm64, unslid
  base `0x180000000`; QuartzCore image at **`0x18827b000`**.

All addresses below are un-slid virtual addresses from that cache.

## 0. Answers at a glance

| # | Question | Answer |
|---|---|---|
| 1 | What builds `metallib_url`, from what | `CA::OGL::new_metal_context()` at **`0x18847efcc`** (inlined `MetalContext` body; the assert's `__func__` is `MetalContext`). It does `+[NSBundle bundleWithIdentifier:@"com.apple.QuartzCore"]`, then `-[NSBundle URLForResource:withExtension:]` with **`default`** + **`metallib`**. |
| 2 | Exact path expected | `file:///System/Library/Frameworks/QuartzCore.framework/default.metallib` — an `NSURL` from the framework's `NSBundle`, flat-bundle resource lookup. No hard-coded path, no `CFBundleCopyResourceURL`. |
| 3 | Does CA open/parse it | No. The URL goes straight to `-[MTLDevice newLibraryWithURL:error:]`; Metal opens and parses it. Immediately after: `pathForResource:ofType:` (`default`/`pipelinelib`) → `-[MTLDevice newPipelineLibraryWithFilePath:error:]` → later `-[MTLPipelineLibrary newRenderPipelineStateWithName:options:reflection:error:]`. |
| 4 | What `functions`/`pipelines.desc` are | `functions` is an **LPPM** container of **451 single-function MTLB fragments** (AIR 2.5, LLVM bitcode) — the *specialised* CA renderer shaders, 444 fragment + 7 vertex, 4 distinct `fixed_*` base names. `pipelines.desc` is the matching metadata: 451 function descriptors (7 `.vert.` + 444 `.frag.`), **1079 render-pipeline names** `com.apple.coreanimation.draw.<spec>`, per-specialisation function constants, and one source-library label `default.metallib`. Mapping to the modules is **ordinal/name**, not hash. |

Bottom line for Stage C: the AIR for the precompiled CA pipeline path **is already in the guest
image** (`default.pipelinelib/functions`); the missing `default.metallib` is the *source* library
named in the desc and the runtime-specialisation library (it would carry the `fixed_*` base
functions, including the three `lpf_*` ones that the pipelinelib does **not** contain). It is not
needed to get past the assert, and it cannot be reconstructed byte-exactly from this image.

## 1. Method

- `ipsw` 3.1.749 (Linux x86_64 prebuilt) was downloaded, but it refuses this asset: the cache
  header makes it look for `dyld_shared_cache_arm64.symbols`, which is not shipped, and a
  fabricated one is rejected. So the cache was parsed directly.
- Small Python parser `/tmp/dscwork/dsc.py`: image array at main-cache file offset **`0x270`**
  (`imagesOffset` = header `0x1c0`, count 2710 at `0x1c4`, 32-byte `dyld_cache_image_info`
  records), subcache array at `0x2a7f0` (`0x188`/`0x18c`, 42 entries of 0x38: uuid, vm offset,
  suffix), then each subcache's own mapping table at `0x200` for VA→file translation.
  QuartzCore: VA `0x18827b000`, `__TEXT` 3,461,120 B ending `0x1885c8000`, `__LINKEDIT` at
  `0x216168000` in `dyld_shared_cache_arm64.42.dyldlinkedit`.
- Capstone 5.0.7 (`pip install --user --break-system-packages capstone`) for AArch64;
  function boundaries from `LC_FUNCTION_STARTS` (dataoff `0x7175f10` in
  `.42.dyldlinkedit`, 10,484 starts, first `0x18827d598`); LLVM 18 (`llvm-dis`) for the AIR
  modules; `$HAKIOS/tools/metallib.py` (read-only) for the MTLB fragments.
- Caveat: the symbol names in this asset's linkedit are **redacted** — 9,989 of 12,700
  QuartzCore `nlist` names are the literal string `<redacted>` (only C symbols such as
  `_NXCompareHashTables` survive). That is why function names here come from the existing
  `docs/metal-plan.md`/`docs/ru/metal.md` addresses and from structure, never from the symbol
  table. The assert strings themselves are not redacted.

## 2. The URL (short; static agrees with the empirical result)

`CA::OGL::new_metal_context()` — function `0x18847efcc`–`0x188480008` (0x103c bytes). The
`MetalContext` construction is inlined into it; the cold assert blocks carry the
`__func__`/file/line/expression strings:

```
0x18847f57c  ldr  x0, [x8,#0x168]        ; x0 = OBJC_CLASS_$_NSBundle  (classref 0x1dcf3a168)
0x18847f584  adrp x2, ... ; add x2,#0x838 ; x2 = @"com.apple.QuartzCore"  (CFString 0x1d5b5e838)
0x18847f58c  bl   0x1885bf020            ; stub: +[NSBundle bundleWithIdentifier:]
0x18847f590  mov  x23, x0
0x18847f594  cbz  x0, 0x18847fd28        ; -> __assert_rtn("MetalContext","ogl-metal.mm",3305,"quartzcore_bundle != nil")
0x18847f598  adrp x2, ... ; add x2,#0x958 ; x2 = CFString "default"   (0x1d5b5c958, len 7)
0x18847f5a0  adrp x3, ... ; add x3,#0x858 ; x3 = CFString "metallib"  (0x1d5b5e858, len 8)
0x18847f5ac  bl   0x1885bd600            ; stub: -[NSBundle URLForResource:withExtension:]
0x18847f5b4  cbz  x0, 0x18847fd4c        ; -> __assert_rtn("MetalContext","ogl-metal.mm",3308,"metallib_url != nil")
```

The cold block at `0x18847fd4c` is the exact assertion in the bug report:

```
0x18847fd50  add x0, x0, #0xecd    ; "MetalContext"        0x18857eecd
0x18847fd58  add x1, x1, #0xd1b    ; "ogl-metal.mm"        0x18857ed1b
0x18847fd60  add x3, x3, #0xefc    ; "metallib_url != nil" 0x18857eefc
0x18847fd64  mov w2, #0xcec        ; 3308
0x18847fd68  bl  0x18dbb89d0        ; __assert_rtn
```

Stub resolution (each `__objc_stubs` entry: `adrp x16,page; ldr x16,[x16,#off]; br x16`; the
slot is a chained rebase, target = `value & 0xFFFFFFFFF`):

| Stub | Selector |
|---|---|
| `0x1885bf020` | `bundleWithIdentifier:` |
| `0x1885bd600` | `URLForResource:withExtension:` |
| `0x1885c2ee0` | `newLibraryWithURL:error:` |
| `0x1885c3680` | `pathForResource:ofType:` |
| `0x1885c2f20` | `newPipelineLibraryWithFilePath:error:` |
| `0x1885c2f60` | `newRenderPipelineStateWithName:options:reflection:error:` |

So the resource is `default.metallib` in the QuartzCore framework bundle:
`/System/Library/Frameworks/QuartzCore.framework/default.metallib`.

Caller chain (window-server path, static): `CA::WindowServer::EAGLServer::renderer()` at
`0x1882e2548` calls `CAMetalContextCreate()` (`0x18837b718`) at `0x1882e25a8`, stores the
`MTLDevice` in `server+0x108`, then calls `CA::OGL::new_metal_context()` at `0x1882e25d0`
(args: device, 0, 0, 0, 0, 4, label), storing the renderer in `server+0x318`. The other
`new_metal_context` call sites in QuartzCore are `0x1882a0e78` (`CA::OGL::MetalContext::create`
path), `0x1882fa620`, `0x1883b8ad4`, `0x1883c604c`, `0x1884fea4c`, `0x1885330e8` and a tail branch
at `0x188437fb4` — every Metal client of CA reaches the same `MetalContext` construction and the
same line 3308.

Two details that matter for the injected dylib:

- QuartzCore references the **two-argument** `URLForResource:withExtension:` (it is one of the
  2,309 selector refs; `URLForResource:withExtension:subdirectory:` is *not* among them). It is
  Foundation's two-argument implementation that forwards to the three-argument one — which is
  why the Lead's hook of `-[NSBundle URLForResource:withExtension:subdirectory:]` intercepts the
  call. Hooking the two-argument method (or both) is the more local fix.
- No `CFBundle*` API is on this path. `CFBundleCopyResourceURL` appears only as an *imported
  symbol string* in the linkedit (unrelated use elsewhere); the code above is pure ObjC
  `NSBundle`.

## 3. What CA does immediately after the assert

1. `-[MTLDevice newLibraryWithURL:error:]` — `0x18847f5c0`, device in `x0` (the
   `MTLDevice` from `CAMetalContextCreate`), URL in `x2`, `&NSError` in `x3=&sp+0x38`.
   Result stored at `MetalContext+0xc50` (`0x18847f5c4`). `0x18847f5c8 cbz` jumps to the cold
   block that builds `"Metal failed to load library: %s"` from `[NSError localizedDescription]`
   (string `0x188580ef2`, format `"%s: %s"` at `0x18857ef10`) and continues.
2. `-[NSBundle pathForResource:ofType:]` with `default` / `pipelinelib` — `0x18847f6c4`
   (CFStrings `0x1d5b5c958`, `0x1d5b5e878`). `0x18847f6cc cbz` → assert **line 3335**
   (`0xd07`) `"pipelinelib_path != nil"` (`0x18847fe00`).
3. `-[MTLDevice newPipelineLibraryWithFilePath:error:]` — `0x18847f6d8`, result at
   `MetalContext+0xc58`; `0x18847f6e8` sets its label to
   `@"com.apple.coreanimation.pipeline-library"` (`0x1d5b5e898`); failure logs
   `"Metal failed to load pipeline library: %s"` (`0x188580f35`).
   A **nil** pipeline library is tolerated: `0x18847f804` calls
   `-[MTLPipelineLibrary setDisableRunTimeCompilation:1]` (stub `0x1885c4f60`, selector
   `setDisableRunTimeCompilation:`), which is nil-safe, and the constructor continues at
   `0x18847f808` (device property queries, `ctx+0xbb0`, limits, etc.).
4. Much later, when a render pipeline is requested,
   `0x18836cd48` (a small `MetalContext` method) checks `[x20,#0xc58]`, then calls
   `-[MTLPipelineLibrary newRenderPipelineStateWithName:options:reflection:error:]` at
   `0x18836ce38` with `options=0`, `reflection=0`, `&err`; a nil result logs
   `"Metal failed to load render pipeline: %s: %s"` (`0x188580fb3`).
   The name is a caller-supplied `NSString`; CA composes it from the prefix
   `com.apple.coreanimation.draw.` (`0x18857f949`, one occurrence in `__TEXT`) and a spec string.

CA itself performs **no** `open`/`mmap`/parse of either file: both are handed to Metal
(`newLibraryWithURL:`, `newPipelineLibraryWithFilePath:`). This matches the field observation
(the crash after the placeholder URL is inside Metal's `newPipelineLibraryWithFilePath:`).

## 4. What is in the image, and what `default.metallib` would be

### 4.1 The framework on disk

`QuartzCore.framework/` contains `Info.plist`, `_CodeSignature/CodeResources` (empty resource
dict), and `default.pipelinelib/{functions,pipelines.desc}`. There is **no** `default.metallib`
anywhere in the rootfs (9 `.metallib` files exist elsewhere: `AGXMetalA11.bundle/*_rt.metallib`,
`AppleMetalGLRenderer.bundle/default.metallib`, `PortraitFilters.cifilter/*.metallib`,
`AV1SW.metallib`). SHA-256:
`functions` = `1cb383a63bce78f4…69fcbe91`, `pipelines.desc` = `a6a43ffb97c34613…7bda27bb`.

### 4.2 `functions` — LPPM container of AIR modules

Header (little-endian, same layout in PencilKit/PassKit/etc. pipelinelibs):

```
0x00 "LPPM"  0x04 u32 1   0x08 u32 1   0x0c u32 0
0x10 u64 0x36b27c  (total file size, 3,584,636)
0x18 u64 0x28      (observed constant; likely header/naming-block size)
0x20 u64 0x54c0    (end of TOC = start of the module region)
0x28 u64 451       (module count)
```

TOC: 451 records of 0x30 bytes at `0x30`: `sha256[32]`, `u64 offset` (relative to `0x54c0`),
`u64 size`. The offsets tile `[0, 0x365dbc)` exactly (the whole module region).

Each TOC entry is a **complete single-function MTLB metallib fragment** (`MTLB` v1.2,
`total` = record size, `funcs=(0x58,0x96)`, `bitcode=(0x132,len)`, one `NAME`/`TYPE`/`HASH`/
`OFFT`/`VERS`/`MDSZ` set). Aggregate, all 451:

| property | value |
|---|---|
| defined functions per module | 1 (449 modules), 8 (2 modules) |
| function name (MTLB `NAME`) | `fixed_frag_lph_cpf` ×305, `fixed_frag_lph_cph` ×139, `fixed_vert_lph_spc` ×6, `fixed_vert_lph_gen` ×1 |
| type | 444 fragment, 7 vertex |
| `VERS` tag | `2.5.3.0` for all 451; `!air.version` metadata `{2,5,0}` |
| target triple | `air64-apple-ios16.5.0` for all 451 |
| bitcode | 451/451 unwrap from the `0x0B17C0DE` wrapper; `llvm-dis` (LLVM 18) disassembles all 451 with no error |

So this is not an opaque blob: it is CA's own Metal shader set, specialised. The modules are
real graphics entry points (`!air.vertex` ×7, `!air.fragment` ×444, 45 with
`!air.sampler_states`), with `%struct.CA::OGL::Metal::VertexShaderConfig` /
`FragmentShaderConfig` parameter blocks, render targets `color0/1/2`, fragment inputs
`generated(...)`, and baked sampler-state i64 constants. 70 distinct `air.*` names are used
(`air.any.v3i1` 397 modules, `air.sample_texture_2d.v4f16` 307, `air.fast_fmax.f32` 302,
`air.convert.f.v3f16.f.v3f32` 272, …). No `fc_*`/function-constant references remain in the
IR: the constants are baked in, which is what "specialised" means here.

TOC digests are **not** SHA-256 of the record, of the bitcode, or the MTLB `HASH` tag (tested on
several modules); their derivation is unresolved (see §8).

### 4.3 `pipelines.desc` — the metadata that names the functions and pipelines

1,593,060 bytes. Header: 12 `u32` = `11`, then 11 section offsets:
`0x30, 0x54, 0x7ab4, 0x7ab8, 0x14d28, 0x14d2c, 0x107d4c, 0x107d50, 0x184ed8, 0x184edc,
0x184ee0`. Decoded layout (offsets are section starts; adjacent offsets 4 bytes apart are
alignment gaps, all zero):

| section | range | content |
|---|---|---|
| S0 | `0x30..0x54` | source-library table (36 B): `01 08 01 08` then the only string **`default.metallib`** at `0x40` |
| S1 | `0x54..0x7ab4` | count 451 + 451 record offsets (rel. `0x54`); each record = 16-byte header `{2,12,16,index}` + cstring. Names: **7** `com.apple.coreanimation.vert.*` + **444** `com.apple.coreanimation.frag.*` (e.g. `…vert.VfxXgh`, `…frag.A2Xghfc`). |
| S2 | `0x7ab8..0x14d28` | count 1079 + records with short pipeline names `<format>_<function-code>` (e.g. `PBGRAXm_A2Xghfc`; the format part encodes pixel format/blend flags, with several hundred distinct values) |
| S3 | `0x14d2c..0x107d4c` | count 451 + records: 32-byte header, `fixed_*` name, `u32 nconst`, offset table, then per constant a 16-byte descriptor + `fc_*` name. Constant counts: 3 (the 1 gen), 4 (the 6 spc), 26 (2 cpf), 58 (303 cpf + 139 cph). 61 distinct `fc_*` names (e.g. `fc_base_color`, `fc_attachment_count`, `fc_vertex_layout`). |
| S4 | `0x107d50..0x184ed8` | count 1079 + records: descriptor header, then the full pipeline name **`com.apple.coreanimation.draw.<short>`** (all 1079), then more descriptor data |
| S5 | `0x184edc` | one `u32 0` |

**The relation between the two files is now clear:**

- S3 record *i* carries exactly the same `fixed_*` name, in the same order, as MTLB module *i*
  in `functions` — verified for all **451/451** (including the interleaved vertex/fragment
  order). Type alignment S1 vs modules is also **451/451**.
- There is no hash/offset cross-reference: none of the 451 TOC digests, none of the MTLB
  `HASH` tags, and no SHA-256 of the records occurs in `pipelines.desc`. The mapping is by
  **order and name**, with per-specialisation constants in S3.
- `default.metallib` occurs exactly **once**, in S0, with no hash and no data. The same S0 table
  with the same string exists in other pipelinelibs of the image (PencilKit's
  `PencilKit_iOS.pipelinelib` has the identical `01 08 01 08 "default.metallib"` header), so the
  string is the **source-library name recorded when the pipelinelib was built**, not a runtime
  reference to a file. The code is embedded in `functions`.
- S1 is a 451-entry function table (7 vertex + 444 fragment), S2/S4 are 1,079 render pipelines.
  **986 of the 1,079** pipeline names end with one of S1's 444 fragment codes (all 444 are
  referenced); the 7 vertex codes are not part of the pipeline name (the vertex function is
  selected by the format spec). **93** pipeline names embed function codes that do not occur
  among S1's 451 (e.g. `P0551Xm_TlutA2Xhf_Isrc_Isrc`, `PBGRABsovXm_Tds4A2Xhf_Isrc`). So the
  pipelinelib does not cover every pipeline CA could ask for by name — those would go through
  the runtime-specialisation path that needs the metallib.
- Other pipelinelibs in the image ship per-GPU compiled pipeline binaries
  (`pipelines.g4p`, `pipelines.g9g`, `pipelines.g10p`, …; PencilKit, PassKit, RenderBox,
  SiriUICore, …). **QuartzCore's `default.pipelinelib` has none** — only `functions` and
  `pipelines.desc`. Its pipelines are therefore AIR-only and have to be compiled by Metal at
  load/use time, which is exactly the same AIR→GPU gate.

### 4.4 So is the missing `default.metallib` "the CA shaders"?

Partly, and the distinction matters:

- The **precompiled path's shader code is present** in `functions`: 451 specialised AIR modules
  for the `lph_*` family under their `fixed_*` base names, matching S3 one-for-one.
- The **metallib would hold the base functions**, not these specialisations. QuartzCore's
  `__TEXT` names exactly **seven** `fixed_*` functions, selected through a CFString table by
  feature bits: fragment `fixed_frag_lph_cph`, `fixed_frag_lph_cpf`, `fixed_frag_lpf_cpf`;
  vertex `fixed_vert_lph_spc`, `fixed_vert_lph_gen`, `fixed_vert_lpf_spc`,
  `fixed_vert_lpf_gen` (CFStrings at `0x1d5b5eed8`…, table at `0x1d5b53db0` (frag) and
  `0x1d5b53f80` (vert), used at `0x18847c8c8` and `0x18847d4f0`; loader strings
  `"Loading shader function %s for %s"`, `"Specializing shader function %s for %s with constant
  values %s"`, `"Building render pipeline %s with descriptor %s"`, and the prefixes
  `com.apple.coreanimation.vert.`/`.frag.` at `0x18857f81b`/`0x18857f7b1`).
- The pipelinelib contains **only the four `lph_*` names** (305+139+6+1). The three `lpf_*`
  names (one fragment, two vertex) are **absent**. Those, and the 93 unmatched pipeline codes,
  are what the runtime path would need the metallib for.
- The metallib's exact bytes are **not recoverable** from this image (no copy anywhere in the
  rootfs, no hash of it recorded in the desc); the pipelinelib does not embed it either, it
  embeds equivalent specialised AIR.

For the assert alone none of this matters: any non-nil URL passes it. For an honest CA-on-Metal
later, the URL can point at a synthesised placeholder (as in the live experiment) or at an MTLB
rebuilt from the 451 fragments; the shader code to compile is the LPPM modules.

## 5. Stage C: AIR→SPIR-V

### 5.1 What the corpus is

`default.pipelinelib/functions` is a *second* AIR corpus, disjoint from the 456 modules in
`docs/air-report.md` (that corpus is only the `.metallib` files listed in `$HAKIOS/tools/air/check.sh`:
AV1SW, AppleMetalGLRenderer, PortraitFilters ×2). The 451 CA modules were never in it.

Properties relevant to a translator:

- 451/451 are ordinary LLVM bitcode (`llvm-dis` 18 parses all of them; AIR 2.5, target
  `air64-apple-ios16.5.0`), one graphics entry point each (two modules carry 8 internal
  functions too).
- 444 fragment / 7 vertex; metadata `!air.vertex`/`!air.fragment`, `air.render_target`,
  `air.fragment_input`, `air.vertex_output`, `air.buffer`/`air.location_index`/
  `air.address_space`/`air.arg_type_name`/`air.arg_name`, `air.sampler_states` (sampler state
  values are baked as i64 constants), `air.compile_options`.
- 70 distinct `air.*` intrinsic names. Against the HakiOS reader's own vocabulary
  (`$HAKIOS/tools/mesa/air/air_intrinsics.c`): 32 of the 35 operations are in its `strcmp` table,
  `sample_texture_2d`/`sample_texture_3d` are handled by the texture-name parser
  (`parse_tex_name`, same file), and **`air.unpack.unorm4x8.v4f16` has no handler anywhere in
  `$HAKIOS/tools/mesa/air/`** — it occurs in exactly **one** module, the generic vertex shader
  `fixed_vert_lph_gen` (module 0). Everything else (`fast_*`, `convert.*`, `dot`, `fwidth`,
  `dfdx/dfdy`, `mix`, `saturate`, `sample_texture_*`, …) appears in the reader's operation
  table.

### 5.2 Is the HakiOS AIR→NIR reader reusable?

For **reading and lowering the shader semantics, yes**: the CA modules are the same shape as the
271 fragment + 3 vertex modules the reader already translates in the existing corpus (entry
points, not `air.visible` library functions), and 34 of 35 operations plus all textures are
already implemented. The gap to check honestly is one module (`air.unpack.unorm4x8.v4f16`) and
the two multi-function modules.

But the reader's output is **NIR for ir3/GL**, not SPIR-V. Reusing it for Stage C means
choosing one of:

1. **AIR → NIR (reader) → SPIR-V.** Needs a NIR→SPIR-V emitter. Mesa ships SPIR-V→NIR only
   (`src/compiler/spirv`), so this means writing an emitter for the NIR subset these shaders use
   (ALU, texture ops, load/store, intrinsics) or reusing an external one. The value of the
   reader here is its intrinsic table and metadata parsing.
2. **AIR → standard LLVM IR → SPIR-V** (SPIRV-LLVM-Translator or LLVM's SPIR-V backend). The
   `air_intrinsics.c` table already documents the exact semantics of every `air.*` name
   (read off AGXCompilerCore), so the same lowering can be written as an LLVM pass; the
   metadata (`air.buffer`, `air.location_index`, `air.render_target`, fragment/vertex I/O) still
   has to become SPIR-V bindings/decorations by hand.
3. A hybrid: compile the 451 modules with the existing reader+NIR for a CPU/GL reference, and
   only feed the *pipeline pairs* (vertex+fragment, 1,079 names) through the SPIR-V emitter.

Either way the missing pieces are the same: (a) `air.unpack.unorm4x8.v4f16`; (b) lowering of the
70 intrinsic names (32+2 already have a written spec in `air_intrinsics.c`); (c) Metal argument
metadata → Vulkan descriptor/push-constant bindings and interpolation/location mapping;
(d) baked `air.sampler_states` → `VkSampler` objects; (e) render-target formats from
`pipelines.desc` S4. The shaders are already specialised, so no function-constant
specialisation stage is needed for these 451 (only the metallib path needs that).

### 5.3 Smallest honest gate

1. **Reader gate (half a day):** add an LPPM extractor (the 451 fragments are already extracted
   in `/tmp/ca_air/all/` by the work behind this report; `$HAKIOS/tools/metallib.py` parses each fragment)
   and run the existing host `air2nir`/`corpus.py` over the 451 modules. Expect **450/451** to
   parse+translate as-is; the one failure is module 0, `fixed_vert_lph_gen`, on
   `air.unpack.unorm4x8.v4f16`. Implement that one intrinsic; gate = 451/451 translate.
2. **SPIR-V gate (the real work):** take one fragment (e.g. `fixed_frag_lph_cph`, module 100,
   4.7 KB bitcode) and one vertex (module 0) through whichever emitter is chosen; require
   `spirv-val --target-env vulkan1.1` clean. Then 451/451, then the pair through the stage29
   bridge. This is a per-module gate, not a whole-pipeline one.
3. **Pipeline gate:** assemble one pipeline from `pipelines.desc` S4 (name
   `com.apple.coreanimation.draw.<spec>`), map the short name to S1/S3/module index, and draw
   through the Metal→Vulkan path; until then, the stubbed pipeline library (§6) is the only way
   to keep CA alive.

## 6. Stubbing `newPipelineLibraryWithFilePath:` and the by-name API

API shape, as CA uses it (all from the cache):

- `-[MTLDevice newPipelineLibraryWithFilePath:error:]` (selector ref resolved at QuartzCore stub
  `0x1885c2f20`) returns an `MTLPipelineLibrary` (or nil). CA stores it at `MetalContext+0xc58`,
  calls `setLabel:` with `com.apple.coreanimation.pipeline-library`, and tolerates **nil**
  (it logs `Metal failed to load pipeline library: <localizedDescription>` and keeps
  initialising the context at `0x18847f808`).
- `-[MTLPipelineLibrary newRenderPipelineStateWithName:options:reflection:error:]`
  (stub `0x1885c2f60`, called at `0x18836ce38` with `options=0`, `reflection=0`) returns an
  `MTLRenderPipelineState` (or nil). CA checks the library pointer before the call, and a nil
  state only logs `Metal failed to load render pipeline: <name>: <description>`.
- The name CA passes is an `NSString` it composes. The one prefix string in `__TEXT` is
  `com.apple.coreanimation.draw.` (`0x18857f949`); the suffix is the `<format>_<function>` spec
  from the renderer's function table. `pipelines.desc` S4 holds exactly the 1,079 resulting
  full names (`com.apple.coreanimation.draw.PBGRAXm_A2Xghfc`, …), so the names CA will ask for
  can be enumerated offline without running the device.

**No file parsing is needed to stub this.** CA also calls the private
`-[MTLPipelineLibrary setDisableRunTimeCompilation:]` (`1`) right after loading, so the stub
class must implement that selector too. A `_MTLPipelineLibrary` subclass (or any object that
declares the protocol) that answers `setDisableRunTimeCompilation:` and returns a stub
`_MTLRenderPipelineState` for every name makes CA proceed; logging the names recovers the exact
pipeline set for the offline path. The
`functions`/`pipelines.desc` files can be ignored entirely for the stub; only a real AIR→SPIR-V
backend needs them.

## 7. Recommended interposition (minimum viable)

- Hook `-[NSBundle URLForResource:withExtension:]` (and, as the live experiment proved,
  `-[NSBundle URLForResource:withExtension:subdirectory:]` — Foundation's two-argument
  implementation funnels through it). When `name == "default"` and `extension == "metallib"`
  on the QuartzCore bundle, return a non-nil `fileURLWithPath:` to a placeholder file, e.g.
  `/redwood/share/ca-default.metallib`. Keep it a real resolvable file URL if any later code
  asks for `checkResourceIsReachable`; the content is never read by CA.
- Do **not** hook `CFBundle*`; CA does not use it here.
- Leave `pathForResource:ofType:` alone: `default.pipelinelib` exists on disk, so
  `pipelinelib_path` is naturally non-nil.
- Then stub the device side: `-newLibraryWithURL:error:` may return a stub `MTLLibrary`;
  `-newPipelineLibraryWithFilePath:error:` should return a stub `MTLPipelineLibrary`
  (non-nil, so the `Metal failed to load pipeline library` path is not taken) and
  `-newRenderPipelineStateWithName:options:reflection:error:` a stub render pipeline state.
  That gets CA through context creation without Metal touching either file.

## 8. What remains uncertain, and how to settle it

1. **The exact contents of `default.metallib`.** Not present in this image and not hashed in
   `pipelines.desc`. The binary shows it would carry the seven `fixed_*` base functions (four
   `lph_*` already covered by `functions`, three `lpf_*` not covered). Settle by obtaining the
   same 20F66 IPSW's QuartzCore resource from a device/image that still has it, or by boot
   instrumentation of `-[MTLLibrary functionNames]` on a Metal-capable host.
2. **The TOC digests** in `functions` (32 B per module) are not SHA-256 of the record, the
   bitcode, or the MTLB `HASH` tag. Probably a digest of Apple's source descriptor; irrelevant
   to the stub path. Could be settled by finding the producer (metal-archive tool) or by
   checking a second pipelinelib's digests against its modules.
3. **The 93 pipeline names whose function codes are not among S1's 451.** They suggest the
   metallib/runtime path is needed for part of CA's pipeline set. Settle by logging the names
   passed to `newRenderPipelineStateWithName:` during a boot (the stub gives them for free) and
   intersecting with S4.
4. **S4/S2 descriptor header encoding** (the 16/26/32-byte pre-name headers and the constant
   16-byte descriptors) is only partially decoded; names, counts and constants are certain, the
   bitfield meanings are not. Needed only for a real pipeline-library implementation.
5. **Whether the precompiled path is even enabled on this port.** `new_metal_context` has four
   gates that log "Precompiled pipelines disabled …" (debug option, renderer feature flag, GPU
   frame capture, unsupported hardware at `0x1e1f6a159`). If the unsupported-hardware gate wins
   on the VM, CA will take the runtime-specialisation path and require the metallib's functions
   for real, not just a URL. The boot log will say which gate fired; the stub-library approach
   works either way until rendering is attempted.
6. **The two modules with 8 defined functions** — the reader's per-module assumptions should be
   checked when the reader gate is run.

## 9. Evidence appendix

Key addresses (unslid):

| What | Address / offset |
|---|---|
| QuartzCore image | `0x18827b000` |
| `CA::OGL::new_metal_context()` | `0x18847efcc`–`0x188480008` |
| assert `quartzcore_bundle != nil` | line 3305, cold block `0x18847fd28` |
| assert `metallib_url != nil` | line 3308, cold block `0x18847fd4c` |
| assert `pipelinelib_path != nil` | line 3335, cold block `0x18847fdfc` |
| `URLForResource:withExtension:` call | `0x18847f5ac` (stub `0x1885bd600`) |
| `newLibraryWithURL:error:` call | `0x18847f5c0` (stub `0x1885c2ee0`) |
| `newPipelineLibraryWithFilePath:error:` call | `0x18847f6d8` (stub `0x1885c2f20`) |
| `newRenderPipelineStateWithName:…` call | `0x18836ce38` (stub `0x1885c2f60`) |
| pipeline name prefix `com.apple.coreanimation.draw.` | `0x18857f949` |
| `fixed_*` CFString table | `0x1d5b53db0` (frag), `0x1d5b53f80` (vert) |
| assert strings | `0x18857eefc` `metallib_url != nil`, `0x18857eeda` `quartzcore_bundle != nil`, `0x18857effd` `pipelinelib_path != nil`, `0x18857eecd` `MetalContext`, `0x18857ed1b` `ogl-metal.mm` |

Commands used (reproducible):

```sh
# cache: no ipsw (it wants dyld_shared_cache_arm64.symbols); custom parser instead
python3 /tmp/dscwork/dsc.py          # image array @0x270, subcaches @0x2a7f0, mapping @0x200
# QuartzCore at 0x18827b000; __TEXT dumped from .01 subcache
# function starts: LC_FUNCTION_STARTS dataoff 0x7175f10 in .42.dyldlinkedit -> 10484 starts
# disassembly: capstone 5.0.7 over __text, 0x18827d598..0x188546a2c
# selector resolution: __objc_stubs + __objc_selrefs, chained-rebase target = value & 0xFFFFFFFFF
# AIR: tools/metallib.py parse of each LPPM fragment; llvm-dis (LLVM 18) on all 451 bitcode blobs
# desc: Python, header at 0..0x30, sections S0..S5 as in 4.3
```

Key strings and numbers: 451 modules; 305 `fixed_frag_lph_cpf` + 139 `fixed_frag_lph_cph` +
6 `fixed_vert_lph_spc` + 1 `fixed_vert_lph_gen`; 444 fragment + 7 vertex; 1079 pipelines;
61 distinct `fc_*` names; 70 distinct `air.*` names; 986/1079 pipeline names matched to S1
fragment codes; `functions` SHA-256 `1cb383a6…69fcbe91`, `pipelines.desc` SHA-256
`a6a43ffb…7bda27bb`.

## 10. Addendum: the live crash after the placeholder pipeline library (Metal +0xa7f7c)

During the Lead's boot experiment `newPipelineLibraryWithFilePath:error:` returned the
placeholder, and the next Metal instruction executed faulted at `0x181ab9f7c` (Metal.framework,
image base `0x181a12000`). Resolved statically:

- `LC_FUNCTION_STARTS` for Metal (`dataoff 0x71629d0` in `.42.dyldlinkedit`, 8,705 starts)
  puts `0x181ab9f7c` at **+0x30 inside function `0x181ab9f4c`** (size 0x54, next start
  `0x181ab9fa0`).
- That function **is an ObjC method IMP**: class **`_MTLPipelineLibrary`**, method-list entry 1
  (method lists decode with `IMP = (entry+8) + impOffset`; type offset likewise relative to
  `entry+8`, giving type encoding `v20@0:8B16` — a setter taking one `BOOL`).
- The QuartzCore caller fixes the name: `0x18847f7fc ldr x0,[x21,#0xc58]` (the pipeline
  library), `0x18847f800 mov w2,#1`, `0x18847f804 bl 0x1885c4f60` (objc stub),
  `0x18847f808 ...` (lr of the crash). Stub `0x1885c4f60`'s selref is `0x1d5b80170`, whose
  chained target is selector string `0x1824d225d` = **`setDisableRunTimeCompilation:`**.
  Therefore the crashing method is `-[_MTLPipelineLibrary setDisableRunTimeCompilation:]`,
  called by CA immediately after loading the pipelinelib and before any pipeline creation.
- The fault itself: `0x181ab9f64 ldrsw x8,[0x1dcf0a560]; 0x181ab9f6c strb w2,[x0,x8]` stores
  the flag; `0x181ab9f70 ldrsw x21,[0x1dcf0a55c]; 0x181ab9f78 ldr x8,[x0,x21]` reads an object
  ivar; `0x181ab9f7c ldr x0,[x8,#0x10]` dereferences it (+0x10 and later +0x18). A placeholder
  that inherits the real method but has none of `_MTLPipelineLibrary`'s ivars has `x8 == 0`
  there, hence the fault address `0x10`. One no-op override of
  `setDisableRunTimeCompilation:` (like `setLabel:`) removes this wall; no file parsing.

For reference, the previous crash `0x181a3ca3c` is at **+0x344 inside helper `0x181a3c6f8`**
(size 0x66c, next start `0x181a3cd64`), which is *not* an ObjC method entry; it is called from
`0x181a3d9b8` by `_MTLDevice` method IMP **`0x181a3d9b0`** whose type encoding is
`@24@0:8@16^@24` — consistent with `-[_MTLDevice newPipelineLibraryWithFilePath:error:]`. So
the method attribution was right, but the pc was in its error-path helper, not at the method
entry. The IMP/function-start decode was validated on all 5,374 ObjC methods of Metal.framework:
every decoded IMP is exactly a function start.

### 10.1 Second live fault: `0x18db56628` (libdispatch) with lr `0x181a37930` (Metal)

- Metal lr `0x181a37930` is inside **`-[_MTLCommandQueue dealloc]`**, IMP **`0x181a37898`**
  (class `_MTLCommandQueue`, type `v16@0:8`, method-list entry 14; the function's log string at
  `0x1825653cb` is the literal "dealloc").
- The unguarded call is `0x181a37928 ldr x0,[x19,#0xb0]; 0x181a3792c bl 0x181b30430` with lr
  `0x181a37930`. The offset global `0x1dcf0aef4` holds **0xb0**; `_MTLCommandQueue`'s ivar list
  names offset `0xb0` **`_submittedGroup`**, type `@"NSObject<OS_dispatch_group>"`. Unlike the
  neighbouring releases in the same dealloc (all `cbz`-guarded), this one has no nil check.
- Metal stub `0x181b30430` → Metal `__got` slot `0x1dadda990` → libdispatch symbol stub
  `0x18db94880` → lazy binder calls libdispatch's resolver **`_dispatch_release` at
  `0x18db84910`**, which selects a `$VARIANT` implementation; the selected one is
  **`0x18db56620`** (libdispatch base `0x18db23000`, +0x33620). The fault pc `0x18db56628` is
  its first dereference, `ldr x8,[x0]` — x0 is the nil `_submittedGroup`, so the fault address is
  `0x0`.
- Frames: QuartzCore `+0x1fb534` = `0x188476534`, inside the renderer virtual method
  `0x188475eec–0x188476784`; the return address follows `bl 0x188549a4c`, the stub for
  **`_objc_autoreleasePoolPop`** (libobjc `0x1800bc4b4`). `+0x6763c` = `0x1882e263c` is inside
  `CA::WindowServer::EAGLServer::renderer()` after `blr x8` (`vtable[0x98]`), i.e. renderer()
  called that method. `+0x162cbc` = `0x1883ddcbc` is in the epilogue of the small KVC helper
  `0x1883ddc58–0x1883ddcd4` (`numberWithLongLong:`, `stringWithUTF8String:`,
  `setValue:forKey:`) — not a call site, so that frame is stale/saved LR. libobjc `+0x45b0` =
  `0x1800bc5b0` is inside `_objc_autoreleasePoolPop`; `+0x11a8` = `0x1800b91a8` is inside
  libobjc's AutoreleasePoolPage release loop `0x1800b90e8–0x1800b9208` (0xa1a1/0xa3a3 magics),
  which sends `-release` to the pool's objects. `0xb84a4cd0` is outside the shared-cache address
  range and is not a code address (saved data/isa or corrupt frame).
- Implication for the plug-in: a fake `_MTLCommandQueue` must either skip `_MTLCommandQueue`'s
  real `dealloc` (do not call `[super dealloc]`) or create `_submittedGroup` at offset `0xb0`
  with `dispatch_group_create()`. This is the same failure shape as
  `setDisableRunTimeCompilation:` — an inherited real method body reading ivars the stub never
  initialised — and it does not involve the pipeline/AIR path.

## 11. Third live fault: `0x181aec43c` (Metal), lr `0x181aec420` — library function creation, not device pipeline creation

This is the fault after the plug-in began answering a specialized function for `fixed_frag_lpf_cpf`.
Resolved statically:

- `LC_FUNCTION_STARTS` (Metal, dataoff `0x71629d0`): pc `0x181aec43c` is **+0x78 inside
  `0x181aec3c4`–`0x181aec510`** (function size 0x14c). The function is an ObjC method IMP of
  **`_MTLLibrary`** (method-list entry 36) with type encoding
  **`@48@0:8@16@24@32^@40`** — receiver, `_cmd`, three object arguments, `NSError**` (i.e. a
  three-argument library SPI whose failure error string, CFString at `0x1d59e4630`, is
  **"Function %@ was not found in the library"**).
- The faulting instructions:
  ```
  0x181aec430 adrp x8, #0x1dcf0a000
  0x181aec434 ldrsw x8, [x8, #0x8f0]     ; x8 = 0x30  (_MTLLibrary._cacheEntry ivar offset)
  0x181aec438 ldr  x8, [x21, x8]         ; x8 = self->_cacheEntry      (nil for the fake library)
  0x181aec43c ldr  x23, [x8]             ; <-- fault addr 0x0
  ```
  So the object being dereferenced is the receiver (`x21`, the fake `_MTLLibrary`): it has no
  ivar at offset **0x30**, which `_MTLLibrary`'s ivar list names **`_cacheEntry`**
  (type `^{MTLLibraryContainer=^{MTLPipelineCollection}Ai{?=[32C]}^{MTLLibraryData}@@@@@}`).
  lr `0x181aec420` is the return address of `0x181aec41c bl 0x181bc9760` (Metal-internal
  cache lookup/insert helper); the crash happens immediately after that helper returned.
- The call that returns to `0x181aec554`: `0x181aec550 bl 0x181bc97a0`, a Metal ObjC selector
  stub (slot `0x1d5a27328`) whose selector is
  **`newSpecializedFunctionWithDescriptorInternal:destinationArchive:functionCache:error:`**.
  The containing function `0x181aec51c`–`0x181aec65c` is `_MTLLibrary` method-list entry 34,
  type `@40@0:8@16@24^@32` (two object arguments + error); its body is the small wrapper that
  forwards to the three-argument "Internal" specialization variant with `functionCache = 0` and
  `&error`. That forwarding call lands in the crashing function `0x181aec3c4`.
- QuartzCore frames (all return addresses; base `0x18827b000`):
  - `0x188479b90` → function **`0x188479918`–`0x188479c20`**. It builds an
    `MTLFunctionDescriptor` (`functionDescriptor` getter on `[0x1dcf3a3c0]`, `setName:`,
    `setConstantValues:`) and at `0x188479b8c` calls objc stub `0x1885c2ea0`, selector
    **`newFunctionWithDescriptor:error:`** (selref slot `0x1d5b7f210` → `0x183273b68`), on
    `x20`, which the prologue took from incoming `x1` — the library (`[MetalContext+0xc50]`).
  - `0x18847d22c` → function **`0x18847c810`–`0x18847d454`**, return from
    `0x18847d228 bl 0x188479918`; it passes `x1 = [ctx+0xc50]` (library), name/descriptor args
    and `&err`.
  - `0x188378284` → function **`0x188378090`–`0x188379434`**, return from
    `0x188378280 bl 0x18847c810`. This is the render-pipeline factory: it also calls
    `-[MTLDevice newRenderPipelineStateWithDescriptor:error:]` further down
    (`0x188379098`, `0x1883790f0`), i.e. the device pipeline call comes *after* the function
    creation that is failing here.
  - The remaining frames are its callers: `0x1882c3548` in `0x1882c12ac`–`0x1882c364c`
    (calls `0x188378090`); `0x1882ccc64` in `0x1882ccbf0`–`0x1882cccb0` (calls `0x1882c12ac`);
    `0x1882fa9dc` in `0x1882fa894`–`0x1882faa48` (calls `0x1882eeb44`); `0x1882f5da8` in
    `0x1882f5cc0`–`0x1882f5fa4` (calls `0x1882fa894`); `0x1882e2b60` in
    `0x1882e2a6c`–`0x1882e372c` (calls `0x1882f5cc0`).

**Conclusion.** The hypothesis that this pc is
`-[_MTLDevice newRenderPipelineStateWithDescriptorInternal:options:reflection:destinationBinaryArchive:error:completionHandler:]`
is **not** supported: the immediate failing call is CoreAnimation's
`-[<library> newFunctionWithDescriptor:error:]`, and the crash is inside `_MTLLibrary`'s inherited
function-creation path (it looks the function up in `_cacheEntry`). The device pipeline SPI sits
later in the same CoreAnimation stack (`0x188378090`) and is likely the next wall once this one is
removed.

Fix options for the plug-in: override
`-newFunctionWithDescriptor:error:` on the fake **library** class (return a placeholder function,
short-circuiting Metal's lookup), and/or override the exact SPI selector
`newSpecializedFunctionWithDescriptorInternal:destinationArchive:functionCache:error:` — note the
cache spelling contains **`DescriptorInternal:`**, so a selector declared as
`newSpecializedFunctionWithDescriptor:...` (without `Internal`) will not intercept it. Merely
setting `_cacheEntry` to a real `MTLLibraryContainer` is not practical from the stub.
