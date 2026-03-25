# VGeo Resource Schema

Last updated: 2026-03-23

This is the first-pass logical schema for a dense-geometry resource.

It is intentionally implementation-agnostic and should be refined before binary serialization is frozen.

## Goals

- represent clustered rigid geometry
- support hierarchical LOD
- support page streaming
- preserve a fallback path for unsupported runtimes
- leave room for future compressed and alternate geometry representations

## Logical Sections

## 1. Header

Required fields:

- magic
- schema version
- builder version
- source asset identifier
- source asset hash
- import settings hash
- total cluster count
- total hierarchy node count
- total page count
- material slot count
- bounds
- fallback mesh presence flag

## 2. Resource Metadata

Required fields:

- root hierarchy node index
- page table offset or reference
- page dependency table offset or reference
- node lod link table offset or reference
- cluster table offset or reference
- cluster geometry payload offset or reference
- lod geometry payload offset or reference
- material mapping offset or reference
- optional debug info offset or reference

## 3. Material Mapping

The canonical material truth should be:

- material section table keyed by material slot / fallback section identity
- per-cluster material section index
- per-lod-cluster material section index
- fallback section reference

Reverse material-to-cluster ranges are optional derived metadata, not the primary source of truth.

## 4. Hierarchy Nodes

Each node represents a selectable cut element in the LOD hierarchy.

In the current prototype, clusters are packed in hierarchy traversal order so `first_cluster_index` and `cluster_count` always describe a real contiguous range.

Required fields:

- node index
- parent index or invalid root marker
- first child index
- child count
- first cluster index
- cluster count
- first lod link index
- lod link count
- bounds
- geometric error
- min resident page
- max resident page
- flags

## 5. Clusters

Required fields per cluster:

- cluster index
- owning hierarchy node index
- local vertex count
- local triangle count
- geometry payload offset
- geometry payload size
- page index
- bounds
- normal cone or directional culling data
- local error
- material section index
- flags

## 6. Page Table

Required fields per page:

- page index
- page kind / payload domain
- byte offset in file
- compressed byte size
- uncompressed byte size
- first cluster index
- cluster count
- first lod cluster index
- lod cluster count
- dependency page range or list reference
- flags

In the current prototype, base-cluster pages and lod-cluster pages share one page table, but page kind makes the payload domain explicit.
The dependency list is currently a soft prefetch-hint list for adjacent replacement levels of the same exact-match span, not a hard prerequisite list.

## 7. Node LOD Links

Required fields per link:

- lod group index

In the current prototype, links are emitted only when a hierarchy node covers the exact base-cluster span represented by the linked LOD groups.
In practice this is resolved by builder-side provenance tracking over source clusters, not by heuristic matching on bounds or depth.

## 8. LOD Groups

Required fields per group:

- depth
- first lod cluster index
- lod cluster count
- bounds
- geometric error
- flags

## 9. LOD Clusters

Required fields per LOD cluster:

- refined group index
- group index
- local vertex count
- local triangle count
- geometry payload offset
- geometry payload size
- page index
- bounds
- local error
- material section index
- flags

## 10. Geometry Payloads

The initial payload can be uncompressed but must conceptually support separate sections for:

- base-cluster vertex/index payload blocks
- lod-cluster vertex/index payload blocks
- optional attribute blocks
- optional compressed blocks

## 11. Fallback Mesh Reference

Required if fallback support is emitted:

- fallback mesh identifier or embedded payload reference
- material section mapping

## 12. Debug Metadata

Optional:

- original submesh names
- builder diagnostics
- cluster coloring seeds
- source triangle statistics

## Flags To Reserve

Reserve flags now for future use:

- compressed payload
- procedural representation present
- foliage candidate
- RT metadata present
- debug data present

## Open Questions

- whether hierarchy nodes and clusters should be stored as separate tables or partially fused
- whether page dependencies should be explicit or inferred
- which fields must remain 32-bit versus 64-bit
- when material mapping should be fully embedded versus external
