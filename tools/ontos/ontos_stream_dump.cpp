#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <utility>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
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

static void put_u16le(u8 *dst, u16 v) {
  dst[0] = static_cast<u8>(v & 0xff);
  dst[1] = static_cast<u8>(v >> 8);
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
static const f64 C_CONTACT_R = 2.0;
static const u32 C_MONOPOLE_BASE = 0xFF000000u;
static const u32 C_WALL_BASE = 0xFFFFFF00u;

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

// Section 19 collapsed-region state: totals frozen at collapse, membership,
// and the splitmix64 state left after the jitter draws (the expansion spread
// continues drawing from it). Jitter offsets live on the world (indexed by
// body id). Section 20 multipole totals (mx, my and the second central
// moments) freeze alongside; mp marks the cycle as section 20. Section 23
// adds the binding statistic; radial marks the cycle. Section 25 adds the
// per-shell intra-shell bindings (binding carries the section 23 total);
// shells marks the cycle.
struct GCollapsed {
  bool active = false;
  bool mp = false;
  bool radial = false;
  bool shells = false;
  u64 n = 0;
  f64 mass = 0, com_x = 0, com_y = 0, pxt = 0, pyt = 0, vcx = 0, vcy = 0, energy = 0;
  f64 mx = 0, my = 0, qxx = 0, qxy = 0, qyy = 0;
  f64 binding = 0;
  f64 shell_b[4] = {0.0, 0.0, 0.0, 0.0};
  u64 jitter_state = 0;
  std::vector<std::size_t> members;
};

// Section 21 contact event; vn is the pre-impulse approach speed, vn_after
// the post-impulse one (verification-only state; the record payload is
// tick, a, b, jn, cx, cy). mu feeds the section 22 audio rule (static
// contactants use their own reduced-mass rule).
struct GContact {
  u64 tick = 0;
  u32 a = 0, b = 0;
  f64 jn = 0, cx = 0, cy = 0, vn = 0, vn_after = 0, mu = 0;
};

static int g_region_at(f64 x, f64 y) {
  if (x < 0.0 || x >= 128.0 || y < 0.0 || y >= 128.0) {
    return G_UNMANAGED;
  }
  const int rx = static_cast<int>(x / 64.0);
  const int ry = static_cast<int>(y / 64.0);
  return ry * 2 + rx;
}

// Spec section 25: S = min(4, max(1, n div 3)) for n >= 1; every shell
// holds at least 3 members (single-pair shells are degenerate).
static int g_shell_count(std::size_t n) {
  if (n == 0) {
    return 0;
  }
  int v = static_cast<int>(n / 3);
  if (v < 1) {
    v = 1;
  }
  if (v > 4) {
    v = 4;
  }
  return v;
}

// Spec section 25 rank shells: sort members by (radius, id), split into S
// equal-count groups (first n mod S groups one larger). Returns the shell
// index per member, members in id order.
static std::vector<int> g_shell_assignment(const std::vector<f64> &radii) {
  const std::size_t n = radii.size();
  std::vector<int> shells(n, 0);
  if (n == 0) {
    return shells;
  }
  std::vector<std::size_t> order(n);
  for (std::size_t i = 0; i < n; ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&radii](std::size_t a, std::size_t b) {
    if (radii[a] != radii[b]) {
      return radii[a] < radii[b];
    }
    return a < b;
  });
  const int s = g_shell_count(n);
  const std::size_t ss = static_cast<std::size_t>(s);
  const std::size_t q = n / ss;
  const std::size_t rem = n % ss;
  std::size_t pos = 0;
  for (std::size_t k = 0; k < ss; ++k) {
    const std::size_t size = q + (k < rem ? 1u : 0u);
    for (std::size_t j = 0; j < size; ++j) {
      shells[order[pos]] = static_cast<int>(k);
      ++pos;
    }
  }
  return shells;
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
  bool mp_enabled = true;
  bool radial_enabled = false;
  bool shells_enabled = false;
  // Section 24 extended contact parameters (from the ContactParams record).
  bool contacts_params = false;
  bool walls_on = false;
  f64 restitution = 0.0;
  f64 friction = 0.0;
  std::vector<GBody> bodies;
  std::vector<GFit> coarse;
  std::vector<u8> body_region;
  // Per-body level byte: 0 ephemeris-coarse, 1 fine, 2 collapsed.
  std::vector<u8> blevel;
  // Per-region level byte: 0 coarse (ephemeris), 1 fine, 2 collapsed.
  u8 rstate[4] = {1, 1, 1, 1};
  GCollapsed collapsed[4];
  // Frozen section 19 jitter offsets per body (valid while blevel == 2).
  std::vector<f64> body_jx, body_jy;
  // Section 21 contact mode: sticky from the first Contact record; the
  // touching set is replay state (pairs overlapping at the previous pass).
  bool contacts_on = false;
  std::vector<std::pair<u32, u32>> touching;
  std::vector<GContact> last_contacts;

  explicit GravityWorld(u64 s, u32 count, const char *ic_profile = nullptr) : seed(s) {
    SplitMix64 rng(s);
    bodies.resize(count);
    coarse.resize(count);
    body_region.assign(count, G_UNMANAGED);
    blevel.assign(count, 1);
    body_jx.assign(count, 0.0);
    body_jy.assign(count, 0.0);
    for (u32 i = 0; i < count; ++i) {
      const u64 u0 = rng.draw();
      const u64 u1 = rng.draw();
      const u64 u2 = rng.draw();
      const u64 u3 = rng.draw();
      const u64 u4 = rng.draw();
      GBody &b = bodies[i];
      b.id = i;
      b.mass = 0.5 + static_cast<f64>(u0) * G_TWO_POW_NEG64 * 2.0;
      if (ic_profile == nullptr) {
        b.x = 32.0 + static_cast<f64>(u1) * G_TWO_POW_NEG64 * 64.0;
        b.y = 32.0 + static_cast<f64>(u2) * G_TWO_POW_NEG64 * 64.0;
        b.vx = (static_cast<f64>(u3) * G_TWO_POW_NEG64 - 0.5) * 0.5;
        b.vy = (static_cast<f64>(u4) * G_TWO_POW_NEG64 - 0.5) * 0.5;
      } else if (std::strcmp(ic_profile, "wallshot") == 0) {
        // Test-only corpus ICs (ontos docs/DESIGN.md corpus coverage):
        // body i targets wall i % 4, near it and inbound at 2..5.
        const f64 along = 16.0 + static_cast<f64>(u1) * G_TWO_POW_NEG64 * 96.0;
        const f64 off = static_cast<f64>(u2) * G_TWO_POW_NEG64 * 2.0;
        const f64 speed = 2.0 + static_cast<f64>(u3) * G_TWO_POW_NEG64 * 3.0;
        const f64 drift = (static_cast<f64>(u4) * G_TWO_POW_NEG64 - 0.5) * 0.5;
        switch (i % 4) {
          case 0:
            b.x = 2.0 + off;
            b.y = along;
            b.vx = 0.0 - speed;
            b.vy = drift;
            break;
          case 1:
            b.x = 124.0 + off;
            b.y = along;
            b.vx = speed;
            b.vy = drift;
            break;
          case 2:
            b.x = along;
            b.y = 2.0 + off;
            b.vx = drift;
            b.vy = 0.0 - speed;
            break;
          default:
            b.x = along;
            b.y = 124.0 + off;
            b.vx = drift;
            b.vy = speed;
            break;
        }
      } else if (std::strcmp(ic_profile, "coarsehit") == 0) {
        // Interceptors (ids 0..3) aimed at a demoted target cluster
        // (ids 4..7) over shared y lanes; the fine body carries the
        // smaller id because the section 21/24 sweep is lexicographic.
        const f64 lane =
            77.0 + 8.0 * static_cast<f64>(i % 4) + static_cast<f64>(u2) * G_TWO_POW_NEG64 * 2.0;
        if (i < 4) {
          b.x = 56.0 + static_cast<f64>(u1) * G_TWO_POW_NEG64 * 4.0;
          b.y = lane;
          b.vx = 56.0 + static_cast<f64>(u3) * G_TWO_POW_NEG64 * 16.0;
          b.vy = (static_cast<f64>(u4) * G_TWO_POW_NEG64 - 0.5) * 0.5;
        } else {
          b.x = 84.0 + static_cast<f64>(u1) * G_TWO_POW_NEG64 * 4.0;
          b.y = lane;
          b.vx = (static_cast<f64>(u3) * G_TWO_POW_NEG64 - 0.5) * 0.5;
          b.vy = (static_cast<f64>(u4) * G_TWO_POW_NEG64 - 0.5) * 0.5;
        }
      } else {
        std::fprintf(stderr, "error: unknown corpus profile '%s'\n", ic_profile);
        std::exit(3);
      }
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

  bool is_coarse(std::size_t i) const { return blevel[i] == 0; }

  GBody state_at(std::size_t i, u64 t) const {
    GBody b = bodies[i];
    if (blevel[i] == 2) {
      const GCollapsed &c = collapsed[body_region[i]];
      b.x = c.com_x + body_jx[i];
      b.y = c.com_y + body_jy[i];
      b.vx = c.vcx;
      b.vy = c.vcy;
      return b;
    }
    if (is_coarse(i)) {
      const GFit &fit = coarse[i];
      const f64 s = -1.0 + static_cast<f64>(t - fit.t0) / 16.0;
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
    rstate[region] = 0;
    if (members.empty()) {
      return;
    }
    fit_members(members, t0);
    for (std::size_t i : members) {
      body_region[i] = static_cast<u8>(region);
      blevel[i] = 0;
    }
  }

  // Section 19/26 collapse: materialize ephemeris evaluations at the
  // boundary tick (terminating the region's own window — out-of-box
  // bodies leave as unmanaged fine, the section 14 refit exit rule),
  // select in-box bodies by evaluated state with collapsed bodies
  // excluded (the section 14 demote rule; foreign windows are absorbed
  // and their fits discarded), freeze the totals (fixed id-order sums),
  // and draw the frozen jitter from a re-seeded generator. Returns the
  // computed totals for record validation.
  GCollapsed collapse(int region, u64 t) {
    const f64 x0 = static_cast<f64>(region % 2) * 64.0;
    const f64 y0 = static_cast<f64>(region / 2) * 64.0;
    if (rstate[region] != 1) {
      for (std::size_t i = 0; i < bodies.size(); ++i) {
        if (body_region[i] == region && blevel[i] != 1) {
          bodies[i] = state_at(i, t);
          body_region[i] = G_UNMANAGED;
          blevel[i] = 1;
        }
      }
      collapsed[region] = GCollapsed{};
    }
    GCollapsed c;
    std::vector<std::size_t> members;
    for (std::size_t i = 0; i < bodies.size(); ++i) {
      if (blevel[i] == 2) {
        continue;
      }
      const GBody b = state_at(i, t);
      if (b.x >= x0 && b.x < x0 + 64.0 && b.y >= y0 && b.y < y0 + 64.0) {
        members.push_back(i);
      }
    }
    for (std::size_t i : members) {
      if (blevel[i] == 0) {
        bodies[i] = state_at(i, t);
        body_region[i] = G_UNMANAGED;
        blevel[i] = 1;
      }
    }
    c.members = members;
    c.n = c.members.size();
    f64 sum_mx = 0.0;
    f64 sum_my = 0.0;
    f64 ke = 0.0;
    for (std::size_t i : c.members) {
      const GBody &b = bodies[i];
      c.mass += b.mass;
      sum_mx += b.mass * b.x;
      sum_my += b.mass * b.y;
      c.pxt += b.mass * b.vx;
      c.pyt += b.mass * b.vy;
      ke += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy);
    }
    f64 pe = 0.0;
    f64 binding = 0.0;
    for (std::size_t a = 0; a < c.members.size(); ++a) {
      for (std::size_t b = a + 1; b < c.members.size(); ++b) {
        const GBody &ba = bodies[c.members[a]];
        const GBody &bb = bodies[c.members[b]];
        const f64 dx = bb.x - ba.x;
        const f64 dy = bb.y - ba.y;
        const f64 s2 = dx * dx + dy * dy + G_EPS2;
        pe -= ba.mass * bb.mass / std::sqrt(s2);
        binding += ba.mass * bb.mass / std::sqrt(s2);
      }
    }
    c.energy = ke + pe;
    c.binding = binding;
    if (c.n > 0) {
      c.com_x = sum_mx / c.mass;
      c.com_y = sum_my / c.mass;
      c.vcx = c.pxt / c.mass;
      c.vcy = c.pyt / c.mass;
    }
    // Section 20 multipole totals: the exact dipole accumulators and the
    // second central moments about com (id order, left-to-right sums).
    c.mx = sum_mx;
    c.my = sum_my;
    for (std::size_t i : c.members) {
      const GBody &b = bodies[i];
      const f64 dx = b.x - c.com_x;
      const f64 dy = b.y - c.com_y;
      c.qxx += b.mass * dx * dx;
      c.qxy += b.mass * dx * dy;
      c.qyy += b.mass * dy * dy;
    }
    c.mp = mp_enabled;
    c.radial = radial_enabled;
    c.shells = shells_enabled;
    if (c.n > 0) {
      // Section 25 rank shells of the materialized layout about com;
      // per-shell intra-shell bindings freeze alongside the total.
      std::vector<f64> radii(c.members.size());
      for (std::size_t a = 0; a < c.members.size(); ++a) {
        const f64 dx = bodies[c.members[a]].x - c.com_x;
        const f64 dy = bodies[c.members[a]].y - c.com_y;
        radii[a] = std::sqrt(dx * dx + dy * dy);
      }
      const std::vector<int> shells = g_shell_assignment(radii);
      for (std::size_t a = 0; a < c.members.size(); ++a) {
        for (std::size_t b = a + 1; b < c.members.size(); ++b) {
          if (shells[a] != shells[b]) {
            continue;
          }
          const GBody &ba = bodies[c.members[a]];
          const GBody &bb = bodies[c.members[b]];
          const f64 dx = bb.x - ba.x;
          const f64 dy = bb.y - ba.y;
          const f64 s2 = dx * dx + dy * dy + G_EPS2;
          c.shell_b[shells[a]] += ba.mass * bb.mass / std::sqrt(s2);
        }
      }
    }
    SplitMix64 jitter(seed ^ (static_cast<u64>(region) * 0x9E3779B97F4A7C15ULL));
    for (std::size_t i : c.members) {
      const u64 ux = jitter.draw();
      const u64 uy = jitter.draw();
      body_jx[i] = (static_cast<f64>(ux) * G_TWO_POW_NEG64 - 0.5) * 8.0;
      body_jy[i] = (static_cast<f64>(uy) * G_TWO_POW_NEG64 - 0.5) * 8.0;
    }
    c.jitter_state = jitter.state;
    for (std::size_t i : c.members) {
      body_region[i] = static_cast<u8>(region);
      blevel[i] = 2;
    }
    rstate[region] = 2;
    c.active = true;
    collapsed[region] = c;
    return c;
  }

  // Section 19/20/23 expansion: positions per mode (section 20 synthesizes
  // under dipole-exact + quadrupole-transform constraints; section 23 adds
  // the binding-matched radial scale + spread-scale energy closure; section
  // 19 keeps com + raw jitter), velocities v_com + spread with the last body
  // absorbing the exact momentum residual (bit-identical per the spec).
  f64 radial_scale(std::vector<std::pair<f64, f64>> &base, const GCollapsed &c) {
    if (c.members.size() < 2) {
      return 0.0;
    }
    std::vector<std::pair<f64, f64>> pairs;
    for (std::size_t a = 0; a < c.members.size(); ++a) {
      for (std::size_t b = a + 1; b < c.members.size(); ++b) {
        const f64 w = bodies[c.members[a]].mass * bodies[c.members[b]].mass;
        const f64 dx = base[b].first - base[a].first;
        const f64 dy = base[b].second - base[a].second;
        pairs.emplace_back(w, dx * dx + dy * dy);
      }
    }
    const auto f = [&](f64 lam) {
      f64 total = 0.0;
      for (const auto &p : pairs) {
        total += p.first / std::sqrt(lam * lam * p.second + 1.0);
      }
      return total;
    };
    const f64 target = c.binding;
    f64 lam;
    if (target >= f(0.0)) {
      lam = 0.0;
    } else {
      f64 hi = 1.0;
      int doublings = 0;
      while (f(hi) > target && doublings < 64) {
        hi *= 2.0;
        ++doublings;
      }
      f64 lo = 0.0;
      for (int it = 0; it < 128; ++it) {
        const f64 mid = (lo + hi) * 0.5;
        if (f(mid) >= target) {
          lo = mid;
        } else {
          hi = mid;
        }
      }
      lam = (lo + hi) * 0.5;
    }
    for (auto &b : base) {
      b.first *= lam;
      b.second *= lam;
    }
    f64 swx = 0.0;
    f64 swy = 0.0;
    for (std::size_t a = 0; a < c.members.size(); ++a) {
      const f64 m = bodies[c.members[a]].mass;
      swx += m * base[a].first;
      swy += m * base[a].second;
    }
    const f64 wx = swx / c.mass;
    const f64 wy = swy / c.mass;
    for (auto &b : base) {
      b.first -= wx;
      b.second -= wy;
    }
    return f(lam);
  }

  // Section 25 per-shell radial synthesis: one global scale closes the
  // recorded total binding (the section 23 bisection), then per-shell
  // corrections in pinned shell order close each shell's intra-shell
  // binding, then the section 23 recenter. Returns F_total over the final
  // displacements (the sigma solve's binding input).
  f64 shell_scale(std::vector<std::pair<f64, f64>> &base, const GCollapsed &c) {
    const std::size_t n = c.members.size();
    if (n < 2) {
      return 0.0;
    }
    const auto solve = [](const std::vector<std::pair<f64, f64>> &pairs, f64 target) {
      const auto f = [&pairs](f64 lam) {
        f64 total = 0.0;
        for (const auto &p : pairs) {
          total += p.first / std::sqrt(lam * lam * p.second + 1.0);
        }
        return total;
      };
      if (target >= f(0.0)) {
        return 0.0;
      }
      f64 hi = 1.0;
      int doublings = 0;
      while (f(hi) > target && doublings < 64) {
        hi *= 2.0;
        ++doublings;
      }
      f64 lo = 0.0;
      for (int it = 0; it < 128; ++it) {
        const f64 mid = (lo + hi) * 0.5;
        if (f(mid) >= target) {
          lo = mid;
        } else {
          hi = mid;
        }
      }
      return (lo + hi) * 0.5;
    };
    std::vector<std::pair<f64, f64>> all_pairs;
    for (std::size_t a = 0; a < n; ++a) {
      for (std::size_t b = a + 1; b < n; ++b) {
        const f64 w = bodies[c.members[a]].mass * bodies[c.members[b]].mass;
        const f64 dx = base[b].first - base[a].first;
        const f64 dy = base[b].second - base[a].second;
        all_pairs.emplace_back(w, dx * dx + dy * dy);
      }
    }
    const f64 lam = solve(all_pairs, c.binding);
    for (auto &b : base) {
      b.first *= lam;
      b.second *= lam;
    }
    f64 swx = 0.0;
    f64 swy = 0.0;
    for (std::size_t a = 0; a < n; ++a) {
      const f64 m = bodies[c.members[a]].mass;
      swx += m * base[a].first;
      swy += m * base[a].second;
    }
    const f64 cx = swx / c.mass;
    const f64 cy = swy / c.mass;
    std::vector<f64> radii(n);
    for (std::size_t a = 0; a < n; ++a) {
      const f64 dx = base[a].first - cx;
      const f64 dy = base[a].second - cy;
      radii[a] = std::sqrt(dx * dx + dy * dy);
    }
    const std::vector<int> shells = g_shell_assignment(radii);
    const int s = g_shell_count(n);
    std::vector<std::vector<std::pair<f64, f64>>> pairs(s);
    for (std::size_t a = 0; a < n; ++a) {
      for (std::size_t b = a + 1; b < n; ++b) {
        if (shells[a] != shells[b]) {
          continue;
        }
        const f64 w = bodies[c.members[a]].mass * bodies[c.members[b]].mass;
        const f64 dx = base[b].first - base[a].first;
        const f64 dy = base[b].second - base[a].second;
        pairs[shells[a]].emplace_back(w, dx * dx + dy * dy);
      }
    }
    f64 mus[4] = {1.0, 1.0, 1.0, 1.0};
    for (int k = 0; k < s; ++k) {
      if (pairs[k].empty() || c.shell_b[k] <= 0.0) {
        continue;
      }
      mus[k] = solve(pairs[k], c.shell_b[k]);
    }
    for (std::size_t a = 0; a < n; ++a) {
      base[a].first *= mus[shells[a]];
      base[a].second *= mus[shells[a]];
    }
    swx = 0.0;
    swy = 0.0;
    for (std::size_t a = 0; a < n; ++a) {
      const f64 m = bodies[c.members[a]].mass;
      swx += m * base[a].first;
      swy += m * base[a].second;
    }
    const f64 wx = swx / c.mass;
    const f64 wy = swy / c.mass;
    for (auto &b : base) {
      b.first -= wx;
      b.second -= wy;
    }
    f64 ft = 0.0;
    for (std::size_t a = 0; a < n; ++a) {
      for (std::size_t b = a + 1; b < n; ++b) {
        const f64 w = bodies[c.members[a]].mass * bodies[c.members[b]].mass;
        const f64 dx = base[b].first - base[a].first;
        const f64 dy = base[b].second - base[a].second;
        ft += w / std::sqrt(dx * dx + dy * dy + 1.0);
      }
    }
    return ft;
  }

  void expand(int region) {
    GCollapsed &c = collapsed[region];
    f64 sigma = 1.0;
    if (c.mp) {
      std::vector<std::pair<f64, f64>> base;
      base.reserve(c.members.size());
      for (std::size_t i : c.members) {
        base.emplace_back(body_jx[i], body_jy[i]);
      }
      if (c.members.size() >= 3) {
        f64 swx = 0.0;
        f64 swy = 0.0;
        for (std::size_t a = 0; a < c.members.size(); ++a) {
          const f64 m = bodies[c.members[a]].mass;
          swx += m * base[a].first;
          swy += m * base[a].second;
        }
        const f64 wx = swx / c.mass;
        const f64 wy = swy / c.mass;
        std::vector<std::pair<f64, f64>> dhat(c.members.size());
        f64 jxx = 0.0;
        f64 jxy = 0.0;
        f64 jyy = 0.0;
        for (std::size_t a = 0; a < c.members.size(); ++a) {
          const f64 m = bodies[c.members[a]].mass;
          const f64 dx = base[a].first - wx;
          const f64 dy = base[a].second - wy;
          jxx += m * dx * dx;
          jxy += m * dx * dy;
          jyy += m * dy * dy;
          dhat[a] = {dx, dy};
        }
        if (jxx > 0.0 && c.qxx > 0.0) {
          const f64 lj00 = std::sqrt(jxx);
          const f64 lj10 = jxy / lj00;
          const f64 jjd = jyy - lj10 * lj10;
          if (jjd > 0.0) {
            const f64 lq00 = std::sqrt(c.qxx);
            const f64 lq10 = c.qxy / lq00;
            const f64 qqd = c.qyy - lq10 * lq10;
            if (qqd > 0.0) {
              const f64 lj11 = std::sqrt(jjd);
              const f64 lq11 = std::sqrt(qqd);
              const f64 u00 = 1.0 / lj00;
              const f64 u11 = 1.0 / lj11;
              const f64 u10 = -(lj10 / (lj00 * lj11));
              const f64 a00 = lq00 * u00;
              const f64 a11 = lq11 * u11;
              const f64 a10 = lq10 * u00 + lq11 * u10;
              for (std::size_t a = 0; a < c.members.size(); ++a) {
                base[a] = {a00 * dhat[a].first, a10 * dhat[a].first + a11 * dhat[a].second};
              }
            }
          }
        }
      }
      f64 fl = 0.0;
      if (c.radial) {
        fl = radial_scale(base, c);
      }
      if (c.shells) {
        fl = shell_scale(base, c);
      }
      f64 sum_mx = 0.0;
      f64 sum_my = 0.0;
      for (std::size_t a = 0; a + 1 < c.members.size(); ++a) {
        GBody &b = bodies[c.members[a]];
        b.x = c.com_x + base[a].first;
        b.y = c.com_y + base[a].second;
        sum_mx += b.mass * b.x;
        sum_my += b.mass * b.y;
      }
      if (!c.members.empty()) {
        const std::size_t last = c.members.back();
        bodies[last].x = (c.mx - sum_mx) / bodies[last].mass;
        bodies[last].y = (c.my - sum_my) / bodies[last].mass;
      }
      if (c.radial || c.shells) {
        // Section 23/25 spread-scale: close the synthesized kinetic energy
        // on the recorded total via the three-point parabola through
        // ke(sigma); shells feed the binding input F_total from the
        // per-shell-scaled displacements.
        std::vector<std::pair<f64, f64>> spread(c.members.size());
        SplitMix64 sj(c.jitter_state);
        for (std::size_t a = 0; a < c.members.size(); ++a) {
          if (a + 1 < c.members.size()) {
            const u64 ux = sj.draw();
            const u64 uy = sj.draw();
            spread[a] = {(static_cast<f64>(ux) * G_TWO_POW_NEG64 - 0.5) * 0.1,
                         (static_cast<f64>(uy) * G_TWO_POW_NEG64 - 0.5) * 0.1};
          }
        }
        const auto synth_velocities = [&](f64 s) {
          std::vector<std::pair<f64, f64>> vs(c.members.size());
          f64 smx = 0.0;
          f64 smy = 0.0;
          for (std::size_t a = 0; a < c.members.size(); ++a) {
            const std::size_t i = c.members[a];
            if (a + 1 < c.members.size()) {
              const f64 vx = c.vcx + s * spread[a].first;
              const f64 vy = c.vcy + s * spread[a].second;
              smx += bodies[i].mass * vx;
              smy += bodies[i].mass * vy;
              vs[a] = {vx, vy};
            } else {
              vs[a] = {(c.pxt - smx) / bodies[i].mass, (c.pyt - smy) / bodies[i].mass};
            }
          }
          return vs;
        };
        const auto synth_ke = [&](f64 s) {
          const auto vs = synth_velocities(s);
          f64 ke = 0.0;
          for (std::size_t a = 0; a < c.members.size(); ++a) {
            const f64 m = bodies[c.members[a]].mass;
            ke += 0.5 * m * (vs[a].first * vs[a].first + vs[a].second * vs[a].second);
          }
          return ke;
        };
        const f64 c0 = synth_ke(0.0);
        const f64 c1 = synth_ke(1.0);
        const f64 cm = synth_ke(-1.0);
        const f64 qa = ((c1 + cm) - (c0 + c0)) * 0.5;
        const f64 qb = (c1 - cm) * 0.5;
        const f64 k_target = c.energy + fl;
        const f64 disc = qb * qb - 4.0 * qa * (c0 - k_target);
        if (qa == 0.0) {
          sigma = 0.0;
        } else if (disc < 0.0) {
          sigma = (0.0 - qb) / (2.0 * qa);
        } else {
          const f64 sq = std::sqrt(disc);
          const f64 r1 = ((0.0 - qb) + sq) / (2.0 * qa);
          const f64 r2 = ((0.0 - qb) - sq) / (2.0 * qa);
          sigma = r1 * r1 <= r2 * r2 ? r1 : r2;
        }
        const auto vs = synth_velocities(sigma);
        for (std::size_t a = 0; a < c.members.size(); ++a) {
          bodies[c.members[a]].vx = vs[a].first;
          bodies[c.members[a]].vy = vs[a].second;
        }
        for (std::size_t i : c.members) {
          blevel[i] = 1;
          body_region[i] = G_UNMANAGED;
        }
        c = GCollapsed{};
        rstate[region] = 1;
        return;
      }
    }
    SplitMix64 jitter(c.jitter_state);
    f64 sum_mv_x = 0.0;
    f64 sum_mv_y = 0.0;
    f64 last_mass = 0.0;
    for (std::size_t a = 0; a < c.members.size(); ++a) {
      const std::size_t i = c.members[a];
      GBody &b = bodies[i];
      if (!c.mp) {
        b.x = c.com_x + body_jx[i];
        b.y = c.com_y + body_jy[i];
      }
      if (a + 1 < c.members.size()) {
        const u64 ux = jitter.draw();
        const u64 uy = jitter.draw();
        b.vx = c.vcx + (static_cast<f64>(ux) * G_TWO_POW_NEG64 - 0.5) * 0.1;
        b.vy = c.vcy + (static_cast<f64>(uy) * G_TWO_POW_NEG64 - 0.5) * 0.1;
        sum_mv_x += b.mass * b.vx;
        sum_mv_y += b.mass * b.vy;
      } else {
        last_mass = b.mass;
      }
      blevel[i] = 1;
      body_region[i] = G_UNMANAGED;
    }
    if (!c.members.empty()) {
      const std::size_t last = c.members.back();
      bodies[last].vx = (c.pxt - sum_mv_x) / last_mass;
      bodies[last].vy = (c.pyt - sum_mv_y) / last_mass;
    }
    c = GCollapsed{};
    rstate[region] = 1;
  }

  void promote(int region, u64 t) {
    if (rstate[region] == 2) {
      expand(region);
      return;
    }
    for (std::size_t i = 0; i < bodies.size(); ++i) {
      if (blevel[i] == 0 && body_region[i] == region) {
        bodies[i] = state_at(i, t);
        body_region[i] = G_UNMANAGED;
        blevel[i] = 1;
      }
    }
    rstate[region] = 1;
  }

  void refit(int region, u64 t) {
    const f64 x0 = static_cast<f64>(region % 2) * 64.0;
    const f64 y0 = static_cast<f64>(region / 2) * 64.0;
    std::vector<std::size_t> members;
    for (std::size_t i = 0; i < bodies.size(); ++i) {
      if (blevel[i] == 0 && body_region[i] == region) {
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
        blevel[i] = 1;
      }
    }
    if (keep.empty()) {
      rstate[region] = 1;
      return;
    }
    fit_members(keep, t);
    for (std::size_t i : keep) {
      body_region[i] = static_cast<u8>(region);
      blevel[i] = 0;
    }
  }

  // view holds each body's state at the tick being entered; flags carry the
  // per-body level byte (0 ephemeris-coarse, 1 fine, 2 collapsed). Pair
  // rules: fine-fine -> symmetric ff; fine x ephemeris -> one-sided fc;
  // anything touching a collapsed body -> skipped (the region acts as a
  // monopole instead). After all pairs, fine bodies accumulate the
  // collapsed-region monopoles in region index order (section 19), into the
  // fc arrays (one-sided, ledger-tracked like ephemeris pairs).
  void accel_split(const std::vector<GBody> &view, const std::vector<u8> &flags,
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
        if (flags[i] == 1 && flags[j] == 1) {
          ax_ff[i] += view[j].mass * fx;
          ay_ff[i] += view[j].mass * fy;
          ax_ff[j] -= view[i].mass * fx;
          ay_ff[j] -= view[i].mass * fy;
        } else if (flags[i] == 1 && flags[j] == 0) {
          ax_fc[i] += view[j].mass * fx;
          ay_fc[i] += view[j].mass * fy;
        } else if (flags[i] == 0 && flags[j] == 1) {
          ax_fc[j] -= view[i].mass * fx;
          ay_fc[j] -= view[i].mass * fy;
        }
      }
    }
    for (int region = 0; region < 4; ++region) {
      const GCollapsed &c = collapsed[region];
      if (!c.active || c.n == 0) {
        continue;
      }
      for (std::size_t i = 0; i < n; ++i) {
        if (flags[i] != 1) {
          continue;
        }
        const f64 dx = c.com_x - view[i].x;
        const f64 dy = c.com_y - view[i].y;
        const f64 s2 = dx * dx + dy * dy + G_EPS2;
        const f64 inv3 = 1.0 / (s2 * std::sqrt(s2));
        ax_fc[i] += c.mass * (G_CONST * inv3 * dx);
        ay_fc[i] += c.mass * (G_CONST * inv3 * dy);
      }
    }
  }

  // Section 24 one-sided impulse vs a frozen contactant: only the fine body
  // changes; the ledger books the intended impulse exactly (fc-kick rule).
  std::pair<f64, f64> static_impulse(std::size_t i, f64 nx, f64 ny, f64 vrx, f64 vry) {
    const f64 e = restitution;
    const f64 fr = friction;
    const f64 mi = bodies[i].mass;
    const f64 vn = vrx * nx + vry * ny;
    const f64 s = (1.0 + e) * vn;
    bodies[i].vx += s * nx;
    bodies[i].vy += s * ny;
    px += mi * (s * nx);
    py += mi * (s * ny);
    const f64 jn = (0.0 - s) * mi;
    if (fr > 0.0) {
      const f64 vt = (0.0 - vrx) * ny + vry * nx;
      f64 jt = vt * mi;
      const f64 jt_max = fr * jn;
      if (jt > jt_max) {
        jt = jt_max;
      }
      if (jt < 0.0 - jt_max) {
        jt = 0.0 - jt_max;
      }
      const f64 w = jt / mi;
      bodies[i].vx += w * (0.0 - ny);
      bodies[i].vy += w * nx;
      px += jt * (0.0 - ny);
      py += jt * nx;
    }
    return {vn, jn};
  }

  // Section 21 + 24 contact pass: single pinned lexicographic impulse sweep
  // over fine pairs (fine x coarse static pairs included when the
  // ContactParams record is present), then collapsed-region monopoles in
  // region order, then walls in body order — applied immediately, after the
  // second fc kick.
  void contact_pass(u64 entering, const std::vector<u8> &flags) {
    const std::size_t n = bodies.size();
    std::vector<f64> radii(n);
    for (std::size_t i = 0; i < n; ++i) {
      radii[i] = C_CONTACT_R * std::sqrt(bodies[i].mass);
    }
    const f64 e = restitution;
    const f64 fr = friction;
    const bool extended = contacts_params;
    std::vector<std::pair<u32, u32>> nxt;
    std::vector<GContact> events;
    for (std::size_t i = 0; i < n; ++i) {
      if (flags[i] != 1) {
        continue;
      }
      for (std::size_t j = i + 1; j < n; ++j) {
        if (flags[j] == 2) {
          continue;
        }
        if (flags[j] == 0 && !extended) {
          continue;
        }
        const GBody sj = state_at(j, entering);
        const f64 dx = sj.x - bodies[i].x;
        const f64 dy = sj.y - bodies[i].y;
        const f64 rs = radii[i] + radii[j];
        const f64 d2 = dx * dx + dy * dy;
        if (d2 >= rs * rs) {
          continue;
        }
        const std::pair<u32, u32> pair(static_cast<u32>(i), static_cast<u32>(j));
        nxt.push_back(pair);
        f64 nx = 1.0;
        f64 ny = 0.0;
        if (d2 != 0.0) {
          const f64 dist = std::sqrt(d2);
          nx = dx / dist;
          ny = dy / dist;
        }
        const f64 vrx = sj.vx - bodies[i].vx;
        const f64 vry = sj.vy - bodies[i].vy;
        const f64 vn = vrx * nx + vry * ny;
        if (vn >= 0.0) {
          continue;
        }
        const f64 mi = bodies[i].mass;
        const f64 mj = bodies[j].mass;
        const f64 cx = (bodies[i].x + sj.x) * 0.5;
        const f64 cy = (bodies[i].y + sj.y) * 0.5;
        f64 jn;
        f64 mu;
        if (flags[j] == 0) {
          const std::pair<f64, f64> res = static_impulse(i, nx, ny, vrx, vry);
          jn = res.second;
          mu = mi;
        } else {
          const f64 inv = 1.0 / (mi + mj);
          const f64 t = vn * inv;
          const f64 s = (1.0 + e) * t;
          const f64 fi = s * mj;
          const f64 fj = s * mi;
          bodies[i].vx += fi * nx;
          bodies[i].vy += fi * ny;
          bodies[j].vx -= fj * nx;
          bodies[j].vy -= fj * ny;
          mu = (mi * mj) / (mi + mj);
          jn = ((0.0 - vn) * (1.0 + e)) * mu;
          if (fr > 0.0) {
            const f64 vt = (0.0 - vrx) * ny + vry * nx;
            f64 q = vt * inv;
            const f64 qmax = (fr * jn) * inv;
            if (q > qmax) {
              q = qmax;
            }
            if (q < 0.0 - qmax) {
              q = 0.0 - qmax;
            }
            const f64 fti = q * mj;
            const f64 ftj = q * mi;
            bodies[i].vx += fti * (0.0 - ny);
            bodies[i].vy += fti * nx;
            bodies[j].vx -= ftj * (0.0 - ny);
            bodies[j].vy -= ftj * nx;
          }
        }
        bool was = false;
        for (const auto &p : touching) {
          if (p == pair) {
            was = true;
            break;
          }
        }
        if (was) {
          continue;
        }
        GContact c;
        c.tick = entering;
        c.a = static_cast<u32>(i);
        c.b = static_cast<u32>(j);
        c.jn = jn;
        c.cx = cx;
        c.cy = cy;
        c.vn = vn;
        // The contactant is measured at its post-impulse state for fine
        // pairs (flags 1) and at its (frozen) polynomial evaluation for
        // coarse pairs (flags 0) — never at the stale demote-time slot.
        c.vn_after =
            flags[j] == 1
                ? (bodies[j].vx - bodies[i].vx) * nx + (bodies[j].vy - bodies[i].vy) * ny
                : (sj.vx - bodies[i].vx) * nx + (sj.vy - bodies[i].vy) * ny;
        c.mu = mu;
        events.push_back(c);
      }
    }
    if (extended) {
      for (int region = 0; region < 4; ++region) {
        const GCollapsed &cc = collapsed[region];
        if (!cc.active || cc.n == 0) {
          continue;
        }
        const f64 big_r = C_CONTACT_R * std::sqrt(cc.mass);
        for (std::size_t i = 0; i < n; ++i) {
          if (flags[i] != 1) {
            continue;
          }
          const f64 dx = cc.com_x - bodies[i].x;
          const f64 dy = cc.com_y - bodies[i].y;
          const f64 rs = radii[i] + big_r;
          const f64 d2 = dx * dx + dy * dy;
          if (d2 >= rs * rs) {
            continue;
          }
          const std::pair<u32, u32> pair(static_cast<u32>(i), C_MONOPOLE_BASE + region);
          nxt.push_back(pair);
          f64 nx = 1.0;
          f64 ny = 0.0;
          if (d2 != 0.0) {
            const f64 dist = std::sqrt(d2);
            nx = dx / dist;
            ny = dy / dist;
          }
          const f64 vrx = cc.vcx - bodies[i].vx;
          const f64 vry = cc.vcy - bodies[i].vy;
          const f64 vn = vrx * nx + vry * ny;
          if (vn >= 0.0) {
            continue;
          }
          const f64 mi = bodies[i].mass;
          const f64 cx = (bodies[i].x + cc.com_x) * 0.5;
          const f64 cy = (bodies[i].y + cc.com_y) * 0.5;
          const std::pair<f64, f64> res = static_impulse(i, nx, ny, vrx, vry);
          const f64 mu = (mi * cc.mass) / (mi + cc.mass);
          bool was = false;
          for (const auto &p : touching) {
            if (p == pair) {
              was = true;
              break;
            }
          }
          if (was) {
            continue;
          }
          GContact c;
          c.tick = entering;
          c.a = static_cast<u32>(i);
          c.b = C_MONOPOLE_BASE + region;
          c.jn = res.second;
          c.cx = cx;
          c.cy = cy;
          c.vn = vn;
          c.vn_after = (cc.vcx - bodies[i].vx) * nx + (cc.vcy - bodies[i].vy) * ny;
          c.mu = mu;
          events.push_back(c);
        }
      }
    }
    if (walls_on) {
      for (std::size_t i = 0; i < n; ++i) {
        if (flags[i] != 1) {
          continue;
        }
        for (u32 wall = 0; wall < 4; ++wall) {
          f64 nx = 0.0;
          f64 ny = 0.0;
          f64 cx = 0.0;
          f64 cy = 0.0;
          if (wall == 0) {
            if (!(bodies[i].x - radii[i] < 0.0 && bodies[i].vx < 0.0)) {
              continue;
            }
            nx = 0.0 - 1.0;
            cx = (bodies[i].x + 0.0) * 0.5;
            cy = bodies[i].y;
          } else if (wall == 1) {
            if (!(bodies[i].x + radii[i] > 128.0 && bodies[i].vx > 0.0)) {
              continue;
            }
            nx = 1.0;
            cx = (bodies[i].x + 128.0) * 0.5;
            cy = bodies[i].y;
          } else if (wall == 2) {
            if (!(bodies[i].y - radii[i] < 0.0 && bodies[i].vy < 0.0)) {
              continue;
            }
            ny = 0.0 - 1.0;
            cx = bodies[i].x;
            cy = (bodies[i].y + 0.0) * 0.5;
          } else {
            if (!(bodies[i].y + radii[i] > 128.0 && bodies[i].vy > 0.0)) {
              continue;
            }
            ny = 1.0;
            cx = bodies[i].x;
            cy = (bodies[i].y + 128.0) * 0.5;
          }
          const std::pair<u32, u32> pair(static_cast<u32>(i), C_WALL_BASE + wall);
          nxt.push_back(pair);
          const f64 vrx = 0.0 - bodies[i].vx;
          const f64 vry = 0.0 - bodies[i].vy;
          const f64 vn_pre = vrx * nx + vry * ny;
          if (vn_pre >= 0.0) {
            continue;
          }
          const std::pair<f64, f64> res = static_impulse(i, nx, ny, vrx, vry);
          bool was = false;
          for (const auto &p : touching) {
            if (p == pair) {
              was = true;
              break;
            }
          }
          if (was) {
            continue;
          }
          GContact c;
          c.tick = entering;
          c.a = static_cast<u32>(i);
          c.b = C_WALL_BASE + wall;
          c.jn = res.second;
          c.cx = cx;
          c.cy = cy;
          c.vn = res.first;
          c.vn_after = (0.0 - bodies[i].vx) * nx + (0.0 - bodies[i].vy) * ny;
          c.mu = bodies[i].mass;
          events.push_back(c);
        }
      }
    }
    touching = std::move(nxt);
    last_contacts.insert(last_contacts.end(), events.begin(), events.end());
  }

  void step() {
    const u64 entering = tick + 1;
    for (int region = 0; region < 4; ++region) {
      if (rstate[region] != 0) {
        continue;
      }
      bool ended = false;
      for (std::size_t i = 0; i < bodies.size(); ++i) {
        if (body_region[i] == region && blevel[i] == 0 && entering == coarse[i].t0 + G_WINDOW) {
          ended = true;
          break;
        }
      }
      if (ended) {
        refit(region, entering);
      }
    }
    const std::size_t n = bodies.size();
    std::vector<u8> cflag(n);
    for (std::size_t i = 0; i < n; ++i) {
      cflag[i] = blevel[i];
    }
    const f64 half = G_DT * 0.5;
    std::vector<GBody> view(n);
    for (std::size_t i = 0; i < n; ++i) {
      view[i] = state_at(i, entering);
    }
    std::vector<f64> ax_ff, ay_ff, ax_fc, ay_fc;
    accel_split(view, cflag, ax_ff, ay_ff, ax_fc, ay_fc);
    for (std::size_t i = 0; i < n; ++i) {
      if (cflag[i] == 1) {
        bodies[i].vx += ax_ff[i] * half;
        bodies[i].vy += ay_ff[i] * half;
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (cflag[i] == 1) {
        bodies[i].vx += ax_fc[i] * half;
        bodies[i].vy += ay_fc[i] * half;
        px += bodies[i].mass * (ax_fc[i] * half);
        py += bodies[i].mass * (ay_fc[i] * half);
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (cflag[i] == 1) {
        bodies[i].x += bodies[i].vx * G_DT;
        bodies[i].y += bodies[i].vy * G_DT;
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      view[i] = state_at(i, entering);
    }
    accel_split(view, cflag, ax_ff, ay_ff, ax_fc, ay_fc);
    for (std::size_t i = 0; i < n; ++i) {
      if (cflag[i] == 1) {
        bodies[i].vx += ax_ff[i] * half;
        bodies[i].vy += ay_ff[i] * half;
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (cflag[i] == 1) {
        bodies[i].vx += ax_fc[i] * half;
        bodies[i].vy += ay_fc[i] * half;
        px += bodies[i].mass * (ax_fc[i] * half);
        py += bodies[i].mass * (ay_fc[i] * half);
      }
    }
    if (contacts_on) {
      contact_pass(entering, cflag);
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
      if (blevel[b.id] == 1) {
        ++fine;
      } else {
        // Ephemeris-coarse and collapsed both count as coarse
        // (section 19 clarification: fine + coarse = N).
        ++coarse_n;
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
    out[44] = blevel[i];
  }

  void emitted(std::size_t i, u8 &region, u8 &level, GBody &b) const {
    b = state_at(i, tick);
    level = blevel[i];
    region = blevel[i] == 1 ? static_cast<u8>(g_region_at(b.x, b.y)) : body_region[i];
  }

  u64 region_hash(int region, u8 &level, u64 &pop) const {
    std::vector<std::size_t> members;
    for (std::size_t i = 0; i < bodies.size(); ++i) {
      if (blevel[i] != 1) {
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
    level = rstate[region];
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

static int run_gravity(const std::vector<u8> &data, u64 seed, u32 body_count,
                       const char *wav_out, const char *ic_profile) {
  GravityWorld world(seed, body_count, ic_profile);
  GravityWorld reference(seed, body_count, ic_profile);
  struct Pending {
    int region;
    u8 level;
  };
  struct PendingCollapse {
    u64 t;
    int region;
    u64 n;
    f64 mass, com_x, com_y, pxt, pyt, energy;
  };
  struct PendingMultipole {
    u64 t;
    int region;
    f64 mx, my, qxx, qxy, qyy;
  };
  struct PendingContact {
    u64 t;
    u32 a, b;
    f64 jn, cx, cy;
  };
  struct PendingRadial {
    u64 t;
    int region;
    f64 binding;
  };
  struct PendingShells {
    u64 t;
    int region;
    f64 binding;
    f64 b[4];
  };
  std::vector<Pending> pending;
  std::vector<PendingCollapse> pending_collapsed;
  std::vector<PendingMultipole> pending_multipole;
  std::vector<PendingContact> pending_contact;
  std::vector<PendingRadial> pending_radial;
  std::vector<PendingShells> pending_shells;
  std::vector<PendingContact> all_contacts;
  f64 region_collapse_mass[4] = {0.0, 0.0, 0.0, 0.0};
  u64 ticks_seen = 0;
  u64 totals_seen = 0;
  u64 states_seen = 0;
  u64 bodies_seen = 0;
  u64 collapses_seen = 0;
  bool params_seen = false;
  f64 max_pos_dev = 0.0;
  f64 worst_vn_after = 0.0;
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
        // Collapse mode is selected per cycle by the presence of the
        // section 20 RegionMultipole record (section 19 streams carry
        // none); radial mode per the section 23 RegionRadial record.
        // RegionLevel records queue and apply here, at the tick
        // boundary, in stream order — level 2 collapses validate against
        // the world's frozen totals bit-for-bit.
        world.mp_enabled = !pending_multipole.empty();
        world.radial_enabled = world.radial_enabled || !pending_radial.empty();
        world.shells_enabled = world.shells_enabled || !pending_shells.empty();
        // Section 21: contact mode is sticky from the first Contact record.
        if (!pending_contact.empty()) {
          world.contacts_on = true;
        }
        for (const Pending &p : pending) {
          if (p.level == 0) {
            // Demote on a collapsed region is a no-op (section 19).
            if (world.rstate[p.region] != 2) {
              world.demote(p.region, world.tick + 1);
            }
          } else if (p.level == 1) {
            world.promote(p.region, world.tick + 1);
          } else {
            if (world.rstate[p.region] != 2) {
              world.collapse(p.region, world.tick + 1);
            }
          }
        }
        pending.clear();
        for (const PendingCollapse &pc : pending_collapsed) {
          ++collapses_seen;
          const GCollapsed &c = world.collapsed[pc.region];
          if (!c.active || pc.t != world.tick + 1 || pc.n != c.n || !f64_bits_eq(pc.mass, c.mass) ||
              !f64_bits_eq(pc.com_x, c.com_x) || !f64_bits_eq(pc.com_y, c.com_y) ||
              !f64_bits_eq(pc.pxt, c.pxt) || !f64_bits_eq(pc.pyt, c.pyt) ||
              !f64_bits_eq(pc.energy, c.energy)) {
            std::fprintf(stderr,
                         "MISMATCH: RegionCollapsed tick=%" PRIu64 " region=%d"
                         " stream=(n=%" PRIu64 " mass=%.17g com=(%.17g,%.17g) p=(%.17g,%.17g)"
                         " E=%.17g) computed=(n=%" PRIu64 " mass=%.17g com=(%.17g,%.17g)"
                         " p=(%.17g,%.17g) E=%.17g)\n",
                         pc.t, pc.region, pc.n, pc.mass, pc.com_x, pc.com_y, pc.pxt, pc.pyt,
                         pc.energy, c.n, c.mass, c.com_x, c.com_y, c.pxt, c.pyt, c.energy);
            return 1;
          }
        }
        pending_collapsed.clear();
        for (const PendingMultipole &pm : pending_multipole) {
          const GCollapsed &c = world.collapsed[pm.region];
          if (!c.active || !c.mp || pm.t != world.tick + 1 || !f64_bits_eq(pm.mx, c.mx) ||
              !f64_bits_eq(pm.my, c.my) || !f64_bits_eq(pm.qxx, c.qxx) ||
              !f64_bits_eq(pm.qxy, c.qxy) || !f64_bits_eq(pm.qyy, c.qyy)) {
            std::fprintf(stderr,
                         "MISMATCH: RegionMultipole tick=%" PRIu64 " region=%d"
                         " stream=(m=(%.17g,%.17g) q=(%.17g,%.17g,%.17g))"
                         " computed=(m=(%.17g,%.17g) q=(%.17g,%.17g,%.17g))\n",
                         pm.t, pm.region, pm.mx, pm.my, pm.qxx, pm.qxy, pm.qyy, c.mx, c.my,
                         c.qxx, c.qxy, c.qyy);
            return 1;
          }
        }
        pending_multipole.clear();
        for (const PendingRadial &pr : pending_radial) {
          const GCollapsed &c = world.collapsed[pr.region];
          if (!c.active || !c.radial || pr.t != world.tick + 1 ||
              !f64_bits_eq(pr.binding, c.binding)) {
            std::fprintf(stderr,
                         "MISMATCH: RegionRadial tick=%" PRIu64 " region=%d binding=%.17g"
                         " computed=%.17g\n",
                         pr.t, pr.region, pr.binding, c.binding);
            return 1;
          }
        }
        pending_radial.clear();
        for (const PendingShells &ps : pending_shells) {
          const GCollapsed &c = world.collapsed[ps.region];
          bool ok = c.active && c.shells && ps.t == world.tick + 1 &&
                    f64_bits_eq(ps.binding, c.binding);
          for (int k = 0; ok && k < 4; ++k) {
            ok = f64_bits_eq(ps.b[k], c.shell_b[k]);
          }
          if (!ok) {
            std::fprintf(stderr,
                         "MISMATCH: RegionShells tick=%" PRIu64 " region=%d"
                         " binding=%.17g computed=%.17g b0=%.17g/%.17g\n",
                         ps.t, ps.region, ps.binding, c.binding, ps.b[0], c.shell_b[0]);
            return 1;
          }
        }
        pending_shells.clear();
        world.step();
        reference.step();
        // Section 21: contact records validate against this tick's own
        // pass (the pass runs at the end of the step), in stream order.
        {
          const std::vector<GContact> local = world.last_contacts;
          world.last_contacts.clear();
          if (local.size() != pending_contact.size()) {
            std::fprintf(stderr,
                         "MISMATCH: tick %" PRIu64 " contact count stream=%zu computed=%zu\n",
                         t, pending_contact.size(), local.size());
            return 1;
          }
          for (std::size_t k = 0; k < local.size(); ++k) {
            const PendingContact &pc = pending_contact[k];
            const GContact &lc = local[k];
            if (pc.t != lc.tick || pc.a != lc.a || pc.b != lc.b ||
                !f64_bits_eq(pc.jn, lc.jn) || !f64_bits_eq(pc.cx, lc.cx) ||
                !f64_bits_eq(pc.cy, lc.cy)) {
              std::fprintf(stderr,
                           "MISMATCH: Contact tick=%" PRIu64 " pair=(%" PRIu32 ",%" PRIu32
                           ") stream=(jn=%.17g c=(%.17g,%.17g)) computed=(jn=%.17g"
                           " c=(%.17g,%.17g))\n",
                           pc.t, pc.a, pc.b, pc.jn, pc.cx, pc.cy, lc.jn, lc.cx, lc.cy);
              return 1;
            }
            const f64 residual = lc.vn_after + world.restitution * lc.vn;
            const f64 abs_vn = residual > 0 ? residual : -residual;
            if (abs_vn > worst_vn_after) {
              worst_vn_after = abs_vn;
            }
            all_contacts.push_back(pc);
          }
          pending_contact.clear();
        }
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
        if (rx > 1 || ry > 1 || lv > 2) {
          std::fprintf(stderr,
                       "error: bad RegionLevel (rx=%" PRIu32 " ry=%" PRIu32 " level=%" PRIu8
                       ") at offset %zu\n",
                       rx, ry, lv, rec_start);
          return 2;
        }
        // All levels (including 2/collapse) queue for the next tick
        // boundary, where the collapse's RegionMultipole presence is known.
        pending.push_back({static_cast<int>(ry) * 2 + static_cast<int>(rx), lv});
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
        if (lv > 2 || bid >= world.bodies.size()) {
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
      case 8: {
        // Section 19 RegionCollapsed: parsed here, applied and validated
        // bit-for-bit at the tick boundary (see case 1).
        u64 t = 0;
        u32 rx = 0;
        u32 ry = 0;
        u64 n = 0;
        f64 mass = 0, com_x = 0, com_y = 0, pxt = 0, pyt = 0, energy = 0;
        if (!take_u64(data, off, t) || !take_u32(data, off, rx) || !take_u32(data, off, ry) ||
            !take_u64(data, off, n) || !take_f64(data, off, mass) ||
            !take_f64(data, off, com_x) || !take_f64(data, off, com_y) ||
            !take_f64(data, off, pxt) || !take_f64(data, off, pyt) ||
            !take_f64(data, off, energy)) {
          std::fprintf(stderr, "error: truncated RegionCollapsed at offset %zu\n", rec_start);
          return 2;
        }
        if (rx > 1 || ry > 1) {
          std::fprintf(stderr,
                       "error: bad RegionCollapsed region (%" PRIu32 ",%" PRIu32
                       ") at offset %zu\n",
                       rx, ry, rec_start);
          return 2;
        }
        region_collapse_mass[ry * 2 + rx] = mass;
        pending_collapsed.push_back(
            {t, static_cast<int>(ry) * 2 + static_cast<int>(rx), n, mass, com_x, com_y, pxt, pyt,
             energy});
      } break;
      case 9: {
        // Section 20 RegionMultipole: parsed here, validated bit-for-bit at
        // the tick boundary.
        u64 t = 0;
        u32 rx = 0;
        u32 ry = 0;
        f64 mx = 0, my = 0, qxx = 0, qxy = 0, qyy = 0;
        if (!take_u64(data, off, t) || !take_u32(data, off, rx) || !take_u32(data, off, ry) ||
            !take_f64(data, off, mx) || !take_f64(data, off, my) ||
            !take_f64(data, off, qxx) || !take_f64(data, off, qxy) ||
            !take_f64(data, off, qyy)) {
          std::fprintf(stderr, "error: truncated RegionMultipole at offset %zu\n", rec_start);
          return 2;
        }
        if (rx > 1 || ry > 1) {
          std::fprintf(stderr,
                       "error: bad RegionMultipole region (%" PRIu32 ",%" PRIu32
                       ") at offset %zu\n",
                       rx, ry, rec_start);
          return 2;
        }
        pending_multipole.push_back(
            {t, static_cast<int>(ry) * 2 + static_cast<int>(rx), mx, my, qxx, qxy, qyy});
      } break;
      case 10: {
        // Section 21 Contact: parsed here, validated bit-for-bit at the tick
        // boundary (see case 1).
        u64 t = 0;
        u32 a = 0;
        u32 b = 0;
        f64 jn = 0, cx = 0, cy = 0;
        if (!take_u64(data, off, t) || !take_u32(data, off, a) || !take_u32(data, off, b) ||
            !take_f64(data, off, jn) || !take_f64(data, off, cx) || !take_f64(data, off, cy)) {
          std::fprintf(stderr, "error: truncated Contact at offset %zu\n", rec_start);
          return 2;
        }
        const bool b_static = b >= C_MONOPOLE_BASE;
        if (a >= b || (!b_static && b >= body_count) || t == 0) {
          std::fprintf(stderr,
                       "error: bad Contact (tick=%" PRIu64 " pair=(%" PRIu32 ",%" PRIu32
                       ")) at offset %zu\n",
                       t, a, b, rec_start);
          return 2;
        }
        pending_contact.push_back({t, a, b, jn, cx, cy});
      } break;
      case 11: {
        // Section 23 RegionRadial: parsed here, validated bit-for-bit at the
        // tick boundary.
        u64 t = 0;
        u32 rx = 0;
        u32 ry = 0;
        f64 binding = 0;
        if (!take_u64(data, off, t) || !take_u32(data, off, rx) || !take_u32(data, off, ry) ||
            !take_f64(data, off, binding)) {
          std::fprintf(stderr, "error: truncated RegionRadial at offset %zu\n", rec_start);
          return 2;
        }
        if (rx > 1 || ry > 1) {
          std::fprintf(stderr,
                       "error: bad RegionRadial region (%" PRIu32 ",%" PRIu32 ") at offset %zu\n",
                       rx, ry, rec_start);
          return 2;
        }
        pending_radial.push_back({t, static_cast<int>(ry) * 2 + static_cast<int>(rx), binding});
      } break;
      case 13: {
        // Section 25 RegionShells: parsed here, validated bit-for-bit at
        // the tick boundary.
        u64 t = 0;
        u32 rx = 0;
        u32 ry = 0;
        f64 binding = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0;
        if (!take_u64(data, off, t) || !take_u32(data, off, rx) || !take_u32(data, off, ry) ||
            !take_f64(data, off, binding) || !take_f64(data, off, b0) ||
            !take_f64(data, off, b1) || !take_f64(data, off, b2) ||
            !take_f64(data, off, b3)) {
          std::fprintf(stderr, "error: truncated RegionShells at offset %zu\n", rec_start);
          return 2;
        }
        if (rx > 1 || ry > 1) {
          std::fprintf(stderr,
                       "error: bad RegionShells region (%" PRIu32 ",%" PRIu32 ") at offset %zu\n",
                       rx, ry, rec_start);
          return 2;
        }
        PendingShells ps;
        ps.t = t;
        ps.region = static_cast<int>(ry) * 2 + static_cast<int>(rx);
        ps.binding = binding;
        ps.b[0] = b0;
        ps.b[1] = b1;
        ps.b[2] = b2;
        ps.b[3] = b3;
        pending_shells.push_back(ps);
      } break;
      case 12: {
        // Section 24 ContactParams: at most one, before the first tick.
        f64 restitution = 0;
        f64 friction = 0;
        if (!take_f64(data, off, restitution) || !take_f64(data, off, friction) ||
            data.size() - off < 1) {
          std::fprintf(stderr, "error: truncated ContactParams at offset %zu\n", rec_start);
          return 2;
        }
        const u8 walls = data[off++];
        if (params_seen || last_tick != 0 || walls > 1 || !(restitution >= 0.0 && restitution <= 1.0) ||
            friction < 0.0) {
          std::fprintf(stderr,
                       "error: bad ContactParams (e=%.17g friction=%.17g walls=%" PRIu8
                       ") at offset %zu\n",
                       restitution, friction, walls, rec_start);
          return 2;
        }
        params_seen = true;
        world.contacts_params = true;
        world.restitution = restitution;
        world.friction = friction;
        world.walls_on = walls == 1;
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
  // Spec section 22: modal audio as a pure function of the (verified)
  // stream — contact records excite damped resonators; the whole path
  // stays in the +,-,*,/ closure so it is bit-identical with the Rust
  // and Python implementations.
  static const u64 A_SPT = 64;
  static const int A_RING = 16384;
  static const u64 A_TAIL = 260;
  static const f64 A_OMEGA0 = 0.0004448824124529259;
  static const f64 A_PARTIAL[3] = {1.0, 4.0, 9.0};
  static const f64 A_RHO[3] = {0.9990, 0.9985, 0.9980};
  static const f64 A_AMP[3] = {0.5, 0.3, 0.2};
  const std::size_t n_samples =
      static_cast<std::size_t>((last_tick + A_TAIL) * A_SPT);
  std::vector<f64> abuf(n_samples, 0.0);
  for (const PendingContact &pc : all_contacts) {
    const std::size_t e = static_cast<std::size_t>((pc.t + 1) * A_SPT);
    const f64 ma = world.bodies[pc.a].mass;
    f64 mu;
    if (pc.b >= C_WALL_BASE) {
      mu = ma;
    } else if (pc.b >= C_MONOPOLE_BASE) {
      const f64 m = region_collapse_mass[pc.b - C_MONOPOLE_BASE];
      mu = (ma * m) / (ma + m);
    } else {
      const f64 mb = world.bodies[pc.b].mass;
      mu = (ma * mb) / (ma + mb);
    }
    for (int k = 0; k < 3; ++k) {
      const f64 omega = A_OMEGA0 * A_PARTIAL[k] / mu;
      const f64 a = (2.0 - omega) * A_RHO[k];
      const f64 b = A_RHO[k] * A_RHO[k];
      const f64 s0 = A_AMP[k] * pc.jn;
      f64 s_prev = s0;
      f64 s_prev2 = 0.0;
      for (int i = 0; i < A_RING; ++i) {
        const f64 s = i == 0 ? s0 : (i == 1 ? a * s0 : a * s_prev - b * s_prev2);
        abuf[e + static_cast<std::size_t>(i)] += s;
        s_prev2 = s_prev;
        s_prev = s;
      }
    }
  }
  std::vector<u8> pcm(n_samples * 2);
  for (std::size_t i = 0; i < n_samples; ++i) {
    f64 v = abuf[i];
    if (v < -1.0) {
      v = -1.0;
    } else if (v > 1.0) {
      v = 1.0;
    }
    const f64 q = std::floor(v * 32767.0 + 0.5);
    short sample = static_cast<short>(q);
    pcm[i * 2] = static_cast<u8>(static_cast<u16>(sample) & 0xff);
    pcm[i * 2 + 1] = static_cast<u8>((static_cast<u16>(sample) >> 8) & 0xff);
  }
  const u64 audio_digest = fnv1a64(pcm.data(), pcm.size());
  if (wav_out != nullptr) {
    std::FILE *wf = std::fopen(wav_out, "wb");
    if (!wf) {
      std::fprintf(stderr, "error: cannot open '%s' for writing\n", wav_out);
      return 3;
    }
    const u32 data_size = static_cast<u32>(pcm.size());
    u8 header[44];
    std::memcpy(header, "RIFF", 4);
    put_u32le(header + 4, 36 + data_size);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    put_u32le(header + 16, 16);
    put_u16le(header + 20, 1);   // PCM
    put_u16le(header + 22, 1);   // mono
    put_u32le(header + 24, 65536);
    put_u32le(header + 28, 131072);
    put_u16le(header + 32, 2);   // block align
    put_u16le(header + 34, 16);  // bits per sample
    std::memcpy(header + 36, "data", 4);
    put_u32le(header + 40, data_size);
    std::fwrite(header, 1, sizeof header, wf);
    std::fwrite(pcm.data(), 1, pcm.size(), wf);
    std::fclose(wf);
  }
  std::printf(
      "OK: ticks=%" PRIu64 " totals=%" PRIu64 " region_states=%" PRIu64 " bodies=%" PRIu64
      " collapses=%" PRIu64 " contacts=%zu fine=%" PRIu64 " coarse=%" PRIu64
      " final_world_hash=%016" PRIx64 " audio=%016" PRIx64
      " max_pos_dev=%.3e mom_drift=%.3e energy_drift=%.3e worst_post_vn=%.3e\n",
      ticks_seen, totals_seen, states_seen, bodies_seen, collapses_seen, all_contacts.size(),
      w_fine, w_cn, world.world_hash(), audio_digest, max_pos_dev, mom_drift, e_drift,
      worst_vn_after);
  return 0;
}

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  if (!fnv_self_check()) {
    std::fprintf(stderr, "error: FNV-1a64 self-check failed\n");
    return 3;
  }
  const char *wav_out = nullptr;
  const char *ic_profile = nullptr;
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: ontos_stream_dump <stream-file> <seed> [--wav out.wav] "
                 "[--test-ic wallshot|coarsehit]\n");
    return 3;
  }
  for (int a = 3; a < argc; ++a) {
    if (std::strcmp(argv[a], "--wav") == 0 && a + 1 < argc) {
      wav_out = argv[a + 1];
      ++a;
    } else if (std::strcmp(argv[a], "--test-ic") == 0 && a + 1 < argc) {
      ic_profile = argv[a + 1];
      ++a;
    } else {
      std::fprintf(stderr,
                   "usage: ontos_stream_dump <stream-file> <seed> [--wav out.wav] "
                   "[--test-ic wallshot|coarsehit]\n");
      return 3;
    }
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
    return run_gravity(data, seed, body_count, wav_out, ic_profile);
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
