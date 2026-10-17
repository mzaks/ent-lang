// What a program built for a GPU (`ent build --gpu`, docs/gpu.md) runs
// on, over NVIDIA's driver (libcuda): the functions MLIR's lowering of
// kernel launches calls (mgpu*), the GPU's copy of the world, and the
// copies between the two that the compiler asks for.
//
// The GPU's copy has the CPU's layout, at another address: a kernel
// addresses its columns as the CPU does. Of what both keep (a piece: a
// column, a buffer's field or count) this knows which has it newer; the
// compiler says before a run on either side what it touches
// (ent_gpu_to_device, ent_gpu_to_host: copied where the other side has it
// newer) and after it what it wrote (ent_gpu_wrote). Kernels are given one
// after another to one stream and not waited for: the CPU waits where it
// copies from the GPU, and when a schedule ends (ent_gpu_wait).
//
// A C host that reads or writes the world between schedule calls calls
// ent_gpu_world_to_host first (declared by the header with ENT_GPU).
//
// For finding out how it goes, a program may be run with: ENT_GPU_WHOLE
// (what comes back comes as whole ranges, no shadows), ENT_GPU_REPORT
// (at its end, how much came back whole and how many words packed),
// ENT_GPU_CHECK (each packed copy checked against the whole range) and
// ENT_GPU_LOG (the first copies, a line each).

#include <cuda.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CUcontext context;
static CUstream stream;
// Whether kernels or copies were given to the stream since the last wait.
static bool pending;

static void check(CUresult result, const char *what) {
  if (result == CUDA_SUCCESS)
    return;
  const char *name = "?";
  cuGetErrorName(result, &name);
  fprintf(stderr, "ent: the GPU: %s: %s\n", what, name);
  exit(3);
}

static void start(void) {
  if (context)
    return;
  check(cuInit(0), "cuInit");
  CUdevice device;
  check(cuDeviceGet(&device, 0), "cuDeviceGet");
  check(cuDevicePrimaryCtxRetain(&context, device), "cuDevicePrimaryCtxRetain");
  check(cuCtxSetCurrent(context), "cuCtxSetCurrent");
  check(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
}

void ent_gpu_wait(void) {
  if (!pending)
    return;
  // (An error of a kernel shows here, where the CPU first waits for it.)
  check(cuStreamSynchronize(stream), "a kernel");
  pending = false;
}

// The world, as the program hands memrefs over (llvm.emit_c_interface):
// a byte array of one dimension.
typedef struct {
  char *allocated;
  char *aligned;
  int64_t offset;
  int64_t size;
  int64_t stride;
} ent_gpu_memref;

static char *host;
static CUdeviceptr device;
static int64_t bytes;

// The GPU's copy of the world: made when the program first asks for it.
void _mlir_ciface_ent_gpu_arena(ent_gpu_memref *copy, ent_gpu_memref *world) {
  if (!device) {
    start();
    host = world->aligned;
    bytes = world->size;
    check(cuMemAlloc(&device, (size_t)bytes), "cuMemAlloc (the world)");
  } else if (world->aligned != host) {
    fprintf(stderr, "ent: the GPU: a second world (one is all it keeps)\n");
    exit(3);
  }
  *copy = *world;
  copy->allocated = copy->aligned = (char *)(uintptr_t)device;
}

// Of each piece, the bytes (from its start) that the CPU has newer, and
// those the GPU has: at most one range each, never overlapping. A piece
// not seen yet is the CPU's, all of it: what makes the world runs there.
//
// A piece of which the CPU takes a lot back has a shadow in the GPU's
// memory: what the CPU holds of it, but for the bytes the CPU wrote and
// has not given the GPU yet (they are the CPU's newer ones, which no copy
// back touches, and giving them makes the shadow theirs too). Then what
// the GPU has newer comes back as the 32-bit words that differ from the
// shadow, packed by a kernel (ent_gpu_diff), and not as the whole range:
// a `for` that changes a few entities of many, filtered or not, sends
// back those. (Without the shadow every row in use came back.)
typedef struct {
  bool seen;
  int64_t offset;
  int64_t bytes;
  int64_t cpuFrom, cpuTo;
  int64_t gpuFrom, gpuTo;
  // (The shadow of byte `offset + k` of the world is at `shadow + k +
  // offset % 16`, so that a word is as aligned there as in the world.)
  CUdeviceptr shadow;
  int64_t shadowBytes;
} ent_gpu_piece;
static ent_gpu_piece *pieces;
static int64_t pieceCount;

static ent_gpu_piece *piece(int64_t id, int64_t offset) {
  if (id >= pieceCount) {
    int64_t count = id + 16;
    pieces = realloc(pieces, (size_t)count * sizeof *pieces);
    if (!pieces) {
      fprintf(stderr, "ent: the GPU: out of memory\n");
      exit(3);
    }
    memset(pieces + pieceCount, 0,
           (size_t)(count - pieceCount) * sizeof *pieces);
    pieceCount = count;
  }
  ent_gpu_piece *held = &pieces[id];
  if (!held->seen) {
    held->seen = true;
    held->cpuFrom = 0;
    held->cpuTo = INT64_MAX;
  }
  held->offset = offset;
  return held;
}

static int64_t least(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t most(int64_t a, int64_t b) { return a > b ? a : b; }

// [from, to) less [cut, end), where that leaves one range.
static void without(int64_t *from, int64_t *to, int64_t cut, int64_t end) {
  if (end <= *from || *to <= cut)
    return;
  if (cut <= *from)
    *from = least(end, *to);
  else
    *to = cut;
  if (*from >= *to)
    *from = *to = 0;
}

// What came back, for a program run with ENT_GPU_REPORT set: the bytes
// copied as they were, and the words packed (of how many looked at).
static int64_t wholeBytes, packedWords, lookedWords, outgrown;
static void report(void) {
  fprintf(stderr, "gpu to host: %lld bytes whole, %lld words packed of %lld\n",
          (long long)wholeBytes, (long long)packedWords,
          (long long)lookedWords);
  if (outgrown)
    fprintf(stderr, "gpu shadows outgrown: %lld\n", (long long)outgrown);
}

// (ENT_GPU_LOG: the first copies, one line each, for finding out what
// went where.)
static void logged(const char *what, int64_t id, int64_t offset, int64_t from,
                   int64_t to, int64_t more) {
  static int on = -1, lines;
  if (on < 0)
    on = getenv("ENT_GPU_LOG") != NULL;
  if (!on || lines >= 3000)
    return;
  ++lines;
  fprintf(stderr, "gpu %s piece %lld at %lld [%lld, %lld) %lld\n", what,
          (long long)id, (long long)offset, (long long)from, (long long)to,
          (long long)more);
}

// Where the shadow keeps byte `k` of the piece.
static CUdeviceptr shadowOf(ent_gpu_piece *held, int64_t k) {
  return held->shadow + (CUdeviceptr)(k + held->offset % 16);
}

// The shadow of bytes [from, to) of the piece: what the GPU has of them
// (which the CPU has too, as they were just copied one way or the other).
static void shadowCopies(ent_gpu_piece *held, int64_t from, int64_t to) {
  if (!held->shadow || from >= to)
    return;
  // (A piece that outgrew its shadow, a buffer longer than it was, gives
  // it up: the next copy back that wants one makes a bigger one.)
  if (to + held->offset % 16 > held->shadowBytes) {
    check(cuMemFreeAsync(held->shadow, stream), "cuMemFreeAsync");
    held->shadow = 0;
    held->shadowBytes = 0;
    pending = true;
    ++outgrown;
    return;
  }
  check(cuMemcpyDtoDAsync(shadowOf(held, from),
                          device + (CUdeviceptr)(held->offset + from),
                          (size_t)(to - from), stream),
        "a shadow's copy");
  pending = true;
}

void ent_gpu_to_device(int64_t id, int64_t offset, int64_t size) {
  ent_gpu_piece *held = piece(id, offset);
  held->bytes = size;
  int64_t from = held->cpuFrom, to = least(held->cpuTo, size);
  logged("to_device", id, offset, from, to, size);
  held->cpuFrom = held->cpuTo = 0;
  if (from >= to)
    return;
  // (From the CPU's memory as it is: the call returns when it has it.)
  check(cuMemcpyHtoDAsync(device + (CUdeviceptr)(offset + from),
                          host + offset + from, (size_t)(to - from), stream),
        "a copy to the GPU");
  pending = true;
  shadowCopies(held, from, to);
}

// Bytes [from, to) of the piece to the CPU, as they are.
static void plainToHost(ent_gpu_piece *held, int64_t from, int64_t to) {
  if (from >= to)
    return;
  static int asked;
  if (!asked) {
    asked = 1;
    if (getenv("ENT_GPU_REPORT"))
      atexit(report);
  }
  wholeBytes += to - from;
  check(cuMemcpyDtoHAsync(host + held->offset + from,
                          device + (CUdeviceptr)(held->offset + from),
                          (size_t)(to - from), stream),
        "a copy from the GPU");
  pending = true;
}

// The kernel that packs what differs from a shadow: of `words` 32-bit
// words at `cur`, each that differs from the one at `shadow` becomes the
// shadow's, and (while there is room, `cap` of them) a pair of its index
// and its bits in `pack`; `count` counts them all. (PTX, which the driver
// compiles for the GPU at hand.)
static const char diffPtx[] =
    ".version 7.0\n"
    ".target sm_52\n"
    ".address_size 64\n"
    ".visible .entry ent_gpu_diff(.param .u64 p_cur, .param .u64 p_shadow,\n"
    "    .param .u64 p_words, .param .u64 p_pack, .param .u64 p_cap,\n"
    "    .param .u64 p_count)\n"
    "{\n"
    "  .reg .pred %p<4>;\n"
    "  .reg .b32 %r<6>;\n"
    "  .reg .b64 %rd<16>;\n"
    "  ld.param.u64 %rd1, [p_cur];\n"
    "  ld.param.u64 %rd2, [p_shadow];\n"
    "  ld.param.u64 %rd3, [p_words];\n"
    "  ld.param.u64 %rd4, [p_pack];\n"
    "  ld.param.u64 %rd5, [p_cap];\n"
    "  ld.param.u64 %rd6, [p_count];\n"
    "  mov.u32 %r1, %ctaid.x;\n"
    "  mov.u32 %r2, %ntid.x;\n"
    "  mov.u32 %r3, %tid.x;\n"
    "  mul.wide.u32 %rd7, %r1, %r2;\n"
    "  cvt.u64.u32 %rd8, %r3;\n"
    "  add.u64 %rd7, %rd7, %rd8;\n"
    "  setp.ge.u64 %p1, %rd7, %rd3;\n"
    "  @%p1 bra DONE;\n"
    "  shl.b64 %rd9, %rd7, 2;\n"
    "  add.u64 %rd10, %rd1, %rd9;\n"
    "  add.u64 %rd11, %rd2, %rd9;\n"
    "  ld.global.u32 %r4, [%rd10];\n"
    "  ld.global.u32 %r5, [%rd11];\n"
    "  setp.eq.u32 %p2, %r4, %r5;\n"
    "  @%p2 bra DONE;\n"
    "  st.global.u32 [%rd11], %r4;\n"
    "  atom.global.add.u64 %rd12, [%rd6], 1;\n"
    "  setp.ge.u64 %p3, %rd12, %rd5;\n"
    "  @%p3 bra DONE;\n"
    "  shl.b64 %rd13, %rd12, 3;\n"
    "  add.u64 %rd13, %rd4, %rd13;\n"
    "  cvt.u32.u64 %r1, %rd7;\n"
    "  st.global.v2.u32 [%rd13], {%r1, %r4};\n"
    "DONE:\n"
    "  ret;\n"
    "}\n";
static CUfunction diff;

// Where the packed words go, on the GPU and on the CPU (pinned, so that
// the copy is the GPU's alone), and their count.
static CUdeviceptr pack, counter;
static uint32_t *packed;
static uint64_t *counted;
static int64_t packBytes;

// Below this many bytes a range comes back as it is: the kernel and the
// second wait cost more than the bytes.
enum { ent_gpu_diff_least = 256 * 1024 };

static bool diffOff(void) {
  static int off = -1;
  if (off < 0)
    off = getenv("ENT_GPU_WHOLE") != NULL;
  return off;
}

static void toHost(ent_gpu_piece *held, int64_t size) {
  int64_t from = held->gpuFrom, to = least(held->gpuTo, size);
  held->gpuFrom = held->gpuTo = 0;
  if (from >= to)
    return;
  if (to - from < ent_gpu_diff_least || diffOff()) {
    plainToHost(held, from, to);
    shadowCopies(held, from, to);
    ent_gpu_wait();
    return;
  }
  // The first time (or the piece outgrew it): a shadow made of what the
  // GPU has, once the CPU has that too.
  if (held->shadowBytes < held->bytes + 16) {
    if (held->shadow)
      check(cuMemFreeAsync(held->shadow, stream), "cuMemFreeAsync");
    held->shadowBytes = held->bytes + held->bytes / 4 + 16;
    check(cuMemAllocAsync(&held->shadow, (size_t)held->shadowBytes, stream),
          "cuMemAllocAsync (a shadow)");
    pending = true;
    plainToHost(held, from, to);
    shadowCopies(held, 0, held->bytes);
    ent_gpu_wait();
    return;
  }
  // The words wholly in the range; the bytes before and after them come
  // as they are.
  int64_t start = held->offset + from, end = held->offset + to;
  int64_t first = (start + 3) & ~(int64_t)3, last = end & ~(int64_t)3;
  if (first >= last) {
    plainToHost(held, from, to);
    shadowCopies(held, from, to);
    ent_gpu_wait();
    return;
  }
  plainToHost(held, from, first - held->offset);
  shadowCopies(held, from, first - held->offset);
  plainToHost(held, last - held->offset, to);
  shadowCopies(held, last - held->offset, to);
  uint64_t words = (uint64_t)(last - first) / 4;
  // (Room for one word in eight: more than that, and the range comes as
  // it is, which then costs no more than twice the packing would.)
  uint64_t cap = words / 8;
  if (!diff) {
    CUmodule module;
    check(cuModuleLoadData(&module, diffPtx), "loading ent_gpu_diff");
    check(cuModuleGetFunction(&diff, module, "ent_gpu_diff"), "ent_gpu_diff");
    check(cuMemAlloc(&counter, sizeof(uint64_t)), "cuMemAlloc (a count)");
    check(cuMemAllocHost((void **)&counted, sizeof(uint64_t)),
          "cuMemAllocHost (a count)");
  }
  if ((int64_t)(cap * 8) > packBytes) {
    if (pack)
      check(cuMemFreeAsync(pack, stream), "cuMemFreeAsync");
    if (packed)
      check(cuMemFreeHost(packed), "cuMemFreeHost");
    packBytes = (int64_t)(cap * 8) + (int64_t)(cap * 8) / 4 + 8;
    check(cuMemAllocAsync(&pack, (size_t)packBytes, stream),
          "cuMemAllocAsync (packed words)");
    check(cuMemAllocHost((void **)&packed, (size_t)packBytes),
          "cuMemAllocHost (packed words)");
  }
  check(cuMemsetD32Async(counter, 0, 2, stream), "a count's start");
  CUdeviceptr cur = device + (CUdeviceptr)first;
  CUdeviceptr shadow = shadowOf(held, first - held->offset);
  void *params[] = {&cur, &shadow, &words, &pack, &cap, &counter};
  check(cuLaunchKernel(diff, (unsigned)((words + 255) / 256), 1, 1, 256, 1,
                       1, 0, stream, params, NULL),
        "ent_gpu_diff");
  check(cuMemcpyDtoHAsync(counted, counter, sizeof(uint64_t), stream),
        "a count from the GPU");
  pending = true;
  ent_gpu_wait();
  uint64_t count = *counted;
  lookedWords += (int64_t)words;
  if (count > cap) {
    // (The kernel made the shadow the GPU's all the same.)
    plainToHost(held, first - held->offset, last - held->offset);
    ent_gpu_wait();
    return;
  }
  if (count) {
    check(cuMemcpyDtoHAsync(packed, pack, (size_t)(count * 8), stream),
          "packed words from the GPU");
    pending = true;
    ent_gpu_wait();
  }
  packedWords += (int64_t)count;
  uint32_t *words32 = (uint32_t *)(host + first);
  for (uint64_t k = 0; k < count; ++k)
    words32[packed[2 * k]] = packed[2 * k + 1];
  if (getenv("ENT_GPU_CHECK")) {
    uint32_t *all = malloc((size_t)(last - first));
    check(cuMemcpyDtoH(all, device + (CUdeviceptr)first, (size_t)(last - first)),
          "a check");
    int64_t wrong = 0, at = -1;
    for (uint64_t k = 0; k < words; ++k)
      if (all[k] != words32[k]) {
        if (at < 0)
          at = (int64_t)k;
        ++wrong;
      }
    if (wrong)
      fprintf(stderr, "ent_gpu check: piece at %lld, %lld of %llu words wrong "
              "(first %lld), %llu packed\n", (long long)held->offset,
              (long long)wrong, (unsigned long long)words, (long long)at,
              (unsigned long long)count);
    free(all);
  }
}

void ent_gpu_to_host(int64_t id, int64_t offset, int64_t size) {
  ent_gpu_piece *held = piece(id, offset);
  held->bytes = size;
  logged("to_host", id, offset, held->gpuFrom, least(held->gpuTo, size),
         held->shadow ? 1 : 0);
  toHost(held, size);
}

// After a run: the bytes [from, to) of the piece are newer where it ran.
void ent_gpu_wrote(int64_t id, int64_t onGpu, int64_t offset, int64_t from,
                   int64_t to) {
  ent_gpu_piece *held = piece(id, offset);
  logged(onGpu ? "wrote_gpu" : "wrote_cpu", id, offset, from, to, 0);
  if (from >= to)
    return;
  held->bytes = most(held->bytes, to);
  int64_t *mine = onGpu ? &held->gpuFrom : &held->cpuFrom;
  int64_t *mineTo = onGpu ? &held->gpuTo : &held->cpuTo;
  if (*mine >= *mineTo) {
    *mine = from;
    *mineTo = to;
  } else {
    *mine = least(*mine, from);
    *mineTo = most(*mineTo, to);
  }
  if (onGpu)
    without(&held->cpuFrom, &held->cpuTo, from, to);
  else
    without(&held->gpuFrom, &held->gpuTo, from, to);
}

// A unique that a loop for the GPU accumulates into: its cell in the GPU's
// copy starts where the rule does, and what the loop made of it comes
// back to the CPU, which combines it into the unique (`bytes` of it).
void ent_gpu_cell_set(int64_t offset, int64_t bits, int64_t bytes) {
  // (From the CPU's memory as it is: the call returns when it has it.)
  check(cuMemcpyHtoDAsync(device + (CUdeviceptr)offset, &bits, (size_t)bytes,
                          stream),
        "a unique's cell to the GPU");
  pending = true;
}

int64_t ent_gpu_cell_get(int64_t offset, int64_t bytes) {
  int64_t bits = 0;
  check(cuMemcpyDtoHAsync(&bits, device + (CUdeviceptr)offset, (size_t)bytes,
                          stream),
        "a unique's cell from the GPU");
  pending = true;
  ent_gpu_wait();
  return bits;
}

// The scratch arrays of running sums (see lowerRunning): one for each
// running sum of a query, in the GPU's memory, kept from run to run and
// made bigger where a run needs more. (Queries run one after another on
// the stream: one's kernels are done with them before the next's start.)
static CUdeviceptr scratches[16];
static int64_t scratchBytes[16];

void _mlir_ciface_ent_gpu_scratch(ent_gpu_memref *scratch, int64_t slot,
                                  int64_t size) {
  if (slot < 0 || slot >= 16) {
    fprintf(stderr, "ent: the GPU: more than 16 running sums in a 'for'\n");
    exit(3);
  }
  if (size > scratchBytes[slot]) {
    start();
    if (scratches[slot])
      check(cuMemFreeAsync(scratches[slot], stream), "cuMemFreeAsync");
    int64_t bytes = size + size / 4;
    check(cuMemAllocAsync(&scratches[slot], (size_t)bytes, stream),
          "cuMemAllocAsync (a running sum)");
    scratchBytes[slot] = bytes;
    pending = true;
  }
  scratch->allocated = scratch->aligned = (char *)(uintptr_t)scratches[slot];
  scratch->offset = 0;
  scratch->size = size;
  scratch->stride = 1;
}

// What a scan left at `offset` of a scratch array: the whole sum.
int64_t ent_gpu_scratch_get(int64_t slot, int64_t offset, int64_t bytes) {
  int64_t bits = 0;
  check(cuMemcpyDtoHAsync(&bits, scratches[slot] + (CUdeviceptr)offset,
                          (size_t)bytes, stream),
        "a running sum from the GPU");
  pending = true;
  ent_gpu_wait();
  return bits;
}

// For a C host, between schedule calls: everything the GPU has newer to
// the CPU, and from then on the CPU's (the host may write any of it).
void ent_gpu_world_to_host(void) {
  for (int64_t id = 0; id < pieceCount; ++id) {
    if (!pieces[id].seen)
      continue;
    toHost(&pieces[id], pieces[id].bytes);
    pieces[id].cpuFrom = 0;
    pieces[id].cpuTo = INT64_MAX;
  }
  ent_gpu_wait();
}

// MLIR's launches. The kernels are PTX, compiled by the driver when the
// program loads them.
void *mgpuModuleLoadJIT(void *ptx, int level) {
  start();
  CUmodule module;
  CUjit_option options[] = {CU_JIT_OPTIMIZATION_LEVEL};
  void *values[] = {(void *)(intptr_t)level};
  check(cuModuleLoadDataEx(&module, ptx, 1, options, values), "loading kernels");
  return module;
}

void *mgpuModuleLoad(void *data, size_t size) {
  (void)size;
  start();
  CUmodule module;
  check(cuModuleLoadData(&module, data), "loading kernels");
  return module;
}

void mgpuModuleUnload(void *module) { cuModuleUnload((CUmodule)module); }

void *mgpuModuleGetFunction(void *module, const char *name) {
  CUfunction function;
  check(cuModuleGetFunction(&function, (CUmodule)module, name), name);
  return function;
}

// The world's stream, for every launch.
void *mgpuStreamCreate(void) {
  start();
  return stream;
}
void mgpuStreamSynchronize(void *given) { (void)given; }
void mgpuStreamDestroy(void *given) { (void)given; }

void mgpuLaunchKernel(void *function, intptr_t gridX, intptr_t gridY,
                      intptr_t gridZ, intptr_t blockX, intptr_t blockY,
                      intptr_t blockZ, int32_t sharedBytes, void *given,
                      void **params, void **extra, size_t paramsCount) {
  (void)given;
  (void)paramsCount;
  // (No blocks: a loop over no entities.)
  if (gridX == 0 || gridY == 0 || gridZ == 0)
    return;
  check(cuLaunchKernel((CUfunction)function, gridX, gridY, gridZ, blockX,
                       blockY, blockZ, sharedBytes, stream, params, extra),
        "cuLaunchKernel");
  pending = true;
}
