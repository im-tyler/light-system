# Page Layout Schema

Last updated: 2026-03-23

This document describes the first-pass logical page layout used by the streaming system.

## Design Goals

- small enough for responsive streaming
- large enough for good locality and amortized upload cost
- independent of a single compression scheme
- future-compatible with GPU-side decompression

## Logical Page Structure

Each page should contain:

1. page header
2. cluster record slice
3. geometry payload slice
4. optional attribute payload slice
5. optional compression metadata

## Page Header

Required fields:

- page index
- schema version
- flags
- compressed size
- uncompressed size
- cluster count
- first cluster index
- dependency metadata reference
- checksum or validation field

## Page Contents

### Cluster record slice

Stores the cluster records that become meaningful only when the page is resident.

### Geometry payload slice

Stores:

- local positions
- local indices
- optional packed normals
- optional UV and tangent payloads

### Optional compression metadata

Reserve space for:

- compression codec identifier
- block table
- decode scratch requirements

## Residency Rules

The runtime should assume:

- a page is the minimum residency unit
- a missing page must not stall the frame synchronously
- clusters referencing nonresident pages are skipped or replaced until residency is satisfied

## Scheduler Hints

Each page should be schedulable using:

- current frame visibility
- parent/child hierarchy demand
- shadow demand
- camera velocity and direction
- memory pressure

## First-Pass Constraints

Before the format is frozen, answer:

1. target page size range
2. whether pages can contain mixed material sections
3. whether parent and child hierarchy cuts should be near each other physically
4. whether hot metadata should be split from cold payload bytes

## Recommended Initial Bias

Start with:

- simple contiguous metadata tables
- one compressed or uncompressed payload blob per page
- CPU-side decompression only

Then optimize once the benchmark harness and prototype expose real bottlenecks.
