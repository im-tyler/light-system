#!/usr/bin/env python3
"""Deterministic textured benchmark scene: a UV-mapped heightfield grid.

Connected geometry with TEXCOORD_0 so the textured payload path (base
clusters, LOD clusters with attribute-aware simplification, UV
interpolation, texture sampling) can be exercised end to end. UVs tile
[0, 4] across the grid, so the embedded checker texture repeats four times
per axis and texel variation is verifiable from a screenshot.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path


def height(x: int, z: int, columns: int, rows: int) -> float:
    fx = x / columns
    fz = z / rows
    h = (
        0.35 * math.sin(fx * 6.0 * math.pi) * math.cos(fz * 4.0 * math.pi)
        + 0.2 * math.sin((fx + fz) * 9.0 * math.pi)
        + 0.1 * math.sin(fx * 21.0 * math.pi)
    )
    terrace = 0.3 * math.floor(2.0 * (0.35 * math.sin(fx * 3.0 * math.pi) + 0.5))
    return h + terrace


def build_grid(columns: int, rows: int) -> tuple[list[float], list[float], list[float], list[int]]:
    positions: list[float] = []
    texcoords: list[float] = []
    indices: list[int] = []
    for z in range(rows + 1):
        for x in range(columns + 1):
            positions.extend([x / columns, height(x, z, columns, rows), z / rows])
            texcoords.extend([4.0 * x / columns, 4.0 * z / rows])
    for z in range(rows):
        for x in range(columns):
            v0 = z * (columns + 1) + x
            v1 = v0 + 1
            v2 = v0 + columns + 1
            v3 = v2 + 1
            indices.extend([v0, v2, v1, v1, v2, v3])
    return positions, texcoords, indices


def write_buffer(output_dir: Path, positions: list[float], texcoords: list[float],
                 indices: list[int]) -> tuple[list[dict], list[dict]]:
    buffer_path = output_dir / "textured_grid.bin"
    position_bytes = struct.pack(f"<{len(positions)}f", *positions)
    texcoord_bytes = struct.pack(f"<{len(texcoords)}f", *texcoords)
    index_bytes = struct.pack(f"<{len(indices)}I", *indices)
    buffer_path.write_bytes(position_bytes + texcoord_bytes + index_bytes)

    texcoord_offset = len(position_bytes)
    index_offset = texcoord_offset + len(texcoord_bytes)
    buffer_views = [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(position_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": texcoord_offset, "byteLength": len(texcoord_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": index_offset, "byteLength": len(index_bytes), "target": 34963},
    ]
    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(positions) // 3, "type": "VEC3",
         "min": [min(positions[i::3]) for i in range(3)],
         "max": [max(positions[i::3]) for i in range(3)]},
        {"bufferView": 1, "componentType": 5126, "count": len(texcoords) // 2, "type": "VEC2"},
        {"bufferView": 2, "componentType": 5125, "count": len(indices), "type": "SCALAR"},
    ]
    return buffer_views, accessors


def write_gltf(output_path: Path, columns: int, rows: int) -> None:
    positions, texcoords, indices = build_grid(columns, rows)
    buffer_views, accessors = write_buffer(output_path.parent, positions, texcoords, indices)
    gltf = {
        "asset": {"version": "2.0", "generator": "light-system textured grid"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                "indices": 2,
                "material": 0,
                "mode": 4,
            }],
        }],
        "materials": [{"name": "textured_grid"}],
        "buffers": [{"uri": "textured_grid.bin", "byteLength":
                     buffer_views[-1]["byteOffset"] + buffer_views[-1]["byteLength"]}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    output_path.write_text(json.dumps(gltf, indent=2))
    print(f"wrote {output_path} with {len(positions) // 3} vertices / {len(indices) // 3} triangles")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--columns", type=int, default=96)
    parser.add_argument("--rows", type=int, default=96)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_gltf(args.output, args.columns, args.rows)


if __name__ == "__main__":
    main()
