// A program on many ranks (`tools/ent build --ranks`, docs/ranks.md):
// what the compiler calls, which alone calls MPI.
//
// The same program runs as many processes, the ranks, each with a world
// of its own; `mpirun -n 4 program` starts four. What a device does (an
// extern proc: the console, files, the clock) rank 0 does, and what it
// gives back every rank is given (`ent_ranks_bcast`), so that every rank
// goes on alike and the program says what it says on one process.
// (open_memstream.)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <execinfo.h>
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Partitions in a process. Built for partitions, a process runs several,
// each a thread with a world of its own (ent_ranks_run): a partition is a
// rank of the program (`ent_rank` of `ent_ranks`), and what the ranks do
// together the partitions of a process do through memory, and the
// processes through MPI, which one partition of each does for all of
// them. At most ENT_RUNNING partitions of a process go through their rows
// at once (as many as there are cores; 1: one after another, the least
// memory); one waiting for the others runs none.

static int ent_procs = 1, ent_proc = 0, ent_locals = 1;
static _Thread_local int ent_local = 0;
static pthread_mutex_t ent_meeting = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ent_met = PTHREAD_COND_INITIALIZER;
static int ent_arrived = 0;
static long ent_generation = 0;
static void **ent_args = 0;
static pthread_mutex_t ent_running = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ent_free_slot = PTHREAD_COND_INITIALIZER;
static int ent_slots = 1;

static void ent_slot_take(void) {
  pthread_mutex_lock(&ent_running);
  while (ent_slots == 0)
    pthread_cond_wait(&ent_free_slot, &ent_running);
  --ent_slots;
  pthread_mutex_unlock(&ent_running);
}
static void ent_slot_give(void) {
  pthread_mutex_lock(&ent_running);
  ++ent_slots;
  pthread_cond_signal(&ent_free_slot);
  pthread_mutex_unlock(&ent_running);
}

// All the process's partitions meet: each gives what it brings (`mine`),
// and the last to come does `work` for all of them (by their local
// numbers), the others waiting; then each goes on.
static void ent_meet(void *mine, void (*work)(void **)) {
  ent_slot_give();
  pthread_mutex_lock(&ent_meeting);
  ent_args[ent_local] = mine;
  long generation = ent_generation;
  if (++ent_arrived == ent_locals) {
    work(ent_args);
    ent_arrived = 0;
    ++ent_generation;
    pthread_cond_broadcast(&ent_met);
  } else {
    while (generation == ent_generation)
      pthread_cond_wait(&ent_met, &ent_meeting);
  }
  pthread_mutex_unlock(&ent_meeting);
  ent_slot_take();
}

// (The global rank of the `l`th partition of process `p`.)
static int ent_global(int p, int l) { return p * ent_locals + l; }

// -- A whole number combined over all the partitions.
typedef struct {
  int64_t in, out;
  MPI_Op op;
} ent_reduce_args;
static int64_t ent_fold(int64_t a, int64_t b, MPI_Op op) {
  if (op == MPI_MIN)
    return a < b ? a : b;
  if (op == MPI_MAX)
    return a > b ? a : b;
  return a + b;
}
static void ent_reduce_work(void **args) {
  ent_reduce_args *first = args[0];
  int64_t local = first->in;
  for (int l = 1; l < ent_locals; ++l)
    local = ent_fold(local, ((ent_reduce_args *)args[l])->in, first->op);
  int64_t all = local;
  if (ent_procs > 1)
    MPI_Allreduce(&local, &all, 1, MPI_INT64_T, first->op, MPI_COMM_WORLD);
  for (int l = 0; l < ent_locals; ++l)
    ((ent_reduce_args *)args[l])->out = all;
}
static int64_t ent_allreduce(int64_t mine, MPI_Op op) {
  ent_reduce_args args = {mine, 0, op};
  ent_meet(&args, ent_reduce_work);
  return args.out;
}

// -- What the partitions before this one add up to (nought for the first).
static void ent_exscan_work(void **args) {
  int64_t local = 0;
  for (int l = 0; l < ent_locals; ++l)
    local += ((ent_reduce_args *)args[l])->in;
  int64_t before = 0;
  if (ent_procs > 1) {
    MPI_Exscan(&local, &before, 1, MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD);
    if (ent_proc == 0)
      before = 0;
  }
  for (int l = 0; l < ent_locals; ++l) {
    ent_reduce_args *a = args[l];
    a->out = before;
    before += a->in;
  }
}
static int64_t ent_exscan(int64_t mine) {
  ent_reduce_args args = {mine, 0, MPI_SUM};
  ent_meet(&args, ent_exscan_work);
  return args.out;
}

// -- `bytes` at `data`, as partition `root` has them, on every partition.
typedef struct {
  void *data;
  int64_t bytes;
  int root;
} ent_bcast_args;
static void ent_bcast_work(void **args) {
  ent_bcast_args *first = args[0];
  int root = first->root, from = root / ent_locals;
  int64_t bytes = first->bytes;
  void *source = from == ent_proc ? ((ent_bcast_args *)args[root % ent_locals])->data
                                  : first->data;
  if (ent_procs > 1)
    for (int64_t at = 0; at < bytes; at += 1 << 30) {
      int64_t n = bytes - at < (1 << 30) ? bytes - at : (1 << 30);
      MPI_Bcast((char *)source + at, (int)n, MPI_BYTE, from, MPI_COMM_WORLD);
    }
  for (int l = 0; l < ent_locals; ++l) {
    ent_bcast_args *a = args[l];
    if (a->data != source)
      memcpy(a->data, source, bytes);
  }
}
static void ent_bcast(void *data, int64_t bytes, int root) {
  ent_bcast_args args = {data, bytes, root};
  ent_meet(&args, ent_bcast_work);
}

// -- Each partition's `bytes` (`mine`), all of them in the order of the
// ranks (`all`, ranks times as many), on every partition.
typedef struct {
  const void *mine;
  void *all;
  int64_t bytes;
} ent_gather_args;
static void ent_allgather_work(void **args) {
  ent_gather_args *first = args[0];
  int64_t bytes = first->bytes, block = bytes * ent_locals;
  char *all = first->all;
  for (int l = 0; l < ent_locals; ++l)
    memcpy(all + (ent_proc * ent_locals + l) * bytes,
           ((ent_gather_args *)args[l])->mine, bytes);
  if (ent_procs > 1)
    MPI_Allgather(MPI_IN_PLACE, 0, MPI_BYTE, all, (int)block, MPI_BYTE,
                  MPI_COMM_WORLD);
  for (int l = 1; l < ent_locals; ++l)
    memcpy(((ent_gather_args *)args[l])->all, all,
           bytes * ent_locals * ent_procs);
}
static void ent_allgather(const void *mine, int64_t bytes, void *all) {
  ent_gather_args args = {mine, all, bytes};
  ent_meet(&args, ent_allgather_work);
}

// -- Each partition's piece, `counts[r]` bytes at `displs[r]` (by rank):
// from its `send` into every partition's `recv` (or, where `send` is 0,
// from its own `recv`, in place).
typedef struct {
  const void *send;
  void *recv;
  const int64_t *counts, *displs;
} ent_pieces_args;
static void ent_allgatherv_work(void **args) {
  ent_pieces_args *first = args[0];
  const int64_t *counts = first->counts, *displs = first->displs;
  int ranks = ent_locals * ent_procs;
  // (Each partition's piece where every partition of the process has it.)
  for (int l = 0; l < ent_locals; ++l) {
    ent_pieces_args *a = args[l];
    int r = ent_global(ent_proc, l);
    const char *piece = a->send ? a->send : (char *)a->recv + displs[r];
    for (int m = 0; m < ent_locals; ++m) {
      char *to = (char *)((ent_pieces_args *)args[m])->recv + displs[r];
      if (to != piece)
        memmove(to, piece, counts[r]);
    }
  }
  if (ent_procs == 1)
    return;
  // (Between processes: each one's pieces, one after another.)
  int *sizes = malloc(sizeof(int) * ent_procs), *starts = malloc(sizeof(int) * ent_procs);
  int64_t total = 0;
  for (int p = 0; p < ent_procs; ++p) {
    int64_t size = 0;
    for (int l = 0; l < ent_locals; ++l)
      size += counts[ent_global(p, l)];
    if (size > 0x7fffffff || total > 0x7fffffff) {
      fprintf(stderr, "ent: a process's pieces past 2 GB: not on many ranks yet\n");
      MPI_Abort(MPI_COMM_WORLD, 1);
    }
    sizes[p] = (int)size;
    starts[p] = (int)total;
    total += size;
  }
  char *packed = malloc(total ? total : 1);
  char *at = packed + starts[ent_proc];
  for (int l = 0; l < ent_locals; ++l) {
    int r = ent_global(ent_proc, l);
    memcpy(at, (char *)first->recv + displs[r], counts[r]);
    at += counts[r];
  }
  MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_BYTE, packed, sizes, starts, MPI_BYTE,
                 MPI_COMM_WORLD);
  for (int p = 0; p < ent_procs; ++p) {
    if (p == ent_proc)
      continue;
    char *from = packed + starts[p];
    for (int l = 0; l < ent_locals; ++l) {
      int r = ent_global(p, l);
      for (int m = 0; m < ent_locals; ++m)
        memcpy((char *)((ent_pieces_args *)args[m])->recv + displs[r], from,
               counts[r]);
      from += counts[r];
    }
  }
  (void)ranks;
  free(packed);
  free(sizes);
  free(starts);
}
static void ent_allgatherv(const void *send, void *recv, const int64_t *counts,
                           const int64_t *displs) {
  ent_pieces_args args = {send, recv, counts, displs};
  ent_meet(&args, ent_allgatherv_work);
}

// -- Each partition's words to each (`sent[r]` of them at `sent_at[r]` of
// `out`, by rank), given to each (`got[r]` from rank r at `got_at[r]` of
// `in`); the counts first (ent_alltoall).
typedef struct {
  const int64_t *sent;
  int64_t *got;
} ent_counts_args;
static void ent_alltoall_work(void **args) {
  int ranks = ent_locals * ent_procs;
  // (got[l][r]: what rank r sends local partition l.)
  int64_t *matrix = malloc(sizeof(int64_t) * ent_locals * ranks);
  for (int l = 0; l < ent_locals; ++l)
    for (int m = 0; m < ent_locals; ++m)
      ((ent_counts_args *)args[m])->got[ent_global(ent_proc, l)] =
          ((ent_counts_args *)args[l])->sent[ent_global(ent_proc, m)];
  if (ent_procs > 1) {
    // (To process q, from each of ours to each of its: locals squared.)
    int64_t square = (int64_t)ent_locals * ent_locals;
    int64_t *out = malloc(sizeof(int64_t) * square * ent_procs);
    int64_t *in = malloc(sizeof(int64_t) * square * ent_procs);
    for (int q = 0; q < ent_procs; ++q)
      for (int l = 0; l < ent_locals; ++l)
        for (int m = 0; m < ent_locals; ++m)
          out[q * square + l * ent_locals + m] =
              ((ent_counts_args *)args[l])->sent[ent_global(q, m)];
    MPI_Alltoall(out, (int)square, MPI_INT64_T, in, (int)square, MPI_INT64_T,
                 MPI_COMM_WORLD);
    for (int p = 0; p < ent_procs; ++p)
      if (p != ent_proc)
        for (int l = 0; l < ent_locals; ++l)
          for (int m = 0; m < ent_locals; ++m)
            ((ent_counts_args *)args[m])->got[ent_global(p, l)] =
                in[p * square + l * ent_locals + m];
    free(out);
    free(in);
  }
  free(matrix);
}
static void ent_alltoall(const int64_t *sent, int64_t *got) {
  ent_counts_args args = {sent, got};
  ent_meet(&args, ent_alltoall_work);
}

typedef struct {
  const int64_t *out;
  const int64_t *sent, *sent_at;
  int64_t *in;
  const int64_t *got, *got_at;
} ent_words_args;
static void ent_alltoallv_work(void **args) {
  // (Within the process, from each to each.)
  for (int l = 0; l < ent_locals; ++l) {
    ent_words_args *from = args[l];
    int source = ent_global(ent_proc, l);
    for (int m = 0; m < ent_locals; ++m) {
      ent_words_args *to = args[m];
      int target = ent_global(ent_proc, m);
      memcpy(to->in + to->got_at[source], from->out + from->sent_at[target],
             sizeof(int64_t) * from->sent[target]);
    }
  }
  if (ent_procs == 1)
    return;
  // (Between processes: to process q, from each of ours to each of its in
  // turn, packed; unpacked so on the other side.)
  int *sizes = calloc(ent_procs, sizeof(int)), *size_at = calloc(ent_procs, sizeof(int));
  int *gets = calloc(ent_procs, sizeof(int)), *get_at = calloc(ent_procs, sizeof(int));
  int64_t total = 0, incoming = 0;
  for (int q = 0; q < ent_procs; ++q) {
    int64_t n = 0, g = 0;
    if (q != ent_proc)
      for (int l = 0; l < ent_locals; ++l)
        for (int m = 0; m < ent_locals; ++m) {
          n += ((ent_words_args *)args[l])->sent[ent_global(q, m)];
          g += ((ent_words_args *)args[m])->got[ent_global(q, l)];
        }
    if (total + n > 0x7fffffff || incoming + g > 0x7fffffff) {
      fprintf(stderr, "ent: more than 2^31 words between processes: not on "
                      "many ranks yet\n");
      MPI_Abort(MPI_COMM_WORLD, 1);
    }
    sizes[q] = (int)n;
    size_at[q] = (int)total;
    gets[q] = (int)g;
    get_at[q] = (int)incoming;
    total += n;
    incoming += g;
  }
  int64_t *out = malloc(sizeof(int64_t) * (total ? total : 1));
  int64_t *in = malloc(sizeof(int64_t) * (incoming ? incoming : 1));
  for (int q = 0, at = 0; q < ent_procs; ++q)
    if (q != ent_proc)
      for (int l = 0; l < ent_locals; ++l)
        for (int m = 0; m < ent_locals; ++m) {
          ent_words_args *from = args[l];
          int target = ent_global(q, m);
          memcpy(out + at, from->out + from->sent_at[target],
                 sizeof(int64_t) * from->sent[target]);
          at += from->sent[target];
        }
  MPI_Alltoallv(out, sizes, size_at, MPI_INT64_T, in, gets, get_at,
                MPI_INT64_T, MPI_COMM_WORLD);
  for (int p = 0, at = 0; p < ent_procs; ++p)
    if (p != ent_proc)
      for (int l = 0; l < ent_locals; ++l)
        for (int m = 0; m < ent_locals; ++m) {
          ent_words_args *to = args[m];
          int source = ent_global(p, l);
          memcpy(to->in + to->got_at[source], in + at,
                 sizeof(int64_t) * to->got[source]);
          at += to->got[source];
        }
  free(out);
  free(in);
  free(sizes);
  free(size_at);
  free(gets);
  free(get_at);
}
static void ent_alltoallv(const int64_t *out, const int64_t *sent,
                          const int64_t *sent_at, int64_t *in,
                          const int64_t *got, const int64_t *got_at) {
  ent_words_args args = {out, sent, sent_at, in, got, got_at};
  ent_meet(&args, ent_alltoallv_work);
}

// -- Each partition's `bytes` at `mine`, given to partition 0: all of them
// in the order of the ranks (`*all`, taken with malloc, and their sizes in
// `sizes`), there; nothing elsewhere.
typedef struct {
  const char *mine;
  int64_t bytes;
  char **all;
  int64_t *sizes;
} ent_root_args;
static void ent_gather_root_work(void **args) {
  int ranks = ent_locals * ent_procs;
  int64_t *sizes = calloc(ranks, sizeof(int64_t));
  for (int l = 0; l < ent_locals; ++l)
    sizes[ent_global(ent_proc, l)] = ((ent_root_args *)args[l])->bytes;
  if (ent_procs > 1)
    MPI_Allgather(MPI_IN_PLACE, 0, MPI_INT64_T, sizes, ent_locals, MPI_INT64_T,
                  MPI_COMM_WORLD);
  int64_t local = 0;
  for (int l = 0; l < ent_locals; ++l)
    local += sizes[ent_global(ent_proc, l)];
  char *packed = malloc(local ? local : 1);
  for (int l = 0, at = 0; l < ent_locals; ++l) {
    ent_root_args *a = args[l];
    memcpy(packed + at, a->mine, a->bytes);
    at += a->bytes;
  }
  char *all = 0;
  if (ent_proc == 0) {
    int64_t total = 0;
    for (int r = 0; r < ranks; ++r)
      total += sizes[r];
    all = malloc(total ? total : 1);
  }
  if (ent_procs > 1) {
    int *counts = malloc(sizeof(int) * ent_procs), *starts = malloc(sizeof(int) * ent_procs);
    int64_t at = 0;
    for (int p = 0; p < ent_procs; ++p) {
      int64_t c = 0;
      for (int l = 0; l < ent_locals; ++l)
        c += sizes[ent_global(p, l)];
      counts[p] = (int)c;
      starts[p] = (int)at;
      at += c;
    }
    MPI_Gatherv(packed, (int)local, MPI_BYTE, all, counts, starts, MPI_BYTE, 0,
                MPI_COMM_WORLD);
    free(counts);
    free(starts);
  } else {
    memcpy(all, packed, local);
  }
  free(packed);
  ent_root_args *zero = args[0];
  if (ent_proc == 0) {
    *zero->all = all;
    memcpy(zero->sizes, sizes, sizeof(int64_t) * ranks);
  }
  free(sizes);
}
static void ent_gather_root(const char *mine, int64_t bytes, char **all,
                            int64_t *sizes) {
  ent_root_args args = {mine, bytes, all, sizes};
  ent_meet(&args, ent_gather_root_work);
}

// -- From one partition to the next (the ranks taking turns, see
// ent_ranks_chain_get): through memory within the process, else MPI.
typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t ready;
  char *bytes;
  int64_t size;
  int full;
} ent_mailbox;
static ent_mailbox *ent_mailboxes = 0;
static void ent_send(const void *data, int64_t bytes, int to) {
  if (to / ent_locals != ent_proc) {
    MPI_Send(data, (int)bytes, MPI_BYTE, to / ent_locals, to % ent_locals,
             MPI_COMM_WORLD);
    return;
  }
  ent_mailbox *box = &ent_mailboxes[to % ent_locals];
  pthread_mutex_lock(&box->lock);
  while (box->full)
    pthread_cond_wait(&box->ready, &box->lock);
  box->bytes = malloc(bytes ? bytes : 1);
  memcpy(box->bytes, data, bytes);
  box->size = bytes;
  box->full = 1;
  pthread_cond_broadcast(&box->ready);
  pthread_mutex_unlock(&box->lock);
}
static void ent_recv(void *data, int64_t bytes, int from) {
  if (from / ent_locals != ent_proc) {
    // (Waiting, it lets another partition of the process run.)
    ent_slot_give();
    MPI_Recv(data, (int)bytes, MPI_BYTE, from / ent_locals, ent_local,
             MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    ent_slot_take();
    return;
  }
  ent_mailbox *box = &ent_mailboxes[ent_local];
  ent_slot_give();
  pthread_mutex_lock(&box->lock);
  while (!box->full)
    pthread_cond_wait(&box->ready, &box->lock);
  memcpy(data, box->bytes, bytes);
  free(box->bytes);
  box->full = 0;
  pthread_cond_broadcast(&box->ready);
  pthread_mutex_unlock(&box->lock);
  ent_slot_take();
}

// (As MPI's, counts in ints by rank.)
static void ent_alltoall_ints(const int *sent, int *got) {
  int ranks = ent_locals * ent_procs;
  int64_t *a = malloc(sizeof(int64_t) * ranks), *b = malloc(sizeof(int64_t) * ranks);
  for (int r = 0; r < ranks; ++r)
    a[r] = sent[r];
  ent_alltoall(a, b);
  for (int r = 0; r < ranks; ++r)
    got[r] = (int)b[r];
  free(a);
  free(b);
}
static void ent_alltoallv_ints(const int64_t *out, const int *sent,
                               const int *sent_at, int64_t *in,
                               const int *got, const int *got_at) {
  int ranks = ent_locals * ent_procs;
  int64_t *w = malloc(sizeof(int64_t) * 4 * ranks);
  for (int r = 0; r < ranks; ++r) {
    w[r] = sent[r];
    w[ranks + r] = sent_at[r];
    w[2 * ranks + r] = got[r];
    w[3 * ranks + r] = got_at[r];
  }
  ent_alltoallv(out, w, w + ranks, in, w + 2 * ranks, w + 3 * ranks);
  free(w);
}
static void ent_allgatherv_ints(void *column, const int *counts,
                                const int *starts) {
  int ranks = ent_locals * ent_procs;
  int64_t *w = malloc(sizeof(int64_t) * 2 * ranks);
  for (int r = 0; r < ranks; ++r) {
    w[r] = counts[r];
    w[ranks + r] = starts[r];
  }
  ent_allgatherv(0, column, w, w + ranks);
  free(w);
}

static _Thread_local int ent_rank = 0, ent_ranks = 1, ent_started = 0;
// Whether the distributed archetypes are placed (when `main` starts its
// first loop): until then every rank goes through every row, as one
// process, and what the ranks would combine or give each other is this
// rank's alone (a sum is its own part, a running sum starts at nought).
static _Thread_local int ent_distributed = 0;

void ent_ranks_distribute(void) { ent_distributed = 1; }

// (ENT_RANKS_LOG: the time each rank spent in each kind of call, which is
// where it waited for the others too, said as it ends.)
static const char *ent_kinds[] = {"bcast", "sum", "min", "max", "before", "release", "share_begin", "share_column", "exchange", "gather_column"};
static _Thread_local double ent_spent[10];
static _Thread_local int64_t ent_calls[10];
static _Thread_local int64_t ent_posted = 0, ent_received = 0;
static _Thread_local int64_t ent_rounds = 0, ent_asked_rows = 0, ent_most_stretches = 0;
// (ENT_RANKS_LOG=2: for each place the program calls from, the time it
// worked before the call, since the call before, and spent in it.)
typedef struct {
  void *at;
  double worked, waited;
  int64_t calls;
} ent_place_spent;
static _Thread_local ent_place_spent ent_places_spent[256];
static _Thread_local double ent_last_return = 0;
static void ent_note_place(void *at, double start, double end) {
  for (int i = 0; i < 256; ++i) {
    ent_place_spent *p = &ent_places_spent[(((uintptr_t)at >> 2) + i) & 255];
    if (p->at && p->at != at)
      continue;
    p->at = at;
    if (ent_last_return > 0)
      p->worked += start - ent_last_return;
    p->waited += end - start;
    ++p->calls;
    break;
  }
  ent_last_return = end;
}
static int ent_by_worked(const void *a, const void *b) {
  const ent_place_spent *x = a, *y = b;
  double l = x->worked + x->waited, r = y->worked + y->waited;
  return l < r ? 1 : l > r ? -1 : 0;
}

// The program on its ranks: MPI started, and in each process as many
// partitions as the program is built for over the processes (one where it
// is not built for partitions), each a thread running `program` (main, as
// the compiler makes it) with a world of its own; MPI ended when all are
// done. What the first gave back.
typedef struct {
  int32_t (*program)(void);
  int local;
  int32_t status;
} ent_partition;

static void *ent_partition_runs(void *given) {
  ent_partition *partition = given;
  ent_local = partition->local;
  ent_rank = ent_global(ent_proc, ent_local);
  ent_ranks = ent_procs * ent_locals;
  ent_started = 1;
  ent_slot_take();
  partition->status = partition->program();
  ent_slot_give();
  return 0;
}

int32_t ent_ranks_run(int32_t (*program)(void), int64_t partitions) {
  int provided = 0;
  if (MPI_Init_thread(NULL, NULL, MPI_THREAD_MULTIPLE, &provided) !=
      MPI_SUCCESS) {
    fprintf(stderr, "ent: MPI did not start\n");
    exit(1);
  }
  MPI_Comm_rank(MPI_COMM_WORLD, &ent_proc);
  MPI_Comm_size(MPI_COMM_WORLD, &ent_procs);
  ent_locals = 1;
  if (partitions > 0) {
    if (partitions % ent_procs) {
      if (ent_proc == 0)
        fprintf(stderr,
                "ent: the program is built for %lld partitions, which %d "
                "processes cannot share evenly\n",
                (long long)partitions, ent_procs);
      MPI_Abort(MPI_COMM_WORLD, 1);
    }
    ent_locals = (int)(partitions / ent_procs);
  }
  if (ent_locals > 1 && provided < MPI_THREAD_MULTIPLE) {
    if (ent_proc == 0)
      fprintf(stderr, "ent: this MPI runs no threads, needed for %d "
                      "partitions a process\n",
              ent_locals);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  ent_args = calloc(ent_locals, sizeof(void *));
  ent_mailboxes = calloc(ent_locals, sizeof(ent_mailbox));
  for (int l = 0; l < ent_locals; ++l) {
    pthread_mutex_init(&ent_mailboxes[l].lock, 0);
    pthread_cond_init(&ent_mailboxes[l].ready, 0);
  }
  // (How many go through their rows at once: ENT_RUNNING, else as many as
  // there are cores.)
  const char *running = getenv("ENT_RUNNING");
  long cores = sysconf(_SC_NPROCESSORS_ONLN);
  ent_slots = running ? atoi(running) : (int)(cores > 0 ? cores : 1);
  if (ent_slots < 1)
    ent_slots = 1;
  ent_partition *all = calloc(ent_locals, sizeof(ent_partition));
  pthread_t *threads = calloc(ent_locals, sizeof(pthread_t));
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  // (A stack as large as a program's main has.)
  pthread_attr_setstacksize(&attributes, (size_t)512 << 20);
  for (int l = 0; l < ent_locals; ++l) {
    all[l] = (ent_partition){program, l, 0};
    if (pthread_create(&threads[l], &attributes, ent_partition_runs, &all[l])) {
      fprintf(stderr, "ent: no thread for a partition\n");
      MPI_Abort(MPI_COMM_WORLD, 1);
    }
  }
  for (int l = 0; l < ent_locals; ++l)
    pthread_join(threads[l], 0);
  int32_t status = all[0].status;
  free(all);
  free(threads);
  fflush(stdout);
  MPI_Finalize();
  return status;
}

void ent_ranks_start(void) {}

// (ENT_RANKS_LOG: how much memory the rank has, as it ends.)
static void ent_log_places(void) {
  qsort(ent_places_spent, 256, sizeof *ent_places_spent, ent_by_worked);
  for (int i = 0; i < 14 && ent_places_spent[i].at; ++i) {
    Dl_info info;
    const char *name = "?";
    uintptr_t off = 0;
    // (By its place in the program's file: `nm` names it.)
    if (dladdr(ent_places_spent[i].at, &info)) {
      name = info.dli_sname ? info.dli_sname : "file";
      off = (uintptr_t)ent_places_spent[i].at -
            (uintptr_t)(info.dli_sname ? info.dli_saddr : info.dli_fbase);
    }
    fprintf(stderr, "ent: rank %d: before %s+%#lx: worked %7.3f s, waited %7.3f s (%lld)\n",
            ent_rank, name, (unsigned long)off, ent_places_spent[i].worked,
            ent_places_spent[i].waited, (long long)ent_places_spent[i].calls);
  }
}

static void ent_log_spent(void) {
  if (getenv("ENT_RANKS_LOG")[0] >= '2')
    ent_log_places();
  fprintf(stderr, "ent: rank %d: posted %lld sends, was given %lld\n",
          ent_rank, (long long)ent_posted, (long long)ent_received);
  fprintf(stderr,
          "ent: rank %d: %lld rounds of asking, %lld rows of buffers asked "
          "for, at most %lld stretches of a buffer's rows\n",
          ent_rank, (long long)ent_rounds, (long long)ent_asked_rows,
          (long long)ent_most_stretches);
  for (unsigned k = 0; k < sizeof ent_kinds / sizeof *ent_kinds; ++k)
    if (ent_calls[k])
      fprintf(stderr, "ent: rank %d: %-13s %8lld calls %9.3f s\n", ent_rank,
              ent_kinds[k], (long long)ent_calls[k], ent_spent[k]);
}

static void ent_log_memory(void) {
  FILE *status = fopen("/proc/self/status", "r");
  if (!status)
    return;
  char line[256];
  while (fgets(line, sizeof line, status))
    if (!strncmp(line, "VmRSS:", 6) || !strncmp(line, "VmHWM:", 6))
      fprintf(stderr, "ent: rank %d: %s", ent_rank, line);
  fclose(status);
}

// The world's memory (ENT_RANKS_LOG=3: which of its pages each rank has
// as it ends, to ent-ranks-pages.<rank>, by MB: the offset and the pages
// of it the rank has).
static _Thread_local char *ent_arena = 0;
static _Thread_local int64_t ent_arena_bytes = 0;
void ent_ranks_arena(void *arena, int64_t bytes) {
  ent_arena = arena;
  ent_arena_bytes = bytes;
}
static void ent_log_pages(void) {
  if (!ent_arena)
    return;
  long page = sysconf(_SC_PAGESIZE);
  uintptr_t start = (uintptr_t)ent_arena & ~(uintptr_t)(page - 1);
  int64_t length = (int64_t)((uintptr_t)ent_arena + ent_arena_bytes - start);
  int64_t pages = (length + page - 1) / page;
  unsigned char *have = malloc(pages);
  if (mincore((void *)start, pages * page, have) != 0) {
    free(have);
    return;
  }
  char name[64];
  snprintf(name, sizeof name, "ent-ranks-pages.%d", ent_rank);
  FILE *out = fopen(name, "w");
  int64_t per = (1 << 20) / page;
  for (int64_t at = 0; at < pages; at += per) {
    int64_t n = 0;
    for (int64_t i = at; i < pages && i < at + per; ++i)
      n += have[i] & 1;
    if (n)
      fprintf(out, "%lld %lld\n", (long long)(at * page), (long long)n);
  }
  fclose(out);
  free(have);
}

void ent_ranks_end(void) {
  if (!ent_started)
    return;
  {
    const char *log = getenv("ENT_RANKS_LOG");
    if (log && log[0] == '3')
      ent_log_pages();
  }
  if (getenv("ENT_RANKS_LOG")) {
    ent_log_spent();
    ent_log_memory();
  }
  // (What rank 0 wrote is out before the other ranks are gone.)
  fflush(stdout);
  ent_started = 0;
}

int32_t ent_ranks_rank(void) { return ent_rank; }
int32_t ent_ranks_count(void) { return ent_ranks; }

// `bytes` bytes at `data`, as rank 0 has them, on every rank. (In pieces
// of at most 1 GB: MPI counts in ints.)
static void ent_ranks_bcast_untimed(void *data, int64_t bytes) {
  char *at = (char *)data;
  while (bytes > 0) {
    int piece = bytes > (1 << 30) ? (1 << 30) : (int)bytes;
    ent_bcast(at, piece, 0);
    at += piece;
    bytes -= piece;
  }
}

// ---------------------------------------------------------------------------
// Placement. The entities of a distributed archetype (`distribute Colony
// by number`) are every rank's, as the program spawned them on every rank;
// each rank goes through its own stretch of the rows, `bounds[rank]` to
// `bounds[rank + 1]`, and the other rows are not its to read or write. The
// stretches are worked out alike on every rank, from what every rank has.

typedef struct {
  int placed;
  int64_t count;
  int64_t *bounds; // ranks + 1
  int32_t *noted;  // while placing by an entity: each row's rank
  int dropped;     // the other ranks' rows let go (ent_ranks_drop)
  // Built for partitions (one a rank): partition p has the rows from
  // p * rows, as many as counts[p] says (the world's, on every rank); with
  // first_only all are partition 0's, as many as *counts.
  int64_t rows;
  const int64_t *counts;
  int first_only;
} ent_placement;

static _Thread_local ent_placement *ent_places = 0;
static _Thread_local int32_t ent_places_known = 0;

static void ent_ranks_stop(const char *why) {
  fprintf(stderr, "ent: %s\n", why);
  // (ENT_RANKS_LOG: where it was, by the functions' names.)
  if (getenv("ENT_RANKS_LOG")) {
    void *frames[24];
    backtrace_symbols_fd(frames, backtrace(frames, 24), 2);
  }
  fflush(stderr);
  MPI_Abort(MPI_COMM_WORLD, 1);
  exit(1);
}

static ent_placement *ent_place_of(int32_t archetype) {
  if (archetype >= ent_places_known) {
    int32_t more = archetype + 16;
    ent_places = realloc(ent_places, sizeof(ent_placement) * more);
    for (int32_t i = ent_places_known; i < more; ++i)
      ent_places[i] = (ent_placement){0, 0, 0, 0, 0, 0, 0, 0};
    ent_places_known = more;
  }
  return &ent_places[archetype];
}

// Whether the archetype is to be placed (1), or is placed with these many
// entities already (0). Entities spawned into it, or gone from it, after
// it was placed stop the program: not on many ranks yet.
int32_t ent_ranks_unplaced(int32_t archetype, int64_t count) {
  ent_placement *place = ent_place_of(archetype);
  if (place->rows)
    return 0;
  if (!place->placed)
    return 1;
  if (place->count != count)
    ent_ranks_stop("entities were spawned into, or are gone from, a "
                   "distributed archetype after its entities were placed: "
                   "not on many ranks yet");
  return 0;
}

// (Stretches from each row's rank, which go up with the rows.)
static void ent_settle(ent_placement *place, int64_t count,
                       const int32_t *rank_of) {
  place->bounds = realloc(place->bounds, sizeof(int64_t) * (ent_ranks + 1));
  int64_t row = 0;
  for (int r = 0; r <= ent_ranks; ++r) {
    while (row < count && rank_of[row] < r)
      ++row;
    place->bounds[r] = row;
  }
  for (int64_t i = 1; i < count; ++i)
    if (rank_of[i] < rank_of[i - 1])
      ent_ranks_stop("the rows of a distributed archetype are not in the "
                     "order of what it is distributed by: not on many ranks "
                     "yet");
  place->count = count;
  place->placed = 1;
  // (ENT_RANKS_LOG: each rank's stretch, as it is placed.)
  if (getenv("ENT_RANKS_LOG"))
    fprintf(stderr, "ent: rank %d of %d: rows %lld to %lld of %lld\n",
            ent_rank, ent_ranks, (long long)place->bounds[ent_rank],
            (long long)place->bounds[ent_rank + 1], (long long)count);
}

static int64_t ent_value(const void *values, int32_t bits, int32_t is_signed,
                         int64_t row) {
  switch (bits) {
  case 8:
    return is_signed ? ((const int8_t *)values)[row]
                     : ((const uint8_t *)values)[row];
  case 16:
    return is_signed ? ((const int16_t *)values)[row]
                     : ((const uint16_t *)values)[row];
  case 32:
    return is_signed ? ((const int32_t *)values)[row]
                     : ((const uint32_t *)values)[row];
  default:
    return ((const int64_t *)values)[row];
  }
}

// By a whole number: its values from the least to the most cut into as
// many equal ranges as there are ranks.
void ent_ranks_place_range(int32_t archetype, int64_t count,
                           const void *values, int32_t bits,
                           int32_t is_signed) {
  int32_t *rank_of = malloc(sizeof(int32_t) * (count > 0 ? count : 1));
  int64_t least = 0, most = 0;
  for (int64_t i = 0; i < count; ++i) {
    int64_t v = ent_value(values, bits, is_signed, i);
    if (i == 0 || v < least)
      least = v;
    if (i == 0 || v > most)
      most = v;
  }
  __int128 span = (__int128)most - least + 1;
  for (int64_t i = 0; i < count; ++i)
    rank_of[i] = (int32_t)(((__int128)ent_value(values, bits, is_signed, i) -
                            least) * ent_ranks / span);
  ent_settle(ent_place_of(archetype), count, rank_of);
  free(rank_of);
}

// By its rows (an archetype not declared distributed that a `for` goes
// over with one that is): as many even stretches as there are ranks.
void ent_ranks_place_rows(int32_t archetype, int64_t count) {
  int32_t *rank_of = malloc(sizeof(int32_t) * (count > 0 ? count : 1));
  for (int64_t i = 0; i < count; ++i)
    rank_of[i] = (int32_t)((__int128)i * ent_ranks / count);
  ent_settle(ent_place_of(archetype), count, rank_of);
  free(rank_of);
}

// By an entity: each row with the rank of the entity it names, noted one
// by one, then settled.
void ent_ranks_note(int32_t archetype, int64_t count, int64_t row,
                    int32_t rank) {
  ent_placement *place = ent_place_of(archetype);
  if (row == 0)
    place->noted = realloc(place->noted,
                           sizeof(int32_t) * (count > 0 ? count : 1));
  place->noted[row] = rank;
}

void ent_ranks_settle(int32_t archetype, int64_t count) {
  ent_placement *place = ent_place_of(archetype);
  if (count == 0)
    place->noted = realloc(place->noted, sizeof(int32_t));
  ent_settle(place, count, place->noted);
  free(place->noted);
  place->noted = 0;
}

// Built for partitions: the archetype's rows of each partition (one a
// rank), and where the world keeps how many each has. Placed from the
// start: entities are spawned into their partitions.
void ent_ranks_partition(int32_t archetype, int64_t rows, const int64_t *counts,
                         int32_t first_only) {
  ent_placement *place = ent_place_of(archetype);
  place->placed = 1;
  place->rows = rows;
  place->counts = counts;
  place->first_only = first_only;
}

static int64_t ent_partition_first(const ent_placement *place, int r) {
  if (place->first_only)
    return r == 0 ? 0 : place->counts[0];
  return r * place->rows;
}
static int64_t ent_partition_end(const ent_placement *place, int r) {
  if (place->first_only)
    return place->counts[0];
  return r * place->rows + place->counts[r];
}

// The partition an entity with this value of what its archetype is
// distributed by goes to: the value over the block (ENT_BLOCK, else half
// the partition's rows, an even share of the capacity), the last taking
// the rest.
int32_t ent_ranks_partition_of(int32_t archetype, int64_t value) {
  ent_placement *place = ent_place_of(archetype);
  static _Thread_local int64_t block = -1;
  if (block < 0) {
    const char *given = getenv("ENT_BLOCK");
    block = given ? atoll(given) : 0;
  }
  int64_t size = block > 0 ? block : (place->rows > 1 ? place->rows / 2 : 1);
  int64_t p = value < 0 ? 0 : value / size;
  return (int32_t)(p >= ent_ranks ? ent_ranks - 1 : p);
}

// The rank whose row it is; -1 for an archetype that is not distributed,
// which every rank has.
int32_t ent_ranks_owner(int32_t archetype, int64_t row) {
  if (archetype >= ent_places_known || !ent_places[archetype].placed)
    return -1;
  if (ent_places[archetype].rows) {
    if (ent_places[archetype].first_only)
      return 0;
    int64_t p = row / ent_places[archetype].rows;
    return (int32_t)(p >= ent_ranks ? ent_ranks - 1 : p);
  }
  const int64_t *bounds = ent_places[archetype].bounds;
  int32_t lo = 0, hi = ent_ranks - 1;
  while (lo < hi) {
    int32_t mid = (lo + hi + 1) / 2;
    if (bounds[mid] <= row)
      lo = mid;
    else
      hi = mid - 1;
  }
  return lo;
}

// Placed by an entity whose archetype is not distributed: nothing to
// place it with.
int32_t ent_ranks_owner_of_home(int32_t archetype, int64_t row) {
  int32_t rank = ent_ranks_owner(archetype, row);
  if (rank < 0)
    ent_ranks_stop("an entity is distributed by an entity that every rank "
                   "has (not distributed, or not placed yet): there is "
                   "nothing to place it with");
  return rank;
}

// This rank's stretch of the archetype's rows, of `count` it has: all of
// them before it is placed.
int64_t ent_ranks_first(int32_t archetype) {
  ent_placement *place = ent_place_of(archetype);
  if (place->rows)
    return ent_partition_first(place, ent_rank);
  return place->placed ? place->bounds[ent_rank] : 0;
}
int64_t ent_ranks_end_row(int32_t archetype, int64_t count) {
  ent_placement *place = ent_place_of(archetype);
  if (place->rows)
    return ent_partition_end(place, ent_rank);
  return place->placed ? place->bounds[ent_rank + 1] : count;
}

// Whether the other ranks' rows of the placed archetype are yet to be let
// go (1, once).
int32_t ent_ranks_dropping(int32_t archetype) {
  ent_placement *place = ent_place_of(archetype);
  if (!place->placed || place->dropped)
    return 0;
  place->dropped = 1;
  return 1;
}

// (The whole pages from `from` to `to` given back to the system: read
// again they are noughts.)
static void ent_let_go(char *from, char *to) {
  uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
  uintptr_t start = ((uintptr_t)from + page - 1) & ~(page - 1);
  uintptr_t end = (uintptr_t)to & ~(page - 1);
  if (start < end)
    madvise((void *)start, end - start, MADV_DONTNEED);
}

// The rows of a column (`each` bytes a row, `capacity` rows) of the
// placed archetype that other ranks have, let go: the rank keeps only the
// memory of its own. (Their row numbers stay as they are.)
void ent_ranks_drop(int32_t archetype, void *column, int64_t each,
                    int64_t capacity) {
  ent_placement *place = ent_place_of(archetype);
  char *rows = column;
  ent_let_go(rows, rows + place->bounds[ent_rank] * each);
  ent_let_go(rows + place->bounds[ent_rank + 1] * each, rows + capacity * each);
}

// A row of a distributed archetype that another rank has is not read or
// written here: not on many ranks yet.
void ent_ranks_check_own(int32_t archetype, int64_t row, int32_t line) {
  int32_t rank = ent_ranks_owner(archetype, row);
  if (rank >= 0 && rank != ent_rank) {
    char why[256];
    snprintf(why, sizeof why,
             "rank %d reads or sends to an entity that rank %d has (row %lld "
             "of archetype %d, line %d): not on many ranks yet",
             ent_rank, rank, (long long)row, archetype, line);
    ent_ranks_stop(why);
  }
}

// ---------------------------------------------------------------------------
// What the ranks' parts of a `for` come to.

static int64_t ent_ranks_sum_untimed(int64_t mine) {
  if (!ent_distributed)
    return mine;
  int64_t all = 0;
  all = ent_allreduce(mine, MPI_SUM);
  return all;
}
static int64_t ent_ranks_min_untimed(int64_t mine) {
  if (!ent_distributed)
    return mine;
  int64_t all = 0;
  all = ent_allreduce(mine, MPI_MIN);
  return all;
}
static int64_t ent_ranks_max_untimed(int64_t mine) {
  if (!ent_distributed)
    return mine;
  int64_t all = 0;
  all = ent_allreduce(mine, MPI_MAX);
  return all;
}
// What the ranks before this one added (nought on rank 0).
static int64_t ent_ranks_before_untimed(int64_t mine) {
  if (!ent_distributed)
    return 0;
  int64_t before = 0;
  before = ent_exscan(mine);
  return ent_rank == 0 ? 0 : before;
}

// ---------------------------------------------------------------------------
// What a `for` over a distributed archetype writes to standard output: each
// rank writes what its rows write, the ranks after 0 into memory, which
// rank 0 then writes after its own, rank by rank: the order of the rows.

static _Thread_local FILE *ent_capturing = 0;
static _Thread_local char *ent_captured = 0;
static _Thread_local size_t ent_captured_size = 0;

// Where a partition writes what the console says (devices/console.c): what
// a `for` writes is kept in memory by every partition but rank 0 (see
// ent_ranks_capture).
FILE *ent_stdout(void) { return ent_capturing ? ent_capturing : stdout; }

// Which of the process's partitions calls (devices/sends.c, text.c keep
// what they keep for each).
int ent_partition_index(void) { return ent_local; }

void ent_ranks_capture(void) {
  fflush(stdout);
  if (ent_rank == 0)
    return;
  ent_capturing = open_memstream(&ent_captured, &ent_captured_size);
}

static void ent_ranks_release_untimed(void) {
  int64_t size = 0;
  if (ent_rank != 0) {
    fclose(ent_capturing);
    ent_capturing = 0;
    size = (int64_t)ent_captured_size;
  } else {
    fflush(stdout);
  }
  // (Before the placement every rank wrote all of it: rank 0's is it.)
  if (!ent_distributed) {
    free(ent_captured);
    ent_captured = 0;
    ent_captured_size = 0;
    return;
  }
  char *all = 0;
  int64_t *sizes = calloc(ent_ranks, sizeof(int64_t));
  ent_gather_root(ent_rank == 0 ? "" : ent_captured, ent_rank == 0 ? 0 : size,
                  &all, sizes);
  if (ent_rank == 0) {
    int64_t at = sizes[0];
    for (int r = 1; r < ent_ranks; ++r) {
      fwrite(all + at, 1, sizes[r], stdout);
      at += sizes[r];
    }
    fflush(stdout);
    free(all);
  }
  free(sizes);
  free(ent_captured);
  ent_captured = 0;
  ent_captured_size = 0;
}

// ---------------------------------------------------------------------------
// Rows of a buffer that a `for` over distributed entities wrote: each rank
// wrote its entities' rows, from the least to the most it wrote (nothing
// where the least is above the most), and gives them to every other rank,
// each field's into the same rows there. Two ranks' stretches that overlap
// would give rows another rank wrote: not on many ranks yet.

static _Thread_local int *ent_share_counts = 0, *ent_share_starts = 0;
static _Thread_local int64_t *ent_share_lows = 0, *ent_share_highs = 0;
static _Thread_local int ent_share_any = 0;

static void ent_ranks_share_begin_untimed(int64_t low, int64_t high) {
  ent_share_any = 0;
  if (!ent_distributed)
    return;
  int64_t mine[2] = {low, high};
  int64_t *all = malloc(sizeof(int64_t) * 2 * ent_ranks);
  ent_allgather(mine, 2 * sizeof(int64_t), all);
  ent_share_lows = realloc(ent_share_lows, sizeof(int64_t) * ent_ranks);
  ent_share_highs = realloc(ent_share_highs, sizeof(int64_t) * ent_ranks);
  ent_share_counts = realloc(ent_share_counts, sizeof(int) * ent_ranks);
  ent_share_starts = realloc(ent_share_starts, sizeof(int) * ent_ranks);
  ent_share_any = 0;
  for (int r = 0; r < ent_ranks; ++r) {
    ent_share_lows[r] = all[2 * r];
    ent_share_highs[r] = all[2 * r + 1];
    ent_share_any |= ent_share_lows[r] <= ent_share_highs[r];
  }
  free(all);
  for (int a = 0; a < ent_ranks; ++a)
    for (int b = a + 1; b < ent_ranks; ++b)
      if (ent_share_lows[a] <= ent_share_highs[a] &&
          ent_share_lows[b] <= ent_share_highs[b] &&
          ent_share_lows[a] <= ent_share_highs[b] &&
          ent_share_lows[b] <= ent_share_highs[a])
        ent_ranks_stop("two ranks' entities wrote rows of a buffer between "
                       "each other's (their windows or places are not "
                       "apart by rank): not on many ranks yet");
}

// One column of the buffer, `each` bytes a row, from `column`.
static void ent_ranks_share_column_untimed(void *column, int64_t each) {
  if (!ent_share_any)
    return;
  for (int r = 0; r < ent_ranks; ++r) {
    int64_t rows = ent_share_lows[r] <= ent_share_highs[r]
                       ? ent_share_highs[r] - ent_share_lows[r] + 1
                       : 0;
    int64_t bytes = rows * each, start = rows ? ent_share_lows[r] * each : 0;
    if (bytes > 0x7fffffff || start > 0x7fffffff)
      ent_ranks_stop("a rank wrote more than 2 GB of a buffer's column, or "
                     "rows past 2 GB: not on many ranks yet");
    ent_share_counts[r] = (int)bytes;
    ent_share_starts[r] = (int)start;
  }
  ent_allgatherv_ints(column, ent_share_counts, ent_share_starts);
}

// ---------------------------------------------------------------------------
// Sends of a `for` over distributed entities to entities another rank has
// (or every rank: of an archetype that is not distributed): each posted to
// those ranks, on a channel of the program's (one for each field and rule
// it combines into), and given to them when the `for` ends, where each is
// combined into the entity's field. Only whole numbers added, or kept the
// least or the most, which come to the same in any order.

typedef struct {
  int64_t *words; // channel, id, value
  int64_t count, room;
} ent_posts;

static _Thread_local ent_posts *ent_outbox = 0;
static _Thread_local int64_t *ent_inbox = 0;
static _Thread_local int64_t ent_inbox_count = 0;
// (For each channel, where its sends start in the inbox, sorted by it.)
static _Thread_local int64_t *ent_channel_starts = 0;
static _Thread_local int32_t ent_channels = 0;

static void ent_post_to(int32_t rank, int32_t channel, int64_t id,
                        int64_t value) {
  ++ent_posted;
  if (!ent_outbox)
    ent_outbox = calloc(ent_ranks, sizeof(ent_posts));
  ent_posts *box = &ent_outbox[rank];
  if (box->count + 3 > box->room) {
    box->room = box->room ? box->room * 2 : 3 * 1024;
    box->words = realloc(box->words, sizeof(int64_t) * box->room);
  }
  box->words[box->count++] = channel;
  box->words[box->count++] = id;
  box->words[box->count++] = value;
  if (channel >= ent_channels)
    ent_channels = channel + 1;
}

void ent_ranks_post(int32_t channel, int32_t archetype, int64_t row,
                    int64_t id, int64_t value, int32_t all) {
  // (Before the placement every rank sends all of it: to itself.)
  if (!ent_distributed) {
    ent_post_to(ent_rank, channel, id, value);
    return;
  }
  if (all) {
    for (int r = 0; r < ent_ranks; ++r)
      ent_post_to(r, channel, id, value);
    return;
  }
  ent_post_to(ent_ranks_owner(archetype, row), channel, id, value);
}

// Every rank's posts to every rank, sorted by channel (in the order of the
// ranks they came from, each's in its order).
static void ent_ranks_exchange_untimed(int32_t channels) {
  if (!ent_outbox)
    ent_outbox = calloc(ent_ranks, sizeof(ent_posts));
  int *sent = malloc(sizeof(int) * ent_ranks), *got = malloc(sizeof(int) * ent_ranks);
  int *sent_at = malloc(sizeof(int) * ent_ranks), *got_at = malloc(sizeof(int) * ent_ranks);
  int64_t total = 0;
  for (int r = 0; r < ent_ranks; ++r) {
    if (ent_outbox[r].count > 0x7fffffff || total > 0x7fffffff)
      ent_ranks_stop("a rank sent more than 2^31 words to the others in one "
                     "`for`: not on many ranks yet");
    sent[r] = (int)ent_outbox[r].count;
    total += sent[r];
  }
  int64_t *out = malloc(sizeof(int64_t) * (total > 0 ? total : 1));
  total = 0;
  for (int r = 0; r < ent_ranks; ++r) {
    sent_at[r] = (int)total;
    for (int64_t i = 0; i < ent_outbox[r].count; ++i)
      out[total++] = ent_outbox[r].words[i];
    ent_outbox[r].count = 0;
  }
  ent_alltoall_ints(sent, got);
  int64_t incoming = 0;
  for (int r = 0; r < ent_ranks; ++r) {
    got_at[r] = (int)incoming;
    incoming += got[r];
  }
  int64_t *in = malloc(sizeof(int64_t) * (incoming > 0 ? incoming : 1));
  ent_alltoallv_ints(out, sent, sent_at, in, got, got_at);
  free(out);
  free(sent);
  free(got);
  free(sent_at);
  free(got_at);
  // (By channel: counted, then put in place.)
  if (channels > ent_channels)
    ent_channels = channels;
  ent_channel_starts =
      realloc(ent_channel_starts, sizeof(int64_t) * (ent_channels + 1));
  for (int32_t c = 0; c <= ent_channels; ++c)
    ent_channel_starts[c] = 0;
  ent_inbox_count = incoming / 3;
  ent_received += ent_inbox_count;
  for (int64_t i = 0; i < ent_inbox_count; ++i)
    ++ent_channel_starts[in[3 * i] + 1];
  for (int32_t c = 0; c < ent_channels; ++c)
    ent_channel_starts[c + 1] += ent_channel_starts[c];
  int64_t *place = malloc(sizeof(int64_t) * (ent_channels + 1));
  for (int32_t c = 0; c <= ent_channels; ++c)
    place[c] = ent_channel_starts[c];
  ent_inbox = realloc(ent_inbox, sizeof(int64_t) * 2 * (ent_inbox_count + 1));
  for (int64_t i = 0; i < ent_inbox_count; ++i) {
    int64_t at = place[in[3 * i]]++;
    ent_inbox[2 * at] = in[3 * i + 1];
    ent_inbox[2 * at + 1] = in[3 * i + 2];
  }
  free(place);
  free(in);
}

// How many sends this rank was given on the channel, and the `i`th one's
// entity and value.
int64_t ent_ranks_got(int32_t channel) {
  if (channel >= ent_channels)
    return 0;
  return ent_channel_starts[channel + 1] - ent_channel_starts[channel];
}
int64_t ent_ranks_got_id(int32_t channel, int64_t i) {
  return ent_inbox[2 * (ent_channel_starts[channel] + i)];
}
int64_t ent_ranks_got_value(int32_t channel, int64_t i) {
  return ent_inbox[2 * (ent_channel_starts[channel] + i) + 1];
}

// A send that cannot go to the other ranks yet (a fraction, `=`, or to a
// field whose changes are events) to an entity they have too.
void ent_ranks_cannot_send(int32_t line) {
  char why[256];
  snprintf(why, sizeof why,
           "a `for` over distributed entities sends a value that is not a "
           "whole number added, or kept the least or the most, to an entity "
           "another rank has or every rank has (line %d): not on many ranks "
           "yet",
           line);
  ent_ranks_stop(why);
}

// ---------------------------------------------------------------------------
// A column of a placed archetype (`each` bytes a row) that a `for` over
// distributed entities looks up in entities of any rank: each rank's
// stretch given to every other rank, so that every rank has all of it (the
// ghosts of the other ranks' entities) until the `for` ends.
static void ent_ranks_gather_column_untimed(int32_t archetype, void *column, int64_t each) {
  if (!ent_distributed)
    return;
  ent_placement *place = ent_place_of(archetype);
  int *counts = malloc(sizeof(int) * ent_ranks);
  int *starts = malloc(sizeof(int) * ent_ranks);
  for (int r = 0; r < ent_ranks; ++r) {
    int64_t first = place->rows ? ent_partition_first(place, r)
                                : place->bounds[r];
    int64_t end = place->rows ? ent_partition_end(place, r)
                              : place->bounds[r + 1];
    int64_t start = first * each;
    int64_t bytes = (end - first) * each;
    if (start > 0x7fffffff || bytes > 0x7fffffff)
      ent_ranks_stop("a column looked up across ranks is past 2 GB: not on "
                     "many ranks yet");
    counts[r] = (int)bytes;
    starts[r] = (int)start;
  }
  ent_allgatherv_ints(column, counts, starts);
  free(counts);
  free(starts);
}

// A child of a tree that a `for` over distributed entities goes along
// (row `row` of `archetype`) and its parent (`row` of `home`): the
// same rank's, else the program stops when they are placed.
void ent_ranks_same_rank(int32_t archetype, int64_t row, int32_t home,
                         int64_t home_row) {
  int32_t child = ent_ranks_owner(archetype, row);
  int32_t parent = ent_ranks_owner(home, home_row);
  if (child != parent) {
    char why[256];
    snprintf(why, sizeof why,
             "a child of a tree (row %lld of archetype %d) is placed on rank "
             "%d, its parent (row %lld of archetype %d) on rank %d: a `for` "
             "along the tree needs them on one rank (distribute the child "
             "by its parent, or both by one number)",
             (long long)row, archetype, child, (long long)home_row, home,
             parent);
    ent_ranks_stop(why);
  }
}


// ---------------------------------------------------------------------------
// The calls the compiler makes, timed (see ent_spent).

void ent_ranks_bcast(void *data, int64_t bytes) {
  double start = MPI_Wtime();
  ent_ranks_bcast_untimed(data, bytes);
  double end = MPI_Wtime();
  ent_spent[0] += end - start;
  ent_note_place(__builtin_return_address(0), start, end);
  ++ent_calls[0];
}

int64_t ent_ranks_sum(int64_t mine) {
  double start = MPI_Wtime();
  int64_t result = ent_ranks_sum_untimed(mine);
  double end = MPI_Wtime();
  ent_spent[1] += end - start;
  ent_note_place(__builtin_return_address(0), start, end);
  ++ent_calls[1];
  return result;
}

int64_t ent_ranks_min(int64_t mine) {
  double start = MPI_Wtime();
  int64_t result = ent_ranks_min_untimed(mine);
  double end = MPI_Wtime();
  ent_spent[2] += end - start;
  ent_note_place(__builtin_return_address(0), start, end);
  ++ent_calls[2];
  return result;
}

int64_t ent_ranks_max(int64_t mine) {
  double start = MPI_Wtime();
  int64_t result = ent_ranks_max_untimed(mine);
  double end = MPI_Wtime();
  ent_spent[3] += end - start;
  ent_note_place(__builtin_return_address(0), start, end);
  ++ent_calls[3];
  return result;
}

int64_t ent_ranks_before(int64_t mine) {
  double start = MPI_Wtime();
  int64_t result = ent_ranks_before_untimed(mine);
  double end = MPI_Wtime();
  ent_spent[4] += end - start;
  ent_note_place(__builtin_return_address(0), start, end);
  ++ent_calls[4];
  return result;
}

void ent_ranks_release(void) {
  double start = MPI_Wtime();
  ent_ranks_release_untimed();
  double end = MPI_Wtime();
  ent_spent[5] += end - start;
  ent_note_place(__builtin_return_address(0), start, end);
  ++ent_calls[5];
}

void ent_ranks_share_begin(int64_t low, int64_t high) {
  double start = MPI_Wtime();
  ent_ranks_share_begin_untimed(low, high);
  double end = MPI_Wtime();
  ent_spent[6] += end - start;
  ent_note_place(__builtin_return_address(0), start, end);
  ++ent_calls[6];
}

void ent_ranks_share_column(void *column, int64_t each) {
  double start = MPI_Wtime();
  ent_ranks_share_column_untimed(column, each);
  double end = MPI_Wtime();
  ent_spent[7] += end - start;
  ent_note_place(__builtin_return_address(0), start, end);
  ++ent_calls[7];
}

void ent_ranks_exchange(int32_t channels) {
  double start = MPI_Wtime();
  ent_ranks_exchange_untimed(channels);
  double end = MPI_Wtime();
  ent_spent[8] += end - start;
  ent_note_place(__builtin_return_address(0), start, end);
  ++ent_calls[8];
}

void ent_ranks_gather_column(int32_t archetype, void *column, int64_t each) {
  double start = MPI_Wtime();
  ent_ranks_gather_column_untimed(archetype, column, each);
  double end = MPI_Wtime();
  ent_spent[9] += end - start;
  ent_note_place(__builtin_return_address(0), start, end);
  ++ent_calls[9];
}

// ---------------------------------------------------------------------------
// Edges, which live with their source's rank and, where the target is
// another rank's, with the target's too (every rank's where an end is an
// entity every rank has). What a `for` over distributed entities does to
// edges (connects them, lets them go, writes their fields) is sent to the
// ranks of the edge's two ends, as messages of a channel, each so many
// words; when the `for` ends every rank is given its messages, by channel
// and in the order of the ranks (that of the rows), so the copies of an
// edge stay alike and in one process's order.

typedef struct {
  int64_t *words; // channel, length, then the words, message by message
  int64_t count, room;
} ent_box;

static _Thread_local ent_box *ent_msg_out = 0;
static _Thread_local int *ent_msg_dest = 0;
static _Thread_local int ent_msg_dests = 0;
// (What was given: by channel, where each message's words start.)
static _Thread_local int64_t *ent_msg_in = 0;
static _Thread_local int64_t *ent_msg_starts = 0;  // by channel: where its list starts
static _Thread_local int64_t *ent_msg_list = 0;    // message word places, by channel
static _Thread_local int32_t ent_msg_channels = 0;
static _Thread_local int64_t ent_msg_given = 0;

static void ent_box_put(ent_box *box, int64_t word) {
  if (box->count == box->room) {
    box->room = box->room ? 2 * box->room : 4096;
    box->words = realloc(box->words, sizeof(int64_t) * box->room);
  }
  box->words[box->count++] = word;
}

// A message of `length` words on `channel` to the ranks of the rows
// `row_a` of archetype `a` and `row_b` of `b` (once where it is one rank;
// every rank where either is an entity every rank has, and before the
// placement this rank alone). Its words follow, one by one.
void ent_ranks_msg_ends(int32_t channel, int32_t length, int32_t a,
                        int64_t row_a, int32_t b, int64_t row_b) {
  if (!ent_msg_out) {
    ent_msg_out = calloc(ent_ranks, sizeof(ent_box));
    ent_msg_dest = malloc(sizeof(int) * ent_ranks);
  }
  if (channel >= ent_msg_channels)
    ent_msg_channels = channel + 1;
  ent_msg_dests = 0;
  if (!ent_distributed) {
    ent_msg_dest[ent_msg_dests++] = ent_rank;
  } else {
    int ra = ent_ranks_owner(a, row_a), rb = ent_ranks_owner(b, row_b);
    if (ra < 0 || rb < 0) {
      for (int r = 0; r < ent_ranks; ++r)
        ent_msg_dest[ent_msg_dests++] = r;
    } else {
      ent_msg_dest[ent_msg_dests++] = ra;
      if (rb != ra)
        ent_msg_dest[ent_msg_dests++] = rb;
    }
  }
  for (int d = 0; d < ent_msg_dests; ++d) {
    ent_box_put(&ent_msg_out[ent_msg_dest[d]], channel);
    ent_box_put(&ent_msg_out[ent_msg_dest[d]], length);
  }
}

void ent_ranks_msg_word(int64_t word) {
  for (int d = 0; d < ent_msg_dests; ++d)
    ent_box_put(&ent_msg_out[ent_msg_dest[d]], word);
}

// Every rank's messages to every rank; then by channel, in the order of
// the ranks they came from and each's in its order.
void ent_ranks_msg_exchange(void) {
  if (!ent_msg_out) {
    ent_msg_out = calloc(ent_ranks, sizeof(ent_box));
    ent_msg_dest = malloc(sizeof(int) * ent_ranks);
  }
  int *sent = malloc(sizeof(int) * ent_ranks), *got = malloc(sizeof(int) * ent_ranks);
  int *sent_at = malloc(sizeof(int) * ent_ranks), *got_at = malloc(sizeof(int) * ent_ranks);
  int64_t total = 0;
  for (int r = 0; r < ent_ranks; ++r) {
    if (total + ent_msg_out[r].count > 0x7fffffff)
      ent_ranks_stop("a rank sent more than 2^31 words of edges in one "
                     "`for`: not on many ranks yet");
    sent[r] = (int)ent_msg_out[r].count;
    sent_at[r] = (int)total;
    total += sent[r];
  }
  int64_t *out = malloc(sizeof(int64_t) * (total > 0 ? total : 1));
  for (int r = 0, at = 0; r < ent_ranks; ++r) {
    for (int64_t i = 0; i < ent_msg_out[r].count; ++i)
      out[at++] = ent_msg_out[r].words[i];
    free(ent_msg_out[r].words);
    ent_msg_out[r] = (ent_box){0, 0, 0};
  }
  int64_t incoming = 0;
  if (ent_distributed) {
    ent_alltoall_ints(sent, got);
    for (int r = 0; r < ent_ranks; ++r) {
      if (incoming > 0x7fffffff)
        ent_ranks_stop("a rank was given more than 2^31 words of edges in "
                       "one `for`: not on many ranks yet");
      got_at[r] = (int)incoming;
      incoming += got[r];
    }
    free(ent_msg_in);
    ent_msg_in = malloc(sizeof(int64_t) * (incoming > 0 ? incoming : 1));
    ent_alltoallv_ints(out, sent, sent_at, ent_msg_in, got, got_at);
    free(out);
  } else {
    // (Before the placement, all to this rank itself.)
    free(ent_msg_in);
    ent_msg_in = out;
    incoming = total;
  }
  free(sent);
  free(got);
  free(sent_at);
  free(got_at);
  // (By channel: counted, then their places listed.)
  ent_msg_starts =
      realloc(ent_msg_starts, sizeof(int64_t) * (ent_msg_channels + 1));
  for (int32_t c = 0; c <= ent_msg_channels; ++c)
    ent_msg_starts[c] = 0;
  int64_t messages = 0;
  for (int64_t at = 0; at < incoming; at += 2 + ent_msg_in[at + 1]) {
    ++ent_msg_starts[ent_msg_in[at] + 1];
    ++messages;
  }
  for (int32_t c = 0; c < ent_msg_channels; ++c)
    ent_msg_starts[c + 1] += ent_msg_starts[c];
  ent_msg_list = realloc(ent_msg_list, sizeof(int64_t) * (messages + 1));
  int64_t *next = malloc(sizeof(int64_t) * (ent_msg_channels + 1));
  for (int32_t c = 0; c <= ent_msg_channels; ++c)
    next[c] = ent_msg_starts[c];
  for (int64_t at = 0; at < incoming; at += 2 + ent_msg_in[at + 1])
    ent_msg_list[next[ent_msg_in[at]]++] = at + 2;
  free(next);
  ent_msg_given = messages;
}

// How many messages this rank was given on the channel, and word `word`
// of the `i`th.
int64_t ent_ranks_msg_count(int32_t channel) {
  if (channel >= ent_msg_channels || !ent_msg_starts)
    return 0;
  return ent_msg_starts[channel + 1] - ent_msg_starts[channel];
}
int64_t ent_ranks_msg_read(int32_t channel, int64_t i, int32_t word) {
  return ent_msg_in[ent_msg_list[ent_msg_starts[channel] + i] + word];
}

// Whether this rank keeps an edge from the row `row_a` of archetype `a`
// to `row_b` of `b`: where the source is its own (or every rank's) and
// its edges are visited from the source (`by_source`), or the target and
// from the target (`by_target`).
int32_t ent_ranks_keeps(int32_t a, int64_t row_a, int32_t b, int64_t row_b,
                        int32_t by_source, int32_t by_target) {
  if (!ent_distributed)
    return 1;
  int ra = ent_ranks_owner(a, row_a), rb = ent_ranks_owner(b, row_b);
  return (by_source && (ra < 0 || ra == ent_rank)) ||
         (by_target && (rb < 0 || rb == ent_rank));
}

// (ENT_RANKS_LOG: how many edges of a relation the rank kept when the
// entities were placed.)
void ent_ranks_log_edges(int32_t relation, int64_t had, int64_t kept) {
  if (getenv("ENT_RANKS_LOG"))
    fprintf(stderr, "ent: rank %d: relation %d: kept %lld of %lld edges\n",
            ent_rank, relation, (long long)kept, (long long)had);
}

// The rows `from` to `to` of a column (`each` bytes a row) given back to
// the system: rows a rank no longer has (edges it dropped).
void ent_ranks_drop_rows(void *column, int64_t each, int64_t from,
                         int64_t to) {
  if (from < to)
    ent_let_go((char *)column + from * each, (char *)column + to * each);
}

// Built for so many partitions, one a rank: run on as many ranks.
void ent_ranks_partitions(int64_t partitions) {
  if (partitions != ent_ranks) {
    char why[160];
    snprintf(why, sizeof why,
             "the program is built for %lld partitions, one a rank, and runs "
             "on %d ranks",
             (long long)partitions, ent_ranks);
    ent_ranks_stop(why);
  }
}

// ---------------------------------------------------------------------------
// Fractions added (or kept the least or the most) into a unique by a `for`
// over distributed entities come out other where added in another order:
// the ranks take turns, in their order. Before its rows a rank takes what
// the rank before it ended with; after them it gives its own to the next;
// the last one's is every rank's then. (Each rank's rows wait for the
// ranks' before it.)
void ent_ranks_chain_get(void *cell, int64_t bytes) {
  if (!ent_distributed || ent_rank == 0)
    return;
  ent_recv(cell, bytes, ent_rank - 1);
}
void ent_ranks_chain_put(void *cell, int64_t bytes) {
  if (!ent_distributed || ent_rank == ent_ranks - 1)
    return;
  ent_send(cell, bytes, ent_rank + 1);
}
void ent_ranks_chain_last(void *cell, int64_t bytes) {
  if (!ent_distributed)
    return;
  ent_bcast(cell, bytes, ent_ranks - 1);
}

// ---------------------------------------------------------------------------
// Built for partitions: a relation's edges sorted by the key of an end
// over the rank's own edges only (a stable radix sort), and the offsets of
// the keys of a window of them (see ownKeyWindows in the compiler), so
// that nothing as large as all the world's keys is gone through.
// ent_ranks_sort_begin(n), a key for each of the n (-1: the edge goes),
// ent_ranks_sort_run() (how many stay), then for each place the edge that
// goes there, and the offsets.

static _Thread_local uint64_t *ent_sort_keys = 0, *ent_sort_scratch = 0;
static _Thread_local int64_t *ent_sort_order = 0, *ent_sort_order_scratch = 0;
static _Thread_local int64_t ent_sort_n = 0, ent_sort_room = 0, ent_sort_kept = 0;

void ent_ranks_sort_begin(int64_t n) {
  if (n > ent_sort_room) {
    ent_sort_room = n;
    ent_sort_keys = realloc(ent_sort_keys, sizeof(uint64_t) * n);
    ent_sort_scratch = realloc(ent_sort_scratch, sizeof(uint64_t) * n);
    ent_sort_order = realloc(ent_sort_order, sizeof(int64_t) * n);
    ent_sort_order_scratch =
        realloc(ent_sort_order_scratch, sizeof(int64_t) * n);
  }
  ent_sort_n = n;
}
void ent_ranks_sort_key(int64_t i, int64_t key) {
  ent_sort_keys[i] = (uint64_t)key;
  ent_sort_order[i] = i;
}
int64_t ent_ranks_sort_run(void) {
  uint64_t most = 0;
  for (int64_t i = 0; i < ent_sort_n; ++i)
    if (ent_sort_keys[i] != UINT64_MAX && ent_sort_keys[i] > most)
      most = ent_sort_keys[i];
  // (Sixteen bits a pass, as many passes as the largest key needs; the
  // edges that go, keyed all ones, after all the rest.)
  int passes = 1;
  while (passes < 4 && (most >> (16 * passes)) != 0)
    ++passes;
  static _Thread_local int64_t counts[65537];
  for (int pass = 0; pass < passes; ++pass) {
    int shift = 16 * pass;
    for (int d = 0; d <= 65536; ++d)
      counts[d] = 0;
    for (int64_t i = 0; i < ent_sort_n; ++i) {
      uint64_t key = ent_sort_keys[i];
      int digit = key == UINT64_MAX ? 65536 : (int)((key >> shift) & 0xffff);
      ++counts[digit];
    }
    int64_t at = 0;
    for (int d = 0; d <= 65536; ++d) {
      int64_t c = counts[d];
      counts[d] = at;
      at += c;
    }
    for (int64_t i = 0; i < ent_sort_n; ++i) {
      uint64_t key = ent_sort_keys[i];
      int digit = key == UINT64_MAX ? 65536 : (int)((key >> shift) & 0xffff);
      int64_t to = counts[digit]++;
      ent_sort_scratch[to] = key;
      ent_sort_order_scratch[to] = ent_sort_order[i];
    }
    uint64_t *k = ent_sort_keys;
    ent_sort_keys = ent_sort_scratch;
    ent_sort_scratch = k;
    int64_t *o = ent_sort_order;
    ent_sort_order = ent_sort_order_scratch;
    ent_sort_order_scratch = o;
  }
  ent_sort_kept = ent_sort_n;
  while (ent_sort_kept > 0 && ent_sort_keys[ent_sort_kept - 1] == UINT64_MAX)
    --ent_sort_kept;
  return ent_sort_kept;
}
// The edge (its place before the sort) that goes to place `p`.
int64_t ent_ranks_sort_from(int64_t p) { return ent_sort_order[p]; }

// For each key from `from` to `to` (both), where its edges start among
// those sorted: offsets[key], `bits` wide (32 or 64).
void ent_ranks_sort_offsets(void *offsets, int32_t bits, int64_t from,
                            int64_t to) {
  int64_t at = 0;
  // (The first edge of a key at or after `from`.)
  int64_t lo = 0, hi = ent_sort_kept;
  while (lo < hi) {
    int64_t mid = lo + (hi - lo) / 2;
    if (ent_sort_keys[mid] < (uint64_t)from)
      lo = mid + 1;
    else
      hi = mid;
  }
  at = lo;
  for (int64_t key = from; key <= to; ++key) {
    while (at < ent_sort_kept && ent_sort_keys[at] < (uint64_t)key)
      ++at;
    if (bits == 64)
      ((int64_t *)offsets)[key] = at;
    else
      ((int32_t *)offsets)[key] = (int32_t)at;
  }
}

// Whether the row `row` of `archetype` is this rank's (or every rank's).
int32_t ent_ranks_owns(int32_t archetype, int64_t row) {
  if (!ent_distributed)
    return 1;
  int32_t owner = ent_ranks_owner(archetype, row);
  return owner < 0 || owner == ent_rank;
}

// The sort's own memory, given back where a relation is sorted.
void ent_ranks_sort_end(void) {
  free(ent_sort_keys);
  free(ent_sort_scratch);
  free(ent_sort_order);
  free(ent_sort_order_scratch);
  ent_sort_keys = ent_sort_scratch = 0;
  ent_sort_order = ent_sort_order_scratch = 0;
  ent_sort_room = ent_sort_n = ent_sort_kept = 0;
}

// ---------------------------------------------------------------------------
// Ghosts by request (built for partitions): a `for` over distributed
// entities that looks up entities of other ranks first goes through its
// rows asking (a copy of its body that does nothing but look up, see the
// compiler's askPass), on channels, one for each field looked up; each
// rank is then given what it asked of the ranks that have them, which
// read it from their own columns (registered for the channel and the
// archetype). The `for` then reads those (the ghosts). They are dropped
// when it ends.

typedef struct {
  char *column;  // the field's column, `each` bytes a row
  int64_t each;
  char *present; // an optional component's presence, or 0
} ent_ghost_column;

typedef struct {
  uint64_t key;  // channel << 56 | archetype << 48 | row, or 0 for none
  int64_t value;
  int32_t found;
  int32_t asked; // asked of the owner, not yet given
} ent_ghost;

static _Thread_local ent_ghost_column *ent_ghost_columns = 0; // [channel * 256 + archetype]
static _Thread_local int32_t ent_ghost_channels = 0;
static _Thread_local ent_ghost *ent_ghosts = 0;
static _Thread_local int64_t ent_ghost_room = 0, ent_ghost_used = 0;
static _Thread_local ent_box *ent_ask_out = 0;
static _Thread_local int64_t ent_ask_new = 0;

void ent_ranks_ghost_column(int32_t channel, int32_t archetype, void *column,
                            int64_t each, void *present) {
  if (channel >= ent_ghost_channels) {
    int32_t more = channel + 8;
    ent_ghost_columns =
        realloc(ent_ghost_columns, sizeof(ent_ghost_column) * 256 * more);
    for (int64_t i = 256 * (int64_t)ent_ghost_channels; i < 256 * (int64_t)more; ++i)
      ent_ghost_columns[i] = (ent_ghost_column){0, 0, 0};
    ent_ghost_channels = more;
  }
  ent_ghost_columns[channel * 256 + archetype] =
      (ent_ghost_column){column, each, present};
}

// (A lookup's: its channel and the archetype; a buffer's, archetype -1:
// the buffer and the field in the channel. Then the row, below 2^40.)
static uint64_t ent_ghost_key(int32_t channel, int32_t archetype, int64_t row) {
  uint64_t which = archetype < 0 ? (1ull << 23) | (uint64_t)channel
                                 : ((uint64_t)channel << 8) | (uint64_t)archetype;
  return ((which + 1) << 40) | (uint64_t)row;
}

static ent_ghost *ent_ghost_slot(uint64_t key, int make) {
  if (make && 2 * (ent_ghost_used + 1) > ent_ghost_room) {
    int64_t room = ent_ghost_room ? 2 * ent_ghost_room : 1 << 16;
    ent_ghost *old = ent_ghosts;
    int64_t old_room = ent_ghost_room;
    ent_ghosts = calloc(room, sizeof(ent_ghost));
    ent_ghost_room = room;
    ent_ghost_used = 0;
    for (int64_t i = 0; i < old_room; ++i)
      if (old[i].key) {
        ent_ghost *slot = ent_ghost_slot(old[i].key, 1);
        *slot = old[i];
        ++ent_ghost_used;
      }
    free(old);
  }
  if (!ent_ghost_room)
    return 0;
  uint64_t h = key * 0x9e3779b97f4a7c15ull;
  for (int64_t i = (int64_t)(h >> 20) & (ent_ghost_room - 1);;
       i = (i + 1) & (ent_ghost_room - 1)) {
    if (ent_ghosts[i].key == key)
      return &ent_ghosts[i];
    if (!ent_ghosts[i].key) {
      if (!make)
        return 0;
      ent_ghosts[i].key = key;
      ++ent_ghost_used;
      return &ent_ghosts[i];
    }
  }
}

static int32_t ent_buffer_owner(int32_t buffer, int64_t row);
static int64_t ent_buffer_local(int32_t buffer, int32_t field, int64_t row);

// In the asking pass: the field of row `row` of `archetype` (another
// rank's), asked for where it is not had yet; what is had of it so far
// (nothing yet: 0, not found). (Of a buffer, archetype -1: the channel
// is the buffer's and the field's, see ent_ranks_buffer_read.)
int64_t ent_ranks_ask(int32_t channel, int32_t archetype, int64_t row) {
  uint64_t key = ent_ghost_key(channel, archetype, row);
  ent_ghost *ghost = ent_ghost_slot(key, 0);
  if (ghost)
    return ghost->value;
  ghost = ent_ghost_slot(key, 1);
  ghost->value = 0;
  ghost->found = 0;
  ghost->asked = 1;
  if (!ent_ask_out)
    ent_ask_out = calloc(ent_ranks, sizeof(ent_box));
  ent_box *box =
      &ent_ask_out[archetype < 0 ? ent_buffer_owner(channel >> 6, row)
                                 : ent_ranks_owner(archetype, row)];
  ent_box_put(box, channel);
  ent_box_put(box, archetype);
  ent_box_put(box, row);
  ++ent_ask_new;
  return 0;
}
int32_t ent_ranks_ask_found(int32_t channel, int32_t archetype, int64_t row) {
  ent_ghost *ghost = ent_ghost_slot(ent_ghost_key(channel, archetype, row), 0);
  return ghost ? ghost->found : 0;
}

// What was asked, from the ranks that have it: how many were asked in
// all (over the ranks) this time; nought when the asking is done.
int64_t ent_ranks_fetch(void) {
  if (!ent_ask_out)
    ent_ask_out = calloc(ent_ranks, sizeof(ent_box));
  int64_t asked = ent_ask_new, all = 0;
  ent_ask_new = 0;
  ++ent_rounds;
  all = ent_allreduce(asked, MPI_SUM);
  if (all == 0)
    return 0;
  int *sent = malloc(sizeof(int) * ent_ranks), *got = malloc(sizeof(int) * ent_ranks);
  int *sent_at = malloc(sizeof(int) * ent_ranks), *got_at = malloc(sizeof(int) * ent_ranks);
  int64_t total = 0;
  for (int r = 0; r < ent_ranks; ++r) {
    sent[r] = (int)ent_ask_out[r].count;
    sent_at[r] = (int)total;
    total += sent[r];
  }
  int64_t *out = malloc(sizeof(int64_t) * (total ? total : 1));
  for (int r = 0, at = 0; r < ent_ranks; ++r) {
    for (int64_t i = 0; i < ent_ask_out[r].count; ++i)
      out[at++] = ent_ask_out[r].words[i];
    // (Given back: a round may ask for much, the next for little.)
    free(ent_ask_out[r].words);
    ent_ask_out[r] = (ent_box){0, 0, 0};
  }
  ent_alltoall_ints(sent, got);
  int64_t incoming = 0;
  for (int r = 0; r < ent_ranks; ++r) {
    got_at[r] = (int)incoming;
    incoming += got[r];
  }
  int64_t *in = malloc(sizeof(int64_t) * (incoming ? incoming : 1));
  ent_alltoallv_ints(out, sent, sent_at, in, got, got_at);
  // (The answers, in the places of the questions: value and found.)
  int64_t *answer = malloc(sizeof(int64_t) * (incoming ? incoming : 1));
  for (int64_t i = 0; i + 2 < incoming + 2 && i < incoming; i += 3) {
    int32_t channel = (int32_t)in[i], archetype = (int32_t)in[i + 1];
    int64_t row = in[i + 2];
    if (archetype < 0) {
      // (A buffer's row: this rank's own.)
      answer[i] = channel;
      answer[i + 1] = ent_buffer_local(channel >> 6, channel & 63, row);
      answer[i + 2] = 1;
      continue;
    }
    ent_ghost_column *column =
        channel < ent_ghost_channels
            ? &ent_ghost_columns[channel * 256 + archetype]
            : 0;
    ent_placement *place = ent_place_of(archetype);
    int found = column && column->column && place->rows &&
                row - (row / place->rows) * place->rows <
                    place->counts[row / place->rows];
    if (found && column->present)
      found = column->present[row] != 0;
    int64_t value = 0;
    if (found)
      memcpy(&value, column->column + row * column->each, column->each);
    answer[i] = channel;
    answer[i + 1] = value;
    answer[i + 2] = found;
  }
  int64_t *back = malloc(sizeof(int64_t) * (total ? total : 1));
  ent_alltoallv_ints(answer, got, got_at, back, sent, sent_at);
  for (int64_t i = 0; i < total; i += 3) {
    ent_ghost *ghost = ent_ghost_slot(
        ent_ghost_key((int32_t)out[i], (int32_t)out[i + 1], out[i + 2]), 0);
    if (ghost) {
      ghost->value = back[i + 1];
      ghost->found = (int32_t)back[i + 2];
      ghost->asked = 0;
    }
  }
  free(out);
  free(in);
  free(answer);
  free(back);
  free(sent);
  free(got);
  free(sent_at);
  free(got_at);
  return all;
}

// In the `for` itself: the ghost of the field of row `row` of `archetype`
// (another rank's), and whether its entity was found. One not asked for
// stops the program.
static ent_ghost *ent_ghost_had(int32_t channel, int32_t archetype,
                                int64_t row) {
  ent_ghost *ghost = ent_ghost_slot(ent_ghost_key(channel, archetype, row), 0);
  if (!ghost || ghost->asked) {
    char why[240];
    snprintf(why, sizeof why,
             "rank %d looks up an entity of rank %d (row %lld of archetype "
             "%d) that its asking did not come to (the `for` chose it by "
             "something it wrote first): not on many ranks yet",
             ent_rank, ent_ranks_owner(archetype, row), (long long)row,
             archetype);
    ent_ranks_stop(why);
  }
  return ghost;
}
int64_t ent_ranks_ghost(int32_t channel, int32_t archetype, int64_t row) {
  return ent_ghost_had(channel, archetype, row)->value;
}
int32_t ent_ranks_ghost_found(int32_t channel, int32_t archetype,
                              int64_t row) {
  return ent_ghost_had(channel, archetype, row)->found;
}
// The ghosts of a `for`, dropped when it ends.
void ent_ranks_ghosts_drop(void) {
  free(ent_ghosts);
  ent_ghosts = 0;
  ent_ghost_room = ent_ghost_used = 0;
}

// Whether lookups of other ranks' entities are asked for (ENT_GHOSTS=ask:
// what a rank holds of them is only what its rows look up, for worlds
// larger than a machine), or the fields looked up are gathered whole on
// every rank (faster where they fit).
int32_t ent_ranks_ghosts_ask(void) {
  static _Thread_local int ask = -1;
  if (ask < 0) {
    const char *how = getenv("ENT_GHOSTS");
    ask = how && !strcmp(how, "ask");
  }
  return ask;
}

// ---------------------------------------------------------------------------
// Buffers split by rows (built for partitions, with ENT_GHOSTS=ask): a
// buffer that `for`s over distributed entities write, and only such `for`s
// read, keeps on each rank only the rows it has: those its entities wrote
// (its own), those every rank gave it alike outside a `for` (local), and
// runs of rows all alike, given outside a `for` (a fill), which are not
// written at all. Rows other ranks' entities wrote are theirs, asked for
// like a lookup. A list of stretches of rows says which is which.

enum { ENT_ROWS_LOCAL = 0, ENT_ROWS_RUN = 1, ENT_ROWS_REMOTE = 2 };

typedef struct {
  int64_t from, to; // [from, to)
  int kind;
  int rank;         // of a REMOTE stretch
  int64_t *run;     // of a RUN: the value of each field (bits)
} ent_rows;

typedef struct {
  int registered;
  int fields;
  char *columns[64];
  int64_t each[64];
  int64_t capacity;
  ent_rows *rows;
  int64_t count, room;
  // (A row being given outside a `for`: its values, field by field.)
  int64_t pending_row;
  int64_t pending[64];
} ent_split;

static _Thread_local ent_split *ent_splits = 0;
static _Thread_local int32_t ent_splits_known = 0;
static _Thread_local int ent_asking = 0;

static ent_split *ent_split_of(int32_t buffer) {
  if (buffer >= ent_splits_known) {
    int32_t more = buffer + 8;
    ent_splits = realloc(ent_splits, sizeof(ent_split) * more);
    memset(ent_splits + ent_splits_known, 0,
           sizeof(ent_split) * (more - ent_splits_known));
    ent_splits_known = more;
  }
  return &ent_splits[buffer];
}

void ent_ranks_buffer_column(int32_t buffer, int32_t field, void *column,
                             int64_t each, int64_t capacity) {
  ent_split *split = ent_split_of(buffer);
  split->registered = 1;
  if (field >= split->fields)
    split->fields = field + 1;
  split->columns[field] = column;
  split->each[field] = each;
  split->capacity = capacity;
}

// (The stretch row `row` is in, or -1: no stretch says, so it is local.)
static int64_t ent_rows_at(ent_split *split, int64_t row) {
  int64_t lo = 0, hi = split->count;
  while (lo < hi) {
    int64_t mid = lo + (hi - lo) / 2;
    if (split->rows[mid].to <= row)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < split->count && split->rows[lo].from <= row ? lo : -1;
}

// The rows [from, to) made `kind` (of `rank`, or a run of `run`): the
// stretches they were in cut around them.
static void ent_rows_set(ent_split *split, int64_t from, int64_t to, int kind,
                         int rank, const int64_t *run) {
  if (from >= to)
    return;
  ent_rows *kept = malloc(sizeof(ent_rows) * (split->count + 3));
  int64_t n = 0;
  int placed = 0;
  for (int64_t i = 0; i < split->count; ++i) {
    ent_rows r = split->rows[i];
    if (r.to <= from || r.from >= to) {
      if (!placed && r.from >= to) {
        kept[n++] = (ent_rows){from, to, kind, rank, 0};
        placed = 1;
      }
      kept[n++] = r;
      continue;
    }
    if (r.from < from) {
      ent_rows left = r;
      left.to = from;
      if (left.run) {
        left.run = malloc(sizeof(int64_t) * split->fields);
        memcpy(left.run, r.run, sizeof(int64_t) * split->fields);
      }
      kept[n++] = left;
    }
    if (!placed) {
      kept[n++] = (ent_rows){from, to, kind, rank, 0};
      placed = 1;
    }
    if (r.to > to) {
      ent_rows right = r;
      right.from = to;
      if (right.run) {
        right.run = malloc(sizeof(int64_t) * split->fields);
        memcpy(right.run, r.run, sizeof(int64_t) * split->fields);
      }
      kept[n++] = right;
    }
    free(r.run);
  }
  if (!placed)
    kept[n++] = (ent_rows){from, to, kind, rank, 0};
  for (int64_t i = 0; i < n; ++i)
    if (kept[i].from == from && kept[i].to == to && kept[i].kind == kind &&
        kept[i].run == 0 && run) {
      kept[i].run = malloc(sizeof(int64_t) * split->fields);
      memcpy(kept[i].run, run, sizeof(int64_t) * split->fields);
    }
  free(split->rows);
  split->rows = kept;
  split->count = n;
  split->room = n;
  if (n > ent_most_stretches)
    ent_most_stretches = n;
}

static int32_t ent_buffer_owner(int32_t buffer, int64_t row) {
  ent_split *split = ent_split_of(buffer);
  int64_t at = ent_rows_at(split, row);
  return at >= 0 && split->rows[at].kind == ENT_ROWS_REMOTE
             ? split->rows[at].rank
             : ent_rank;
}

static int64_t ent_buffer_local(int32_t buffer, int32_t field, int64_t row) {
  ent_split *split = ent_split_of(buffer);
  int64_t at = ent_rows_at(split, row);
  if (at >= 0 && split->rows[at].kind == ENT_ROWS_RUN)
    return split->rows[at].run[field];
  int64_t value = 0;
  memcpy(&value, split->columns[field] + row * split->each[field],
         split->each[field]);
  return value;
}

// The asking copy of a `for` goes through its rows (1), or not (0).
void ent_ranks_asking(int32_t asking) { ent_asking = asking; }

// A field of a row of a split buffer, where the buffer has the row: its
// own, a run's, or another rank's: asked for (in the asking copy), or
// what was given.
int64_t ent_ranks_buffer_read(int32_t buffer, int32_t field, int64_t row) {
  ent_split *split = ent_split_of(buffer);
  int64_t at = ent_rows_at(split, row);
  if (at < 0 || split->rows[at].kind != ENT_ROWS_REMOTE)
    return ent_buffer_local(buffer, field, row);
  int32_t channel = buffer << 6 | field;
  if (ent_asking) {
    ++ent_asked_rows;
    return ent_ranks_ask(channel, -1, row);
  }
  return ent_ranks_ghost(channel, -1, row);
}

// Outside a `for`: a row given (`row`, its values field by field, then
// done), which every rank gives alike: a run where it is like the last.
void ent_ranks_buffer_row(int32_t buffer, int64_t row) {
  ent_split_of(buffer)->pending_row = row;
}
void ent_ranks_buffer_value(int32_t buffer, int32_t field, int64_t bits) {
  ent_split_of(buffer)->pending[field] = bits;
}
void ent_ranks_buffer_given(int32_t buffer) {
  ent_split *split = ent_split_of(buffer);
  int64_t row = split->pending_row;
  ent_rows *last = split->count ? &split->rows[split->count - 1] : 0;
  if (last && last->kind == ENT_ROWS_RUN && last->to == row &&
      !memcmp(last->run, split->pending, sizeof(int64_t) * split->fields)) {
    last->to = row + 1;
    return;
  }
  ent_rows_set(split, row, row + 1, ENT_ROWS_RUN, 0, split->pending);
}
// Outside a `for`: `n` rows all alike (values given as above), in place of
// those it had.
void ent_ranks_buffer_filled(int32_t buffer, int64_t n) {
  ent_split *split = ent_split_of(buffer);
  for (int64_t i = 0; i < split->count; ++i)
    free(split->rows[i].run);
  split->count = 0;
  ent_rows_set(split, 0, n, ENT_ROWS_RUN, 0, split->pending);
}
// Outside a `for`: only the first `n` rows kept (0: none).
void ent_ranks_buffer_kept(int32_t buffer, int64_t n) {
  ent_split *split = ent_split_of(buffer);
  if (n < 0)
    n = 0;
  int64_t end = split->count ? split->rows[split->count - 1].to : 0;
  if (end > n)
    ent_rows_set(split, n, end, ENT_ROWS_LOCAL, 0, 0);
}
// Outside a `for`: row `row` written, alike on every rank: local. One of
// another rank's: not yet.
void ent_ranks_buffer_wrote(int32_t buffer, int64_t row) {
  ent_split *split = ent_split_of(buffer);
  int64_t at = ent_rows_at(split, row);
  if (at < 0)
    return;
  if (split->rows[at].kind == ENT_ROWS_REMOTE)
    ent_ranks_stop("a row of a buffer that another rank's entities wrote is "
                   "written outside a `for`: not on many ranks yet");
  if (split->rows[at].kind == ENT_ROWS_RUN) {
    // (Its run's values first, the field being written then given.)
    int64_t *run = split->rows[at].run;
    for (int f = 0; f < split->fields; ++f)
      memcpy(split->columns[f] + row * split->each[f], &run[f],
             split->each[f]);
    ent_rows_set(split, row, row + 1, ENT_ROWS_LOCAL, 0, 0);
  }
}

// After a `for` over distributed entities wrote rows of the split buffer
// (its stretch of each rank, from ent_ranks_share_begin): those of other
// ranks theirs, and the memory of them given back; its own local. The
// rows are not given to the other ranks.
static void ent_retaken_check(int32_t buffer, ent_split *split, int64_t from,
                              int64_t to);
static void ent_retaken_forget(int32_t buffer);

int32_t ent_ranks_ghosts_ask(void);

void ent_ranks_buffer_owned(int32_t buffer) {
  ent_split *split = ent_split_of(buffer);
  if (!split->registered || !ent_ranks_ghosts_ask() || !ent_distributed)
    return;
  if (ent_share_lows[ent_rank] <= ent_share_highs[ent_rank])
    ent_retaken_check(buffer, split, ent_share_lows[ent_rank],
                      ent_share_highs[ent_rank] + 1);
  ent_retaken_forget(buffer);
  for (int r = 0; r < ent_ranks; ++r) {
    int64_t from = ent_share_lows[r], to = ent_share_highs[r] + 1;
    if (from >= to)
      continue;
    ent_rows_set(split, from, to,
                 r == ent_rank ? ENT_ROWS_LOCAL : ENT_ROWS_REMOTE, r, 0);
    if (r != ent_rank)
      for (int f = 0; f < split->fields; ++f)
        ent_let_go(split->columns[f] + from * split->each[f],
                   split->columns[f] + to * split->each[f]);
  }
  ent_share_any = 0;
}

// In a `for` over distributed entities, before this rank's entity writes
// field `field` of row `row` of a split buffer: a row of a run is given
// its run's values first (rows about it, a page's worth, at once); one
// that was another rank's is noted, field by field, and by the end of the
// `for` every field of it must have been written (its others are not
// here), else the program stops.
static _Thread_local int64_t *ent_retaken = 0; // row, fields written (bits)
static _Thread_local int64_t ent_retaken_count = 0, ent_retaken_room = 0;

void ent_ranks_buffer_write(int32_t buffer, int32_t field, int64_t row) {
  ent_split *split = ent_split_of(buffer);
  int64_t at = ent_rows_at(split, row);
  if (at < 0)
    return;
  ent_rows *rows = &split->rows[at];
  if (rows->kind == ENT_ROWS_RUN) {
    int64_t from = row & ~(int64_t)4095, to = from + 4096;
    if (from < rows->from)
      from = rows->from;
    if (to > rows->to)
      to = rows->to;
    int64_t *run = malloc(sizeof(int64_t) * split->fields);
    memcpy(run, rows->run, sizeof(int64_t) * split->fields);
    for (int64_t r = from; r < to; ++r)
      for (int f = 0; f < split->fields; ++f)
        memcpy(split->columns[f] + r * split->each[f], &run[f],
               split->each[f]);
    free(run);
    ent_rows_set(split, from, to, ENT_ROWS_LOCAL, 0, 0);
    return;
  }
  if (rows->kind != ENT_ROWS_REMOTE)
    return;
  for (int64_t i = ent_retaken_count - 2; i >= 0 && i >= ent_retaken_count - 64;
       i -= 2)
    if (ent_retaken[i] == (buffer * (1ll << 40)) + row) {
      ent_retaken[i + 1] |= 1ll << field;
      return;
    }
  if (ent_retaken_count + 2 > ent_retaken_room) {
    ent_retaken_room = ent_retaken_room ? 2 * ent_retaken_room : 4096;
    ent_retaken = realloc(ent_retaken, sizeof(int64_t) * ent_retaken_room);
  }
  ent_retaken[ent_retaken_count++] = buffer * (1ll << 40) + row;
  ent_retaken[ent_retaken_count++] = 1ll << field;
}

static int ent_by_row(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
  return x < y ? -1 : x > y;
}

// (At the end of the `for`: the rows of `buffer` from `from` to `to` that
// were another rank's, all written, every field.)
static void ent_retaken_check(int32_t buffer, ent_split *split, int64_t from,
                              int64_t to) {
  // (Merged by row: a row's fields may be noted apart.)
  qsort(ent_retaken, ent_retaken_count / 2, 2 * sizeof(int64_t), ent_by_row);
  int64_t full = (split->fields >= 63 ? -1 : (1ll << split->fields) - 1);
  for (int64_t at = 0; at < split->count; ++at) {
    ent_rows *rows = &split->rows[at];
    if (rows->kind != ENT_ROWS_REMOTE || rows->to <= from || rows->from >= to)
      continue;
    int64_t lo = rows->from > from ? rows->from : from;
    int64_t hi = rows->to < to ? rows->to : to;
    for (int64_t row = lo; row < hi; ++row) {
      int64_t key = buffer * (1ll << 40) + row, mask = 0;
      int64_t a = 0, b = ent_retaken_count / 2;
      while (a < b) {
        int64_t m = (a + b) / 2;
        if (ent_retaken[2 * m] < key)
          a = m + 1;
        else
          b = m;
      }
      for (int64_t i = a; i < ent_retaken_count / 2 && ent_retaken[2 * i] == key;
           ++i)
        mask |= ent_retaken[2 * i + 1];
      if ((mask & full) != full)
        ent_ranks_stop("an entity wrote some fields of a buffer's row that "
                       "another rank's entity wrote before, not all (the "
                       "others are not here, with ENT_GHOSTS=ask): not on "
                       "many ranks yet");
    }
  }
}

// (The rows of `buffer` noted, forgotten: its `for` is done with them.)
static void ent_retaken_forget(int32_t buffer) {
  int64_t kept = 0;
  for (int64_t i = 0; i < ent_retaken_count; i += 2)
    if (ent_retaken[i] / (1ll << 40) != buffer) {
      ent_retaken[kept] = ent_retaken[i];
      ent_retaken[kept + 1] = ent_retaken[i + 1];
      kept += 2;
    }
  ent_retaken_count = kept;
}
