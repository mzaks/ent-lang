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
typedef struct {
  bool seen;
  int64_t offset;
  int64_t bytes;
  int64_t cpuFrom, cpuTo;
  int64_t gpuFrom, gpuTo;
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

void ent_gpu_to_device(int64_t id, int64_t offset, int64_t size) {
  ent_gpu_piece *held = piece(id, offset);
  held->bytes = size;
  int64_t from = held->cpuFrom, to = least(held->cpuTo, size);
  held->cpuFrom = held->cpuTo = 0;
  if (from >= to)
    return;
  // (From the CPU's memory as it is: the call returns when it has it.)
  check(cuMemcpyHtoDAsync(device + (CUdeviceptr)(offset + from),
                          host + offset + from, (size_t)(to - from), stream),
        "a copy to the GPU");
  pending = true;
}

static void toHost(ent_gpu_piece *held, int64_t size) {
  int64_t from = held->gpuFrom, to = least(held->gpuTo, size);
  held->gpuFrom = held->gpuTo = 0;
  if (from >= to)
    return;
  check(cuMemcpyDtoHAsync(host + held->offset + from,
                          device + (CUdeviceptr)(held->offset + from),
                          (size_t)(to - from), stream),
        "a copy from the GPU");
  pending = true;
  ent_gpu_wait();
}

void ent_gpu_to_host(int64_t id, int64_t offset, int64_t size) {
  ent_gpu_piece *held = piece(id, offset);
  held->bytes = size;
  toHost(held, size);
}

// After a run: the bytes [from, to) of the piece are newer where it ran.
void ent_gpu_wrote(int64_t id, int64_t onGpu, int64_t offset, int64_t from,
                   int64_t to) {
  ent_gpu_piece *held = piece(id, offset);
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
