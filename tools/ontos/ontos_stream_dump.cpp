#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

static const u64 FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
static const u64 FNV_PRIME = 0x100000001b3ULL;
static const int WORLD = 128;
static const int REGION_FINE = 64;
static const int COARSE_N = 32;

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

  static void put_u64le(u8 *dst, u64 v) {
    for (int i = 0; i < 8; ++i) dst[i] = static_cast<u8>(v >> (8 * i));
  }

  static void put_u32le(u8 *dst, u32 v) {
    for (int i = 0; i < 4; ++i) dst[i] = static_cast<u8>(v >> (8 * i));
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
  if (version != 1) {
    std::fprintf(stderr, "error: unsupported stream version %" PRIu32 "\n", version);
    return 2;
  }
  if (world_w != WORLD || world_h != WORLD) {
    std::fprintf(stderr, "error: unsupported world size %" PRIu32 "x%" PRIu32 "\n", world_w,
                 world_h);
    return 2;
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
