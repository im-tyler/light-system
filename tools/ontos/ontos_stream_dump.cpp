#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f64 = double;

static const u64 FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
static const u64 FNV_PRIME = 0x100000001b3ULL;
static const int WORLD = 128;
static const int REGION_FINE = 64;
static const int COARSE_N = 32;

static void put_u64le(u8 *dst, u64 v) {
  for (int i = 0; i < 8; ++i) dst[i] = static_cast<u8>(v >> (8 * i));
}

static void put_u32le(u8 *dst, u32 v) {
  for (int i = 0; i < 4; ++i) dst[i] = static_cast<u8>(v >> (8 * i));
}

static u64 fnv1a64(const u8 *data, std::size_t len) {
  u64 h = FNV_OFFSET_BASIS;
  for (std::size_t i = 0; i < len; ++i) {
    h = (h ^ data[i]) * FNV_PRIME;
  }
  return h;
}

static bool fnv_self_check() {
  static const u8 singleton[] = {'a'};
  return fnv1a64(singleton, 0) == 0xcbf29ce484222325ULL &&
         fnv1a64(singleton, 1) == 0xaf63dc4c8601ec8cULL;
}

enum Level { LEVEL_COARSE = 0, LEVEL_FINE = 1 };

struct Region {
  Level level = LEVEL_FINE;
  std::vector<u8> cells;
};

struct World {
  u64 seed = 0;
  u64 tick = 0;
  Region regions[4];

  World() {
    for (auto &r : regions) {
      r.level = LEVEL_FINE;
      r.cells.assign(static_cast<std::size_t>(REGION_FINE) * REGION_FINE, 0);
    }
  }

  static int region_index(int rx, int ry) { return ry * 2 + rx; }

  static std::size_t cell_index_fine(int fx, int fy) {
    return static_cast<std::size_t>((fy % REGION_FINE) * REGION_FINE + (fx % REGION_FINE));
  }

  static std::size_t cell_index_coarse(int fx, int fy) {
    return static_cast<std::size_t>(((fy % REGION_FINE) / 2) * COARSE_N + ((fx % REGION_FINE) / 2));
  }

  void set_fine(int fx, int fy, bool alive) {
    Region &r = regions[region_index(fx / REGION_FINE, fy / REGION_FINE)];
    std::size_t idx =
        r.level == LEVEL_FINE ? cell_index_fine(fx, fy) : cell_index_coarse(fx, fy);
    r.cells[idx] = alive ? 1 : 0;
  }

  void seed_r_pentomino() {
    const int c = WORLD / 2;
    const int offs[5][2] = {{0, 0}, {1, 0}, {0, 1}, {-1, 1}, {0, 2}};
    for (const auto &o : offs) {
      set_fine((c + o[0] + WORLD) % WORLD, (c + o[1] + WORLD) % WORLD, true);
    }
  }

  u8 read(int fx, int fy) const {
    fx = ((fx % WORLD) + WORLD) % WORLD;
    fy = ((fy % WORLD) + WORLD) % WORLD;
    const Region &r = regions[region_index(fx / REGION_FINE, fy / REGION_FINE)];
    if (r.level == LEVEL_FINE) {
      return r.cells[cell_index_fine(fx, fy)];
    }
    return r.cells[cell_index_coarse(fx, fy)];
  }

  u8 read_block(int cx, int cy) const {
    const int cw = WORLD / 2;
    cx = ((cx % cw) + cw) % cw;
    cy = ((cy % cw) + cw) % cw;
    const Region &r = regions[region_index(cx * 2 / REGION_FINE, cy * 2 / REGION_FINE)];
    if (r.level == LEVEL_COARSE) {
      return r.cells[static_cast<std::size_t>((cy % COARSE_N) * COARSE_N + (cx % COARSE_N))];
    }
    u8 v = 0;
    for (int dy = 0; dy < 2; ++dy) {
      for (int dx = 0; dx < 2; ++dx) {
        v |= read(cx * 2 + dx, cy * 2 + dy);
      }
    }
    return v;
  }

  int expansion_pick(int gx, int gy) const {
    u8 p[16];
    put_u64le(p, seed);
    put_u32le(p + 8, static_cast<u32>(gx));
    put_u32le(p + 12, static_cast<u32>(gy));
    return static_cast<int>(fnv1a64(p, sizeof p) % 4);
  }

  void set_level(int rx, int ry, Level level) {
    Region &r = regions[region_index(rx, ry)];
    if (r.level == level) {
      return;
    }
    if (level == LEVEL_FINE) {
      static const int offs[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
      Region fine;
      fine.level = LEVEL_FINE;
      fine.cells.assign(static_cast<std::size_t>(REGION_FINE) * REGION_FINE, 0);
      for (int cy = 0; cy < COARSE_N; ++cy) {
        for (int cx = 0; cx < COARSE_N; ++cx) {
          if (r.cells[static_cast<std::size_t>(cy * COARSE_N + cx)] == 0) {
            continue;
          }
          int d = expansion_pick(rx * REGION_FINE + cx * 2, ry * REGION_FINE + cy * 2);
          fine.cells[static_cast<std::size_t>((cy * 2 + offs[d][1]) * REGION_FINE + cx * 2 +
                                              offs[d][0])] = 1;
        }
      }
      r = std::move(fine);
    } else {
      Region coarse;
      coarse.level = LEVEL_COARSE;
      coarse.cells.assign(static_cast<std::size_t>(COARSE_N) * COARSE_N, 0);
      for (int cy = 0; cy < COARSE_N; ++cy) {
        for (int cx = 0; cx < COARSE_N; ++cx) {
          u8 alive = 0;
          for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
              alive |= r.cells[static_cast<std::size_t>((cy * 2 + dy) * REGION_FINE + cx * 2 + dx)];
            }
          }
          coarse.cells[static_cast<std::size_t>(cy * COARSE_N + cx)] = alive;
        }
      }
      r = std::move(coarse);
    }
  }

  int fine_neighbors(int gx, int gy) const {
    int count = 0;
    u64 seen[8];
    int nseen = 0;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) {
          continue;
        }
        int nx = ((gx + dx) % WORLD + WORLD) % WORLD;
        int ny = ((gy + dy) % WORLD + WORLD) % WORLD;
        if (read(nx, ny) == 0) {
          continue;
        }
        bool coarse =
            regions[region_index(nx / REGION_FINE, ny / REGION_FINE)].level == LEVEL_COARSE;
        u64 key = coarse ? (0x8000000000000000ULL | (static_cast<u64>(nx / 2) << 32) |
                            static_cast<u64>(ny / 2))
                          : ((static_cast<u64>(nx) << 32) | static_cast<u64>(ny));
        bool dup = false;
        for (int i = 0; i < nseen; ++i) {
          if (seen[i] == key) {
            dup = true;
            break;
          }
        }
        if (dup) {
          continue;
        }
        seen[nseen++] = key;
        ++count;
      }
    }
    return count;
  }

  int coarse_neighbors(int gx, int gy) const {
    int count = 0;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) {
          continue;
        }
        count += read_block(gx + dx, gy + dy);
      }
    }
    return count;
  }

  void step() {
    Region next[4];
    for (int i = 0; i < 4; ++i) {
      const int rx = i % 2;
      const int ry = i / 2;
      const Region &r = regions[i];
      if (r.level == LEVEL_FINE) {
        next[i].level = LEVEL_FINE;
        next[i].cells.assign(static_cast<std::size_t>(REGION_FINE) * REGION_FINE, 0);
        for (int fy = 0; fy < REGION_FINE; ++fy) {
          for (int fx = 0; fx < REGION_FINE; ++fx) {
            bool alive = r.cells[cell_index_fine(fx, fy)] == 1;
            int n = fine_neighbors(rx * REGION_FINE + fx, ry * REGION_FINE + fy);
            bool live = alive ? (n == 2 || n == 3) : (n == 3);
            next[i].cells[cell_index_fine(fx, fy)] = live ? 1 : 0;
          }
        }
      } else {
        next[i].level = LEVEL_COARSE;
        next[i].cells.assign(static_cast<std::size_t>(COARSE_N) * COARSE_N, 0);
        for (int cy = 0; cy < COARSE_N; ++cy) {
          for (int cx = 0; cx < COARSE_N; ++cx) {
            bool alive = r.cells[static_cast<std::size_t>(cy * COARSE_N + cx)] == 1;
            int n = coarse_neighbors(rx * COARSE_N + cx, ry * COARSE_N + cy);
            bool live = alive ? (n == 2 || n == 3) : (n == 3);
            next[i].cells[static_cast<std::size_t>(cy * COARSE_N + cx)] = live ? 1 : 0;
          }
        }
      }
    }
    for (int i = 0; i < 4; ++i) {
      regions[i] = std::move(next[i]);
    }
    ++tick;
  }

  u64 region_population(int idx) const {
    u64 p = 0;
    for (u8 c : regions[idx].cells) {
      p += c;
    }
    return p;
  }

  u64 population() const {
    u64 p = 0;
    for (int i = 0; i < 4; ++i) {
      p += region_population(i);
    }
    return p;
  }

  u64 region_hash(int idx) const {
    const Region &r = regions[idx];
    std::vector<u8> buf;
    buf.reserve(1 + r.cells.size());
    buf.push_back(r.level == LEVEL_COARSE ? 0 : 1);
    buf.insert(buf.end(), r.cells.begin(), r.cells.end());
    return fnv1a64(buf.data(), buf.size());
  }

  u64 world_hash() const {
    u8 buf[40];
    put_u64le(buf, tick);
    put_u64le(buf + 8, region_hash(region_index(0, 0)));
    put_u64le(buf + 16, region_hash(region_index(1, 0)));
    put_u64le(buf + 24, region_hash(region_index(0, 1)));
    put_u64le(buf + 32, region_hash(region_index(1, 1)));
    return fnv1a64(buf, sizeof buf);
  }
};

static bool take_u32(const std::vector<u8> &d, std::size_t &off, u32 &out) {
  if (d.size() - off < 4) {
    return false;
  }
  out = static_cast<u32>(d[off]) | (static_cast<u32>(d[off + 1]) << 8) |
        (static_cast<u32>(d[off + 2]) << 16) | (static_cast<u32>(d[off + 3]) << 24);
  off += 4;
  return true;
}

static bool take_u64(const std::vector<u8> &d, std::size_t &off, u64 &out) {
  if (d.size() - off < 8) {
    return false;
  }
  u64 lo = 0;
  u64 hi = 0;
  for (int i = 0; i < 4; ++i) {
    lo |= static_cast<u64>(d[off + i]) << (8 * i);
    hi |= static_cast<u64>(d[off + 4 + i]) << (8 * i);
  }
  out = lo | (hi << 32);
  off += 8;
  return true;
}

static bool take_f64(const std::vector<u8> &d, std::size_t &off, f64 &out) {
  u64 bits = 0;
  if (!take_u64(d, off, bits)) {
    return false;
  }
  std::memcpy(&out, &bits, 8);
  return true;
}

static const f64 G_CONST = 1.0;
static const f64 G_EPS2 = 1.0;
static const f64 G_DT = 1.0 / 1024.0;
static const int G_WINDOW = 32;
static const int G_DEG = 8;
static const int G_SAMPLES = 33;
static const u8 G_UNMANAGED = 255;
static const f64 G_TWO_POW_NEG64 = 0x1p-64;

struct SplitMix64 {
  u64 state;
  explicit SplitMix64(u64 s) : state(s) {}
  u64 draw() {
    state += 0x9E3779B97F4A7C15ULL;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
};

struct GBody {
  u32 id = 0;
  f64 mass = 0, x = 0, y = 0, vx = 0, vy = 0;
};

struct GFit {
  f64 c[4][9];
  u64 t0 = 0;
};

static int g_region_at(f64 x, f64 y) {
  if (x < 0.0 || x >= 128.0 || y < 0.0 || y >= 128.0) {
    return G_UNMANAGED;
  }
  const int rx = static_cast<int>(x / 64.0);
  const int ry = static_cast<int>(y / 64.0);
  return ry * 2 + rx;
}

static f64 g_clenshaw(const f64 c[9], f64 s) {
  f64 b1 = 0.0;
  f64 b2 = 0.0;
  for (int j = G_DEG; j >= 1; --j) {
    const f64 b0 = c[j] + 2.0 * s * b1 - b2;
    b2 = b1;
    b1 = b0;
  }
  return c[0] + s * b1 - b2;
}

static void g_cheb_table(f64 t[G_DEG + 1][G_SAMPLES], f64 w[G_SAMPLES]) {
  for (int k = 0; k < G_SAMPLES; ++k) {
    w[k] = 1.0;
  }
  w[0] = 0.5;
  w[G_SAMPLES - 1] = 0.5;
  for (int k = 0; k < G_SAMPLES; ++k) {
    const f64 s = -1.0 + static_cast<f64>(k) / 16.0;
    t[0][k] = 1.0;
    t[1][k] = s;
    for (int j = 2; j <= G_DEG; ++j) {
      t[j][k] = 2.0 * s * t[j - 1][k] - t[j - 2][k];
    }
  }
}

static void g_project(const f64 ys[G_SAMPLES], f64 out[9]) {
  static f64 t[G_DEG + 1][G_SAMPLES];
  static f64 w[G_SAMPLES];
  static bool init = false;
  if (!init) {
    g_cheb_table(t, w);
    init = true;
  }
  f64 g[G_DEG + 1][G_DEG + 1];
  for (int j = 0; j <= G_DEG; ++j) {
    for (int l = 0; l <= G_DEG; ++l) {
      f64 sum = 0.0;
      for (int k = 0; k < G_SAMPLES; ++k) {
        sum += w[k] * t[j][k] * t[l][k];
      }
      g[j][l] = sum;
    }
  }
  f64 b[G_DEG + 1];
  for (int j = 0; j <= G_DEG; ++j) {
    f64 sum = 0.0;
    for (int k = 0; k < G_SAMPLES; ++k) {
      sum += w[k] * ys[k] * t[j][k];
    }
    b[j] = sum;
  }
  const int n = G_DEG + 1;
  f64 l[G_DEG + 1][G_DEG + 1];
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j <= i; ++j) {
      f64 sum = g[i][j];
      for (int k = 0; k < j; ++k) {
        sum -= l[i][k] * l[j][k];
      }
      if (i == j) {
        l[i][j] = std::sqrt(sum);
      } else {
        l[i][j] = sum / l[j][j];
      }
    }
  }
  f64 z[G_DEG + 1];
  for (int i = 0; i < n; ++i) {
    f64 sum = b[i];
    for (int k = 0; k < i; ++k) {
      sum -= l[i][k] * z[k];
    }
    z[i] = sum / l[i][i];
  }
  for (int i = n - 1; i >= 0; --i) {
    f64 sum = z[i];
    for (int k = i + 1; k < n; ++k) {
      sum -= l[k][i] * out[k];
    }
    out[i] = sum / l[i][i];
  }
}

struct GravityWorld {
  u64 seed = 0;
  u64 tick = 0;
  f64 px = 0, py = 0;
  std::vector<GBody> bodies;
  std::vector<GFit> coarse;
  std::vector<u8> body_region;
  bool region_coarse[4] = {false, false, false, false};

  explicit GravityWorld(u64 s, u32 count) : seed(s) {
    SplitMix64 rng(s);
    bodies.resize(count);
    coarse.resize(count);
    body_region.assign(count, G_UNMANAGED);
    for (u32 i = 0; i < count; ++i) {
      const u64 u0 = rng.draw();
      const u64 u1 = rng.draw();
      const u64 u2 = rng.draw();
      const u64 u3 = rng.draw();
      const u64 u4 = rng.draw();
      GBody &b = bodies[i];
      b.id = i;
      b.mass = 0.5 + static_cast<f64>(u0) * G_TWO_POW_NEG64 * 2.0;
      b.x = 32.0 + static_cast<f64>(u1) * G_TWO_POW_NEG64 * 64.0;
      b.y = 32.0 + static_cast<f64>(u2) * G_TWO_POW_NEG64 * 64.0;
      b.vx = (static_cast<f64>(u3) * G_TWO_POW_NEG64 - 0.5) * 0.5;
      b.vy = (static_cast<f64>(u4) * G_TWO_POW_NEG64 - 0.5) * 0.5;
      px += b.mass * b.vx;
      py += b.mass * b.vy;
    }
    for (auto &f : coarse) {
      f.t0 = 0;
      for (auto &row : f.c) {
        for (f64 &v : row) {
          v = 0.0;
        }
      }
    }
  }

  bool is_coarse(std::size_t i) const { return body_region[i] != G_UNMANAGED; }

  GBody state_at(std::size_t i, u64 t) const {
    GBody b = bodies[i];
    if (is_coarse(i)) {
      const GFit &fit = coarse[i];
      const f64 s = -1.0 + static_cast<f64>(t - fit.t0) / 32.0;
      b.x = g_clenshaw(fit.c[0], s);
      b.y = g_clenshaw(fit.c[1], s);
      b.vx = g_clenshaw(fit.c[2], s);
      b.vy = g_clenshaw(fit.c[3], s);
    }
    return b;
  }

  static void accumulate(std::vector<GBody> &bs, std::vector<f64> &ax, std::vector<f64> &ay) {
    const std::size_t n = bs.size();
    ax.assign(n, 0.0);
    ay.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = i + 1; j < n; ++j) {
        const f64 dx = bs[j].x - bs[i].x;
        const f64 dy = bs[j].y - bs[i].y;
        const f64 s2 = dx * dx + dy * dy + G_EPS2;
        const f64 inv3 = 1.0 / (s2 * std::sqrt(s2));
        const f64 fx = G_CONST * inv3 * dx;
        const f64 fy = G_CONST * inv3 * dy;
        ax[i] += bs[j].mass * fx;
        ay[i] += bs[j].mass * fy;
        ax[j] -= bs[i].mass * fx;
        ay[j] -= bs[i].mass * fy;
      }
    }
  }

  static void leapfrog(std::vector<GBody> &bs) {
    std::vector<f64> ax, ay;
    const f64 half = G_DT * 0.5;
    accumulate(bs, ax, ay);
    for (std::size_t i = 0; i < bs.size(); ++i) {
      bs[i].vx += ax[i] * half;
      bs[i].vy += ay[i] * half;
    }
    for (auto &b : bs) {
      b.x += b.vx * G_DT;
      b.y += b.vy * G_DT;
    }
    accumulate(bs, ax, ay);
    for (std::size_t i = 0; i < bs.size(); ++i) {
      bs[i].vx += ax[i] * half;
      bs[i].vy += ay[i] * half;
    }
  }

  void fit_members(const std::vector<std::size_t> &members, u64 t0) {
    std::vector<GBody> subset;
    for (std::size_t i : members) {
      subset.push_back(state_at(i, t0));
    }
    std::vector<std::vector<GBody>> samples(members.size());
    for (std::size_t s = 0; s < members.size(); ++s) {
      samples[s].assign(G_SAMPLES, subset[s]);
    }
    std::vector<GBody> cur = subset;
    for (int k = 1; k < G_SAMPLES; ++k) {
      leapfrog(cur);
      for (std::size_t s = 0; s < cur.size(); ++s) {
        samples[s][k] = cur[s];
      }
    }
    for (std::size_t s = 0; s < members.size(); ++s) {
      f64 ys[G_SAMPLES];
      GFit fit;
      fit.t0 = t0;
      for (int coord = 0; coord < 4; ++coord) {
        for (int k = 0; k < G_SAMPLES; ++k) {
          const GBody &b = samples[s][k];
          ys[k] = coord == 0 ? b.x : coord == 1 ? b.y : coord == 2 ? b.vx : b.vy;
        }
        g_project(ys, fit.c[coord]);
      }
      coarse[members[s]] = fit;
    }
  }

  void demote(int region, u64 t0) {
    const f64 x0 = static_cast<f64>(region % 2) * 64.0;
    const f64 y0 = static_cast<f64>(region / 2) * 64.0;
    std::vector<std::size_t> members;
    for (std::size_t i = 0; i < bodies.size(); ++i) {
      const GBody b = state_at(i, t0);
      if (b.x >= x0 && b.x < x0 + 64.0 && b.y >= y0 && b.y < y0 + 64.0) {
        members.push_back(i);
      }
    }
    region_coarse[region] = true;
    if (members.empty()) {
      return;
    }
    fit_members(members, t0);
    for (std::size_t i : members) {
      body_region[i] = static_cast<u8>(region);
    }
  }

  void promote(int region, u64 t) {
    for (std::size_t i = 0; i < bodies.size(); ++i) {
      if (is_coarse(i) && body_region[i] == region) {
        bodies[i] = state_at(i, t);
        body_region[i] = G_UNMANAGED;
      }
    }
    region_coarse[region] = false;
  }

  void refit(int region, u64 t) {
    const f64 x0 = static_cast<f64>(region % 2) * 64.0;
    const f64 y0 = static_cast<f64>(region / 2) * 64.0;
    std::vector<std::size_t> members;
    for (std::size_t i = 0; i < bodies.size(); ++i) {
      if (is_coarse(i) && body_region[i] == region) {
        members.push_back(i);
      }
    }
    for (std::size_t i : members) {
      bodies[i] = state_at(i, t);
    }
    std::vector<std::size_t> keep;
    for (std::size_t i : members) {
      if (bodies[i].x >= x0 && bodies[i].x < x0 + 64.0 && bodies[i].y >= y0 &&
          bodies[i].y < y0 + 64.0) {
        keep.push_back(i);
      } else {
        body_region[i] = G_UNMANAGED;
      }
    }
    if (keep.empty()) {
      region_coarse[region] = false;
      return;
    }
    fit_members(keep, t);
    for (std::size_t i : keep) {
      body_region[i] = static_cast<u8>(region);
    }
  }

  void accel_split(const std::vector<GBody> &view, const std::vector<char> &coarse_flag,
                   std::vector<f64> &ax_ff, std::vector<f64> &ay_ff, std::vector<f64> &ax_fc,
                   std::vector<f64> &ay_fc) const {
    const std::size_t n = view.size();
    ax_ff.assign(n, 0.0);
    ay_ff.assign(n, 0.0);
    ax_fc.assign(n, 0.0);
    ay_fc.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = i + 1; j < n; ++j) {
        const f64 dx = view[j].x - view[i].x;
        const f64 dy = view[j].y - view[i].y;
        const f64 s2 = dx * dx + dy * dy + G_EPS2;
        const f64 inv3 = 1.0 / (s2 * std::sqrt(s2));
        const f64 fx = G_CONST * inv3 * dx;
        const f64 fy = G_CONST * inv3 * dy;
        if (!coarse_flag[i] && !coarse_flag[j]) {
          ax_ff[i] += view[j].mass * fx;
          ay_ff[i] += view[j].mass * fy;
          ax_ff[j] -= view[i].mass * fx;
          ay_ff[j] -= view[i].mass * fy;
        } else if (!coarse_flag[i] && coarse_flag[j]) {
          ax_fc[i] += view[j].mass * fx;
          ay_fc[i] += view[j].mass * fy;
        } else if (coarse_flag[i] && !coarse_flag[j]) {
          ax_fc[j] -= view[i].mass * fx;
          ay_fc[j] -= view[i].mass * fy;
        }
      }
    }
  }

  void step() {
    const u64 entering = tick + 1;
    for (int region = 0; region < 4; ++region) {
      if (!region_coarse[region]) {
        continue;
      }
      bool ended = false;
      for (std::size_t i = 0; i < bodies.size(); ++i) {
        if (body_region[i] == region && is_coarse(i) && entering == coarse[i].t0 + G_WINDOW) {
          ended = true;
          break;
        }
      }
      if (ended) {
        refit(region, entering);
      }
    }
    const std::size_t n = bodies.size();
    std::vector<char> cflag(n);
    for (std::size_t i = 0; i < n; ++i) {
      cflag[i] = is_coarse(i) ? 1 : 0;
    }
    const f64 half = G_DT * 0.5;
    std::vector<GBody> view(n);
    for (std::size_t i = 0; i < n; ++i) {
      view[i] = state_at(i, entering);
    }
    std::vector<f64> ax_ff, ay_ff, ax_fc, ay_fc;
    accel_split(view, cflag, ax_ff, ay_ff, ax_fc, ay_fc);
    for (std::size_t i = 0; i < n; ++i) {
      if (!cflag[i]) {
        bodies[i].vx += ax_ff[i] * half;
        bodies[i].vy += ay_ff[i] * half;
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (!cflag[i]) {
        bodies[i].vx += ax_fc[i] * half;
        bodies[i].vy += ay_fc[i] * half;
        px += bodies[i].mass * (ax_fc[i] * half);
        py += bodies[i].mass * (ay_fc[i] * half);
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (!cflag[i]) {
        bodies[i].x += bodies[i].vx * G_DT;
        bodies[i].y += bodies[i].vy * G_DT;
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      view[i] = state_at(i, entering);
    }
    accel_split(view, cflag, ax_ff, ay_ff, ax_fc, ay_fc);
    for (std::size_t i = 0; i < n; ++i) {
      if (!cflag[i]) {
        bodies[i].vx += ax_ff[i] * half;
        bodies[i].vy += ay_ff[i] * half;
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (!cflag[i]) {
        bodies[i].vx += ax_fc[i] * half;
        bodies[i].vy += ay_fc[i] * half;
        px += bodies[i].mass * (ax_fc[i] * half);
        py += bodies[i].mass * (ay_fc[i] * half);
      }
    }
    tick = entering;
  }

  void totals(u64 &fine, u64 &coarse_n, f64 &mass, f64 &tpx, f64 &tpy, f64 &energy) const {
    const std::size_t n = bodies.size();
    std::vector<GBody> view(n);
    for (std::size_t i = 0; i < n; ++i) {
      view[i] = state_at(i, tick);
    }
    fine = 0;
    coarse_n = 0;
    mass = 0.0;
    f64 ke = 0.0;
    for (const GBody &b : view) {
      if (is_coarse(b.id)) {
        ++coarse_n;
      } else {
        ++fine;
      }
      mass += b.mass;
      ke += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy);
    }
    f64 pe = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = i + 1; j < n; ++j) {
        const f64 dx = view[j].x - view[i].x;
        const f64 dy = view[j].y - view[i].y;
        const f64 s2 = dx * dx + dy * dy + G_EPS2;
        pe -= view[i].mass * view[j].mass / std::sqrt(s2);
      }
    }
    tpx = px;
    tpy = py;
    energy = ke + pe;
  }

  void state_bytes(std::size_t i, u8 out[45]) const {
    const GBody b = state_at(i, tick);
    put_u32le(out, b.id);
    u64 bits;
    std::memcpy(&bits, &b.x, 8);
    put_u64le(out + 4, bits);
    std::memcpy(&bits, &b.y, 8);
    put_u64le(out + 12, bits);
    std::memcpy(&bits, &b.vx, 8);
    put_u64le(out + 20, bits);
    std::memcpy(&bits, &b.vy, 8);
    put_u64le(out + 28, bits);
    std::memcpy(&bits, &b.mass, 8);
    put_u64le(out + 36, bits);
    out[44] = is_coarse(i) ? 0 : 1;
  }

  void emitted(std::size_t i, u8 &region, u8 &level, GBody &b) const {
    b = state_at(i, tick);
    level = is_coarse(i) ? 0 : 1;
    region = is_coarse(i) ? body_region[i] : static_cast<u8>(g_region_at(b.x, b.y));
  }

  u64 region_hash(int region, u8 &level, u64 &pop) const {
    std::vector<std::size_t> members;
    for (std::size_t i = 0; i < bodies.size(); ++i) {
      if (is_coarse(i)) {
        if (body_region[i] == region) {
          members.push_back(i);
        }
      } else {
        const GBody b = state_at(i, tick);
        if (g_region_at(b.x, b.y) == region) {
          members.push_back(i);
        }
      }
    }
    level = region_coarse[region] ? 0 : 1;
    pop = members.size();
    std::vector<u8> buf;
    buf.push_back(level);
    u8 sb[45];
    for (std::size_t i : members) {
      state_bytes(i, sb);
      buf.insert(buf.end(), sb, sb + 45);
    }
    return fnv1a64(buf.data(), buf.size());
  }

  u64 world_hash() const {
    std::vector<u8> buf;
    u8 tmp[8];
    put_u64le(tmp, tick);
    buf.insert(buf.end(), tmp, tmp + 8);
    u8 sb[45];
    for (std::size_t i = 0; i < bodies.size(); ++i) {
      state_bytes(i, sb);
      buf.insert(buf.end(), sb, sb + 45);
    }
    return fnv1a64(buf.data(), buf.size());
  }
};

static u32 le32_at(const std::vector<u8> &d, std::size_t off) {
  return static_cast<u32>(d[off]) | (static_cast<u32>(d[off + 1]) << 8) |
         (static_cast<u32>(d[off + 2]) << 16) | (static_cast<u32>(d[off + 3]) << 24);
}

struct RegionRes {
  bool present = false;
  int level = 0;
  u64 pop = 0;
  bool pop_ok = false;
  bool hash_ok = false;
};

static bool f64_bits_eq(f64 a, f64 b) {
  u64 ba, bb;
  std::memcpy(&ba, &a, 8);
  std::memcpy(&bb, &b, 8);
  return ba == bb;
}

static int run_gravity(const std::vector<u8> &data, u64 seed, u32 body_count) {
  GravityWorld world(seed, body_count);
  GravityWorld reference(seed, body_count);
  struct Pending {
    int region;
    bool to_coarse;
  };
  std::vector<Pending> pending;
  u64 ticks_seen = 0;
  u64 totals_seen = 0;
  u64 states_seen = 0;
  u64 bodies_seen = 0;
  f64 max_pos_dev = 0.0;
  u64 last_tick = 0;
  std::size_t off = 20;

  while (off < data.size()) {
    const std::size_t rec_start = off;
    const u8 tag = data[off++];
    switch (tag) {
      case 1: {
        u64 t = 0;
        if (!take_u64(data, off, t)) {
          std::fprintf(stderr, "error: truncated TickHeader at offset %zu\n", rec_start);
          return 2;
        }
        for (const Pending &p : pending) {
          if (p.to_coarse) {
            world.demote(p.region, world.tick + 1);
          } else {
            world.promote(p.region, world.tick + 1);
          }
        }
        pending.clear();
        world.step();
        reference.step();
        if (t != world.tick) {
          std::fprintf(stderr,
                       "MISMATCH: TickHeader record tick=%" PRIu64 " re-simulated tick=%" PRIu64
                       "\n",
                       t, world.tick);
          return 1;
        }
        last_tick = t;
        ++ticks_seen;
        for (std::size_t i = 0; i < world.bodies.size(); ++i) {
          const GBody b = world.state_at(i, world.tick);
          const GBody &r = reference.bodies[i];
          const f64 dev = b.x - r.x > r.x - b.x ? b.x - r.x : r.x - b.x;
          const f64 devy = b.y - r.y > r.y - b.y ? b.y - r.y : r.y - b.y;
          if (dev > max_pos_dev) {
            max_pos_dev = dev;
          }
          if (devy > max_pos_dev) {
            max_pos_dev = devy;
          }
        }
      } break;
      case 2: {
        u64 p = 0;
        if (!take_u64(data, off, p)) {
          std::fprintf(stderr, "error: truncated Snapshot at offset %zu\n", rec_start);
          return 2;
        }
        if (p != world.bodies.size()) {
          std::fprintf(stderr,
                       "MISMATCH: tick %" PRIu64 " Snapshot population stream=%" PRIu64
                       " computed=%zu\n",
                       last_tick, p, world.bodies.size());
          return 1;
        }
      } break;
      case 3: {
        u64 t = 0;
        u32 x = 0;
        u32 y = 0;
        if (!take_u64(data, off, t) || !take_u32(data, off, x) || !take_u32(data, off, y)) {
          std::fprintf(stderr, "error: truncated CellFlipped at offset %zu\n", rec_start);
          return 2;
        }
      } break;
      case 4: {
        u32 rx = 0;
        u32 ry = 0;
        if (!take_u32(data, off, rx) || !take_u32(data, off, ry) || data.size() - off < 1) {
          std::fprintf(stderr, "error: truncated RegionLevel at offset %zu\n", rec_start);
          return 2;
        }
        const u8 lv = data[off++];
        if (rx > 1 || ry > 1 || lv > 1) {
          std::fprintf(stderr,
                       "error: bad RegionLevel (rx=%" PRIu32 " ry=%" PRIu32 " level=%" PRIu8
                       ") at offset %zu\n",
                       rx, ry, lv, rec_start);
          return 2;
        }
        pending.push_back({static_cast<int>(ry) * 2 + static_cast<int>(rx), lv == 0});
      } break;
      case 5: {
        u64 t = 0;
        u32 rx = 0;
        u32 ry = 0;
        u8 lv = 0;
        u64 p = 0;
        u64 h = 0;
        if (!take_u64(data, off, t) || !take_u32(data, off, rx) || !take_u32(data, off, ry) ||
            data.size() - off < 1) {
          std::fprintf(stderr, "error: truncated RegionState at offset %zu\n", rec_start);
          return 2;
        }
        lv = data[off++];
        if (!take_u64(data, off, p) || !take_u64(data, off, h)) {
          std::fprintf(stderr, "error: truncated RegionState at offset %zu\n", rec_start);
          return 2;
        }
        if (rx > 1 || ry > 1) {
          std::fprintf(stderr, "error: bad RegionState region at offset %zu\n", rec_start);
          return 2;
        }
        ++states_seen;
        u8 w_level = 0;
        u64 w_pop = 0;
        const u64 w_hash = world.region_hash(static_cast<int>(ry) * 2 + static_cast<int>(rx),
                                             w_level, w_pop);
        if (lv != w_level || p != w_pop || h != w_hash) {
          std::fprintf(stderr,
                       "MISMATCH: tick %" PRIu64 " region=(%" PRIu32 ",%" PRIu32
                       ") stream=(level=%" PRIu8 " pop=%" PRIu64 " hash=%016" PRIx64
                       ") computed=(level=%d pop=%" PRIu64 " hash=%016" PRIx64 ")\n",
                       t, rx, ry, lv, p, h, w_level, w_pop, w_hash);
          return 1;
        }
      } break;
      case 6: {
        u64 t = 0;
        u32 bid = 0;
        u8 reg = 0;
        u8 lv = 0;
        f64 x = 0, y = 0, vx = 0, vy = 0, mass = 0;
        if (!take_u64(data, off, t) || !take_u32(data, off, bid) || data.size() - off < 2) {
          std::fprintf(stderr, "error: truncated BodyState at offset %zu\n", rec_start);
          return 2;
        }
        reg = data[off++];
        lv = data[off++];
        if (!take_f64(data, off, x) || !take_f64(data, off, y) || !take_f64(data, off, vx) ||
            !take_f64(data, off, vy) || !take_f64(data, off, mass)) {
          std::fprintf(stderr, "error: truncated BodyState at offset %zu\n", rec_start);
          return 2;
        }
        if (lv > 1 || bid >= world.bodies.size()) {
          std::fprintf(stderr, "error: bad BodyState (id=%" PRIu32 " level=%" PRIu8
                               ") at offset %zu\n",
                       bid, lv, rec_start);
          return 2;
        }
        ++bodies_seen;
        u8 w_reg = 0;
        u8 w_lv = 0;
        GBody b;
        world.emitted(bid, w_reg, w_lv, b);
        if (reg != w_reg || lv != w_lv || !f64_bits_eq(x, b.x) || !f64_bits_eq(y, b.y) ||
            !f64_bits_eq(vx, b.vx) || !f64_bits_eq(vy, b.vy) || !f64_bits_eq(mass, b.mass)) {
          std::fprintf(stderr,
                       "MISMATCH: tick %" PRIu64 " body=%" PRIu32
                       " stream=(reg=%d lvl=%d x=%.17g) computed=(reg=%d lvl=%d x=%.17g)\n",
                       t, bid, reg, lv, x, w_reg, w_lv, b.x);
          return 1;
        }
      } break;
      case 7: {
        u64 t = 0;
        u64 fine = 0;
        u64 cn = 0;
        f64 mass = 0, tpx = 0, tpy = 0, energy = 0;
        if (!take_u64(data, off, t) || !take_u64(data, off, fine) || !take_u64(data, off, cn) ||
            !take_f64(data, off, mass) || !take_f64(data, off, tpx) ||
            !take_f64(data, off, tpy) || !take_f64(data, off, energy)) {
          std::fprintf(stderr, "error: truncated TotalsState at offset %zu\n", rec_start);
          return 2;
        }
        ++totals_seen;
        u64 w_fine = 0;
        u64 w_cn = 0;
        f64 w_mass = 0, w_px = 0, w_py = 0, w_energy = 0;
        world.totals(w_fine, w_cn, w_mass, w_px, w_py, w_energy);
        if (fine != w_fine || cn != w_cn || !f64_bits_eq(mass, w_mass) ||
            !f64_bits_eq(tpx, w_px) || !f64_bits_eq(tpy, w_py) ||
            !f64_bits_eq(energy, w_energy)) {
          std::fprintf(stderr,
                       "MISMATCH: tick %" PRIu64 " totals stream=(fine=%" PRIu64
                       " coarse=%" PRIu64 " px=%.17g E=%.17g) computed=(fine=%" PRIu64
                       " coarse=%" PRIu64 " px=%.17g E=%.17g)\n",
                       t, fine, cn, tpx, energy, w_fine, w_cn, w_px, w_energy);
          return 1;
        }
      } break;
      default:
        std::fprintf(stderr, "error: unknown record tag %u at offset %zu\n", tag, rec_start);
        return 2;
    }
  }

  u64 r_fine = 0, r_cn = 0;
  f64 r_mass = 0, r_px = 0, r_py = 0, r_e0 = 0;
  reference.totals(r_fine, r_cn, r_mass, r_px, r_py, r_e0);
  u64 w_fine = 0, w_cn = 0;
  f64 w_mass = 0, w_px = 0, w_py = 0, w_e = 0;
  world.totals(w_fine, w_cn, w_mass, w_px, w_py, w_e);
  const f64 ref_scale =
      (r_px > 0 ? r_px : -r_px) > (r_py > 0 ? r_py : -r_py) ? (r_px > 0 ? r_px : -r_px)
                                                            : (r_py > 0 ? r_py : -r_py);
  const f64 mom_drift = ((w_px - r_px > r_px - w_px ? w_px - r_px : r_px - w_px) >
                                 (w_py - r_py > r_py - w_py ? w_py - r_py : r_py - w_py)
                             ? (w_px - r_px > r_px - w_px ? w_px - r_px : r_px - w_px)
                             : (w_py - r_py > r_py - w_py ? w_py - r_py : r_py - w_py)) /
                        (ref_scale > 1e-30 ? ref_scale : 1e-30);
  const f64 e_drift =
      ((w_e - r_e0 > r_e0 - w_e ? w_e - r_e0 : r_e0 - w_e)) /
      ((r_e0 > 0 ? r_e0 : -r_e0) > 1e-30 ? (r_e0 > 0 ? r_e0 : -r_e0) : 1e-30);
  std::printf(
      "OK: ticks=%" PRIu64 " totals=%" PRIu64 " region_states=%" PRIu64 " bodies=%" PRIu64
      " fine=%" PRIu64 " coarse=%" PRIu64 " final_world_hash=%016" PRIx64
      " max_pos_dev=%.3e mom_drift=%.3e energy_drift=%.3e\n",
      ticks_seen, totals_seen, states_seen, bodies_seen, w_fine, w_cn, world.world_hash(),
      max_pos_dev, mom_drift, e_drift);
  return 0;
}

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  if (!fnv_self_check()) {
    std::fprintf(stderr, "error: FNV-1a64 self-check failed\n");
    return 3;
  }
  if (argc != 3) {
    std::fprintf(stderr, "usage: ontos_stream_dump <stream-file> <seed>\n");
    return 3;
  }

  u64 seed = 0;
  {
    const char *s = argv[2];
    if (s[0] == '\0' || s[0] == '-') {
      std::fprintf(stderr, "error: bad seed '%s'\n", s);
      return 3;
    }
    char *end = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
      std::fprintf(stderr, "error: bad seed '%s'\n", s);
      return 3;
    }
    seed = static_cast<u64>(v);
  }

  std::FILE *f = std::fopen(argv[1], "rb");
  if (!f) {
    std::fprintf(stderr, "error: cannot open '%s'\n", argv[1]);
    return 3;
  }
  std::vector<u8> data;
  {
    u8 buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
      data.insert(data.end(), buf, buf + n);
    }
    std::fclose(f);
  }

  if (data.size() < 16 || std::memcmp(data.data(), "ONTO", 4) != 0) {
    std::fprintf(stderr, "error: not an ontos stream (bad magic or truncated header)\n");
    return 2;
  }
  const u32 version = le32_at(data, 4);
  const u32 world_w = le32_at(data, 8);
  const u32 world_h = le32_at(data, 12);
  if (version != 1 && version != 2) {
    std::fprintf(stderr, "error: unsupported stream version %" PRIu32 "\n", version);
    return 2;
  }
  if (world_w != WORLD || world_h != WORLD) {
    std::fprintf(stderr, "error: unsupported world size %" PRIu32 "x%" PRIu32 "\n", world_w,
                 world_h);
    return 2;
  }
  if (version == 2) {
    if (data.size() < 20) {
      std::fprintf(stderr, "error: truncated v2 header\n");
      return 2;
    }
    const u32 body_count = le32_at(data, 16);
    if (body_count == 0 || body_count > 100000) {
      std::fprintf(stderr, "error: implausible body count %" PRIu32 "\n", body_count);
      return 2;
    }
    return run_gravity(data, seed, body_count);
  }

  World world;
  world.seed = seed;
  world.seed_r_pentomino();

  RegionRes rr[4];
  bool in_tick = false;
  u64 cur_tick = 0;
  bool has_snap = false;
  u64 snap_pop = 0;
  u64 ticks_seen = 0;
  u64 snapshots_seen = 0;
  u64 states_seen = 0;
  u64 flips_seen = 0;

  auto flush_summary = [&]() {
    if (!in_tick) {
      return;
    }
    std::printf("tick %" PRIu64 "  pop %" PRIu64, cur_tick,
                has_snap ? snap_pop : world.population());
    static const int coords[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int i = 0; i < 4; ++i) {
      const RegionRes &x = rr[i];
      if (x.present) {
        std::printf("  (%d,%d) %s pop=%" PRIu64 " hash=%s", coords[i][0], coords[i][1],
                    x.level == LEVEL_FINE ? "fine" : "coarse", x.pop,
                    x.hash_ok ? "ok" : "FAIL");
      } else {
        std::printf("  (%d,%d) -", coords[i][0], coords[i][1]);
      }
    }
    std::printf("\n");
    in_tick = false;
    has_snap = false;
    snap_pop = 0;
    for (auto &x : rr) {
      x = RegionRes{};
    }
  };

  std::size_t off = 16;
  while (off < data.size()) {
    const std::size_t rec_start = off;
    const u8 tag = data[off++];
    switch (tag) {
      case 1: {
        u64 t = 0;
        if (!take_u64(data, off, t)) {
          std::fprintf(stderr, "error: truncated TickHeader at offset %zu\n", rec_start);
          return 2;
        }
        flush_summary();
        world.step();
        if (t != world.tick) {
          std::fprintf(stderr,
                       "MISMATCH: TickHeader record tick=%" PRIu64 " re-simulated tick=%" PRIu64
                       "\n",
                       t, world.tick);
          return 1;
        }
        in_tick = true;
        cur_tick = t;
        ++ticks_seen;
      } break;
      case 2: {
        u64 p = 0;
        if (!take_u64(data, off, p)) {
          std::fprintf(stderr, "error: truncated Snapshot at offset %zu\n", rec_start);
          return 2;
        }
        ++snapshots_seen;
        u64 actual = world.population();
        if (p != actual) {
          std::fprintf(stderr,
                       "MISMATCH: tick %" PRIu64 " Snapshot population stream=%" PRIu64
                       " computed=%" PRIu64 "\n",
                       world.tick, p, actual);
          return 1;
        }
        has_snap = true;
        snap_pop = p;
      } break;
      case 3: {
        u64 t = 0;
        u32 x = 0;
        u32 y = 0;
        if (!take_u64(data, off, t) || !take_u32(data, off, x) || !take_u32(data, off, y)) {
          std::fprintf(stderr, "error: truncated CellFlipped at offset %zu\n", rec_start);
          return 2;
        }
        ++flips_seen;
      } break;
      case 4: {
        u32 rx = 0;
        u32 ry = 0;
        u8 lv = 0;
        if (!take_u32(data, off, rx) || !take_u32(data, off, ry)) {
          std::fprintf(stderr, "error: truncated RegionLevel at offset %zu\n", rec_start);
          return 2;
        }
        if (data.size() - off < 1) {
          std::fprintf(stderr, "error: truncated RegionLevel at offset %zu\n", rec_start);
          return 2;
        }
        lv = data[off++];
        if (rx > 1 || ry > 1 || lv > 1) {
          std::fprintf(stderr,
                       "error: bad RegionLevel (rx=%" PRIu32 " ry=%" PRIu32 " level=%" PRIu8
                       ") at offset %zu\n",
                       rx, ry, lv, rec_start);
          return 2;
        }
        world.set_level(static_cast<int>(rx), static_cast<int>(ry),
                        lv == 1 ? LEVEL_FINE : LEVEL_COARSE);
      } break;
      case 5: {
        u64 t = 0;
        u32 rx = 0;
        u32 ry = 0;
        u8 lv = 0;
        u64 p = 0;
        u64 h = 0;
        if (!take_u64(data, off, t) || !take_u32(data, off, rx) || !take_u32(data, off, ry) ||
            data.size() - off < 1) {
          std::fprintf(stderr, "error: truncated RegionState at offset %zu\n", rec_start);
          return 2;
        }
        lv = data[off++];
        if (!take_u64(data, off, p) || !take_u64(data, off, h)) {
          std::fprintf(stderr, "error: truncated RegionState at offset %zu\n", rec_start);
          return 2;
        }
        if (rx > 1 || ry > 1) {
          std::fprintf(stderr,
                       "error: bad RegionState region (%" PRIu32 ",%" PRIu32 ") at offset %zu\n",
                       rx, ry, rec_start);
          return 2;
        }
        ++states_seen;
        const int idx = World::region_index(static_cast<int>(rx), static_cast<int>(ry));
        const int actual_level = world.regions[idx].level;
        const u64 actual_pop = world.region_population(idx);
        const u64 actual_hash = world.region_hash(idx);
        const bool level_ok = lv == static_cast<u8>(actual_level);
        const bool pop_ok = p == actual_pop;
        const bool hash_ok = h == actual_hash;
        RegionRes &x = rr[idx];
        x.present = true;
        x.level = lv;
        x.pop = p;
        x.pop_ok = pop_ok;
        x.hash_ok = hash_ok;
        if (!level_ok || !pop_ok || !hash_ok) {
          flush_summary();
          if (!level_ok) {
            std::fprintf(stderr,
                         "MISMATCH: tick %" PRIu64 " region=(%" PRIu32 ",%" PRIu32
                         ") field=level stream=%" PRIu8 " computed=%d\n",
                         t, rx, ry, lv, actual_level);
          } else if (!pop_ok) {
            std::fprintf(stderr,
                         "MISMATCH: tick %" PRIu64 " region=(%" PRIu32 ",%" PRIu32
                         ") field=population stream=%" PRIu64 " computed=%" PRIu64 "\n",
                         t, rx, ry, p, actual_pop);
          } else {
            std::fprintf(stderr,
                         "MISMATCH: tick %" PRIu64 " region=(%" PRIu32 ",%" PRIu32
                         ") field=hash stream=%016" PRIx64 " computed=%016" PRIx64 "\n",
                         t, rx, ry, h, actual_hash);
          }
          return 1;
        }
      } break;
      default:
        std::fprintf(stderr, "error: unknown record tag %u at offset %zu\n", tag, rec_start);
        return 2;
    }
  }

  flush_summary();
  std::printf(
      "OK: ticks=%" PRIu64 " snapshots=%" PRIu64 " region_states=%" PRIu64 " flips=%" PRIu64
      " final_population=%" PRIu64 " final_world_hash=%016" PRIx64 "\n",
      ticks_seen, snapshots_seen, states_seen, flips_seen, world.population(), world.world_hash());
  return 0;
}
