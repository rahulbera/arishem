// trace_cutter.cpp
//
// Splits a zstd-compressed v2 ChampSim trace into N-instruction chunks.
// Each input record is a fixed 512-byte trace_instr_v2_t. Each output chunk
// is itself a self-contained .zst file holding up to N records; the final
// chunk may be shorter.
//
// Pipeline: mmap(input) -> ZSTD_decompressStream -> reassemble 512-byte
// records (with carry across decompress boundaries) -> per-chunk
// ZSTD_compressStream2 -> fwrite. Chunks share one CCtx (reset between
// frames) and one CStreamOut buffer.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zstd.h>

namespace fs = std::filesystem;

static constexpr size_t   RECORD_SIZE        = 512;
static constexpr uint64_t HEARTBEAT_RECORDS  = 5'000'000;
static constexpr size_t   IO_WRITE_BUF_BYTES = 1 << 20;

enum { OPT_DRY_RUN = 1000 };

struct Args {
  std::string input;
  std::string outdir;
  uint64_t    chunk_records = 0;
  int         level         = 3;
  int         workers       = 0;
  bool        dry_run       = false;
};

static void usage(const char* prog) {
  std::fprintf(stderr,
    "Usage: %s -i <input.zst> -o <outdir> -n <records-per-chunk>\n"
    "                   [-l <level>] [-w <workers>] [--dry-run]\n"
    "\n"
    "  -i, --input        input zstd-compressed v2 trace file (required)\n"
    "  -o, --output-dir   output directory; created if missing (required)\n"
    "  -n, --num-instr    records per chunk (required, >0)\n"
    "  -l, --level        zstd compression level for outputs (default 3)\n"
    "  -w, --workers      zstd encoder worker threads (default 0 = single)\n"
    "      --dry-run      count records and report chunks; write nothing\n"
    "  -h, --help         show this help\n",
    prog);
}

static bool parse_args(int argc, char** argv, Args& a) {
  static const option longopts[] = {
    {"input",      required_argument, nullptr, 'i'},
    {"output-dir", required_argument, nullptr, 'o'},
    {"num-instr",  required_argument, nullptr, 'n'},
    {"level",      required_argument, nullptr, 'l'},
    {"workers",    required_argument, nullptr, 'w'},
    {"dry-run",    no_argument,       nullptr, OPT_DRY_RUN},
    {"help",       no_argument,       nullptr, 'h'},
    {nullptr, 0, nullptr, 0}
  };
  int c;
  while ((c = getopt_long(argc, argv, "i:o:n:l:w:h", longopts, nullptr)) != -1) {
    switch (c) {
      case 'i': a.input         = optarg; break;
      case 'o': a.outdir        = optarg; break;
      case 'n': a.chunk_records = std::strtoull(optarg, nullptr, 10); break;
      case 'l': a.level         = std::atoi(optarg); break;
      case 'w': a.workers       = std::atoi(optarg); break;
      case OPT_DRY_RUN: a.dry_run = true; break;
      case 'h': usage(argv[0]); std::exit(0);
      default:  usage(argv[0]); return false;
    }
  }
  if (a.input.empty() || a.outdir.empty() || a.chunk_records == 0) {
    std::fprintf(stderr, "error: --input, --output-dir, --num-instr are required\n\n");
    usage(argv[0]);
    return false;
  }
  return true;
}

// Build "<outdir>/<input-stem-without-.zst>_partNNN.zst".
static std::string output_path(const Args& a, uint32_t part_idx) {
  std::string stem = fs::path(a.input).filename().string();
  static const std::string ext = ".zst";
  if (stem.size() > ext.size() &&
      stem.compare(stem.size() - ext.size(), ext.size(), ext) == 0) {
    stem.resize(stem.size() - ext.size());
  }
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), "_part%03u.zst", part_idx);
  return (fs::path(a.outdir) / (stem + suffix)).string();
}

struct ChunkWriter {
  FILE*             fp   = nullptr;
  ZSTD_CCtx*        cctx = nullptr;
  std::vector<char> out_buf;
  std::vector<char> io_buf;   // backing for setvbuf
  std::string       path;
  uint64_t          bytes_out = 0;

  void init_once(int level, int workers) {
    cctx = ZSTD_createCCtx();
    if (!cctx) { std::fprintf(stderr, "error: ZSTD_createCCtx failed\n"); std::exit(1); }
    out_buf.resize(ZSTD_CStreamOutSize());
    io_buf.resize(IO_WRITE_BUF_BYTES);
    set_params(level, workers);
  }

  void set_params(int level, int workers) {
    size_t rc = ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
    if (ZSTD_isError(rc)) {
      std::fprintf(stderr, "error: set compression level: %s\n", ZSTD_getErrorName(rc));
      std::exit(1);
    }
    if (workers > 0) {
      rc = ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, workers);
      if (ZSTD_isError(rc)) {
        std::fprintf(stderr, "warning: set nbWorkers=%d failed: %s (continuing single-threaded)\n",
                     workers, ZSTD_getErrorName(rc));
      }
    }
  }

  void open_chunk(const std::string& p) {
    path = p;
    fp = std::fopen(p.c_str(), "wb");
    if (!fp) {
      std::fprintf(stderr, "error: cannot open %s for writing: %s\n",
                   p.c_str(), std::strerror(errno));
      std::exit(1);
    }
    std::setvbuf(fp, io_buf.data(), _IOFBF, io_buf.size());
    ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
    bytes_out = 0;
  }

  void write_records(const char* src, size_t nbytes) {
    ZSTD_inBuffer in = { src, nbytes, 0 };
    while (in.pos < in.size) {
      ZSTD_outBuffer out = { out_buf.data(), out_buf.size(), 0 };
      size_t rc = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_continue);
      if (ZSTD_isError(rc)) {
        std::fprintf(stderr, "error: ZSTD_compressStream2: %s\n", ZSTD_getErrorName(rc));
        std::exit(1);
      }
      if (out.pos > 0) {
        if (std::fwrite(out_buf.data(), 1, out.pos, fp) != out.pos) {
          std::fprintf(stderr, "error: short write to %s\n", path.c_str());
          std::exit(1);
        }
        bytes_out += out.pos;
      }
    }
  }

  void close_chunk() {
    ZSTD_inBuffer empty = { nullptr, 0, 0 };
    size_t rem;
    do {
      ZSTD_outBuffer out = { out_buf.data(), out_buf.size(), 0 };
      rem = ZSTD_compressStream2(cctx, &out, &empty, ZSTD_e_end);
      if (ZSTD_isError(rem)) {
        std::fprintf(stderr, "error: ZSTD_compressStream2(end): %s\n", ZSTD_getErrorName(rem));
        std::exit(1);
      }
      if (out.pos > 0) {
        std::fwrite(out_buf.data(), 1, out.pos, fp);
        bytes_out += out.pos;
      }
    } while (rem > 0);
    std::fclose(fp);
    fp = nullptr;
  }

  ~ChunkWriter() { if (cctx) ZSTD_freeCCtx(cctx); }
};

static double secs_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;

  // Open + mmap input.
  int fd = ::open(a.input.c_str(), O_RDONLY);
  if (fd < 0) {
    std::fprintf(stderr, "error: open(%s): %s\n", a.input.c_str(), std::strerror(errno));
    return 1;
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    std::fprintf(stderr, "error: fstat: %s\n", std::strerror(errno));
    return 1;
  }
  size_t in_size = static_cast<size_t>(st.st_size);
  if (in_size == 0) {
    std::fprintf(stderr, "error: input file is empty\n");
    return 1;
  }
  void* in_map = ::mmap(nullptr, in_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (in_map == MAP_FAILED) {
    std::fprintf(stderr, "error: mmap: %s\n", std::strerror(errno));
    return 1;
  }
  ::close(fd);
  ::madvise(in_map, in_size, MADV_SEQUENTIAL);
  ::madvise(in_map, in_size, MADV_WILLNEED);

  // Output directory.
  if (!a.dry_run) {
    std::error_code ec;
    fs::create_directories(a.outdir, ec);
    if (ec && !fs::is_directory(a.outdir)) {
      std::fprintf(stderr, "error: cannot create output dir %s: %s\n",
                   a.outdir.c_str(), ec.message().c_str());
      return 1;
    }
  }

  // Decompression context + buffer.
  ZSTD_DCtx* dctx = ZSTD_createDCtx();
  if (!dctx) { std::fprintf(stderr, "error: ZSTD_createDCtx failed\n"); return 1; }
  std::vector<char> dec_buf(std::max<size_t>(ZSTD_DStreamOutSize(), 1 << 20));

  // Carry buffer for partial records across decompress boundaries.
  char   carry[RECORD_SIZE];
  size_t carry_len = 0;

  ChunkWriter writer;
  if (!a.dry_run) writer.init_once(a.level, a.workers);

  // State
  uint64_t                 total_records   = 0;
  uint64_t                 chunk_records   = 0;
  uint32_t                 part_idx        = 0;
  bool                     writer_open     = false;
  uint64_t                 next_heartbeat  = HEARTBEAT_RECORDS;
  std::vector<uint64_t>    chunk_records_list;
  std::vector<uint64_t>    chunk_bytes_list;
  auto                     t_start         = std::chrono::steady_clock::now();

  auto ensure_writer_open = [&]() {
    if (writer_open) return;
    if (!a.dry_run) writer.open_chunk(output_path(a, part_idx));
    writer_open = true;
  };

  auto finalize_current_chunk = [&]() {
    if (!writer_open) return;
    uint64_t bytes_out = 0;
    std::string p;
    if (!a.dry_run) {
      writer.close_chunk();
      bytes_out = writer.bytes_out;
      p = writer.path;
    }
    chunk_records_list.push_back(chunk_records);
    chunk_bytes_list.push_back(bytes_out);
    if (a.dry_run) {
      std::fprintf(stderr, "[chunk %03u] records=%lu (dry-run)\n",
                   part_idx, (unsigned long)chunk_records);
    } else {
      std::fprintf(stderr, "[chunk %03u] records=%lu compressed=%lu bytes -> %s\n",
                   part_idx, (unsigned long)chunk_records,
                   (unsigned long)bytes_out, p.c_str());
    }
    ++part_idx;
    chunk_records = 0;
    writer_open   = false;
  };

  auto maybe_heartbeat = [&]() {
    while (total_records >= next_heartbeat) {
      double s = secs_since(t_start);
      std::fprintf(stderr, "[heartbeat] processed %lu records  %.1f Mrec/s  elapsed %.1fs\n",
                   (unsigned long)total_records,
                   s > 0 ? (double)total_records / s / 1e6 : 0.0, s);
      next_heartbeat += HEARTBEAT_RECORDS;
    }
  };

  // Hand a run of complete records (multiple of RECORD_SIZE) to the chunker.
  auto emit_records = [&](const char* p, size_t nrec) {
    size_t off = 0;
    while (nrec > 0) {
      ensure_writer_open();
      uint64_t room = a.chunk_records - chunk_records;
      uint64_t take = std::min<uint64_t>(nrec, room);
      size_t   nb   = static_cast<size_t>(take) * RECORD_SIZE;
      if (!a.dry_run) writer.write_records(p + off, nb);
      chunk_records += take;
      total_records += take;
      off           += nb;
      nrec          -= take;
      if (chunk_records == a.chunk_records) finalize_current_chunk();
      maybe_heartbeat();
    }
  };

  // Consume a freshly decompressed buffer: drain carry, emit full-record run,
  // save trailing partial record into carry.
  auto consume_decompressed = [&](const char* dp, size_t len) {
    size_t off = 0;
    if (carry_len > 0) {
      size_t need = RECORD_SIZE - carry_len;
      if (len < need) {
        std::memcpy(carry + carry_len, dp, len);
        carry_len += len;
        return;
      }
      std::memcpy(carry + carry_len, dp, need);
      carry_len = 0;
      off       = need;
      emit_records(carry, 1);
    }
    size_t avail = len - off;
    size_t whole = avail / RECORD_SIZE;
    if (whole > 0) {
      emit_records(dp + off, whole);
      off += whole * RECORD_SIZE;
    }
    size_t tail = len - off;
    if (tail > 0) {
      std::memcpy(carry, dp + off, tail);
      carry_len = tail;
    }
  };

  // Decompression loop. Concatenated zstd frames are handled implicitly:
  // ZSTD_decompressStream returns 0 on frame boundary and resumes on the
  // next call as long as zin.pos < zin.size.
  ZSTD_inBuffer zin = { in_map, in_size, 0 };
  while (zin.pos < zin.size) {
    ZSTD_outBuffer dout = { dec_buf.data(), dec_buf.size(), 0 };
    size_t rc = ZSTD_decompressStream(dctx, &dout, &zin);
    if (ZSTD_isError(rc)) {
      std::fprintf(stderr, "error: ZSTD_decompressStream: %s\n", ZSTD_getErrorName(rc));
      return 1;
    }
    if (dout.pos > 0) consume_decompressed((const char*)dec_buf.data(), dout.pos);
  }

  finalize_current_chunk();

  ZSTD_freeDCtx(dctx);
  ::munmap(in_map, in_size);

  // Final summary.
  double secs = secs_since(t_start);
  uint64_t total_compressed = 0;
  for (auto v : chunk_bytes_list) total_compressed += v;

  std::fprintf(stderr, "\n==== trace_cutter summary ====\n");
  std::fprintf(stderr, "input:                  %s\n", a.input.c_str());
  std::fprintf(stderr, "input compressed size:  %zu bytes\n", in_size);
  std::fprintf(stderr, "total records:          %lu\n", (unsigned long)total_records);
  std::fprintf(stderr, "total uncompressed:     %lu bytes\n",
               (unsigned long)(total_records * RECORD_SIZE));
  std::fprintf(stderr, "chunks emitted:         %u\n", (unsigned)chunk_records_list.size());
  std::fprintf(stderr, "chunk size (target):    %lu records\n",
               (unsigned long)a.chunk_records);
  if (!chunk_records_list.empty()) {
    std::fprintf(stderr, "last chunk records:     %lu\n",
                 (unsigned long)chunk_records_list.back());
  }
  if (!a.dry_run) {
    std::fprintf(stderr, "total output bytes:     %lu\n", (unsigned long)total_compressed);
    if (total_records > 0) {
      double ratio = (double)(total_records * RECORD_SIZE) / (double)total_compressed;
      std::fprintf(stderr, "output compression:     %.2fx (uncompressed/compressed)\n", ratio);
    }
  }
  if (carry_len > 0) {
    std::fprintf(stderr, "warning: %zu trailing bytes did not form a complete record (discarded)\n",
                 carry_len);
  }
  std::fprintf(stderr, "elapsed:                %.2f s\n", secs);
  if (secs > 0 && total_records > 0) {
    double mrps = (double)total_records / secs / 1e6;
    double mbps = (double)(total_records * RECORD_SIZE) / secs / (1024.0 * 1024.0);
    std::fprintf(stderr, "throughput:             %.2f Mrec/s  %.1f MiB/s (uncompressed)\n",
                 mrps, mbps);
  }
  std::fprintf(stderr, "level=%d  workers=%d  dry_run=%s\n",
               a.level, a.workers, a.dry_run ? "yes" : "no");

  return 0;
}
