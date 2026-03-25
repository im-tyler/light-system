"""Generate a massive city block scene targeting 2M+ triangles."""
import struct, json, math, random, os

def add_box(P, N, I, cx, cy, cz, sx, sy, sz):
    base = len(P) // 3
    faces = [
        ((-sx,sy,-sz),(sx,sy,-sz),(sx,sy,sz),(-sx,sy,sz),(0,1,0)),
        ((-sx,-sy,sz),(sx,-sy,sz),(sx,-sy,-sz),(-sx,-sy,-sz),(0,-1,0)),
        ((sx,-sy,-sz),(sx,sy,-sz),(sx,sy,sz),(sx,-sy,sz),(1,0,0)),
        ((-sx,-sy,sz),(-sx,sy,sz),(-sx,sy,-sz),(-sx,-sy,-sz),(-1,0,0)),
        ((-sx,-sy,sz),(sx,-sy,sz),(sx,sy,sz),(-sx,sy,sz),(0,0,1)),
        ((sx,-sy,-sz),(-sx,-sy,-sz),(-sx,sy,-sz),(sx,sy,-sz),(0,0,-1)),
    ]
    for v0,v1,v2,v3,n in faces:
        for v in [v0,v1,v2,v3]:
            P.extend([cx+v[0],cy+v[1],cz+v[2]])
            N.extend(n)
        i=base; I.extend([i,i+1,i+2,i,i+2,i+3]); base+=4

def add_cylinder(P, N, I, cx, cy, cz, radius, height, segments=12):
    base = len(P) // 3
    for i in range(segments):
        a0 = 2*math.pi*i/segments
        a1 = 2*math.pi*(i+1)/segments
        x0,z0 = math.cos(a0)*radius, math.sin(a0)*radius
        x1,z1 = math.cos(a1)*radius, math.sin(a1)*radius
        nx0,nz0 = math.cos(a0), math.sin(a0)
        nx1,nz1 = math.cos(a1), math.sin(a1)
        # Side quad
        b = len(P)//3
        for px,pz,nx,nz in [(x0,z0,nx0,nz0),(x1,z1,nx1,nz1),(x1,z1,nx1,nz1),(x0,z0,nx0,nz0)]:
            py = -height/2 if (px==x0 and pz==z0 and len(P)//3==b) or (px==x1 and pz==z1 and len(P)//3==b+1) else height/2
            if len(P)//3 in [b, b+1]: py = cy-height/2
            else: py = cy+height/2
            P.extend([cx+px, py, cz+pz]); N.extend([nx, 0, nz])
        b2 = len(P)//3 - 4
        I.extend([b2,b2+1,b2+2,b2,b2+2,b2+3])

def generate_building(P, N, I, bx, bz, w, d, h, detail_level):
    floors = max(2, int(h / 1.2))
    fh = h / floors
    for f in range(floors):
        fy = f*fh + fh/2
        add_box(P, N, I, bx, fy, bz, w/2*0.95, fh/2*0.92, d/2*0.95)
        # Window columns
        cols = max(2, int(w * detail_level))
        for c in range(cols):
            wx = bx - w/2 + w/(cols+1) * (c+1)
            for side in [-1, 1]:
                wz = bz + (d/2 + 0.03) * side
                add_box(P, N, I, wx, fy, wz, 0.12, fh/2*0.6, 0.02)
        # Side windows
        side_cols = max(2, int(d * detail_level))
        for c in range(side_cols):
            wz = bz - d/2 + d/(side_cols+1) * (c+1)
            for side in [-1, 1]:
                wx = bx + (w/2 + 0.03) * side
                add_box(P, N, I, wx, fy, wz, 0.02, fh/2*0.6, 0.12)
        # Floor slab edge
        add_box(P, N, I, bx, f*fh, bz, w/2+0.05, 0.04, d/2+0.05)
    # Roof details
    add_box(P, N, I, bx, h+0.1, bz, w/2-0.2, 0.15, d/2-0.2)
    if detail_level > 1:
        for _ in range(3):
            rx = bx + random.uniform(-w/3, w/3)
            rz = bz + random.uniform(-d/3, d/3)
            add_box(P, N, I, rx, h+0.4, rz, 0.3, 0.25, 0.3)

random.seed(42)
P, N, I = [], [], []

grid = 16
spacing = 5.5
detail = 2.5

for gx in range(grid):
    for gz in range(grid):
        bx = (gx - grid/2) * spacing
        bz = (gz - grid/2) * spacing
        w = random.uniform(2.5, 4.5)
        d = random.uniform(2.5, 4.5)
        h = random.uniform(3.0, 20.0)
        generate_building(P, N, I, bx, bz, w, d, h, detail)
    if gx % 4 == 0:
        print(f"  row {gx}/{grid}...")

# Ground + sidewalks
half = grid * spacing / 2 + 5
add_box(P, N, I, 0, -0.05, 0, half, 0.05, half)
# Roads
for i in range(grid+1):
    rx = (i - grid/2) * spacing - spacing/2
    add_box(P, N, I, rx, -0.02, 0, 0.6, 0.02, half)
    add_box(P, N, I, 0, -0.02, rx, half, 0.02, 0.6)

tris = len(I) // 3
verts = len(P) // 3
print(f"Generated city: {verts:,} vertices, {tris:,} triangles")

bmin = [min(P[i::3]) for i in range(3)]
bmax = [max(P[i::3]) for i in range(3)]

out = "/Users/tyler/Documents/renderer/benchmarks/scenes/generated/massive_city.glb"
os.makedirs(os.path.dirname(out), exist_ok=True)

pos_bytes = struct.pack(f'{len(P)}f', *P)
idx_bytes = struct.pack(f'{len(I)}I', *I)
buf = pos_bytes + idx_bytes

gltf = {
    "asset": {"version": "2.0"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
    "materials": [{"name": "city"}],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": verts, "type": "VEC3",
         "min": bmin, "max": bmax},
        {"bufferView": 1, "componentType": 5125, "count": len(I), "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes)},
        {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(idx_bytes)},
    ],
    "buffers": [{"byteLength": len(buf)}],
}

gj = json.dumps(gltf).encode()
while len(gj) % 4: gj += b' '
while len(buf) % 4: buf += b'\x00'
total = 12 + 8 + len(gj) + 8 + len(buf)

with open(out, 'wb') as f:
    f.write(struct.pack('<III', 0x46546C67, 2, total))
    f.write(struct.pack('<II', len(gj), 0x4E4F534A) + gj)
    f.write(struct.pack('<II', len(buf), 0x004E4942) + buf)

print(f"Written {os.path.getsize(out)/1024/1024:.1f}MB to {out}")
