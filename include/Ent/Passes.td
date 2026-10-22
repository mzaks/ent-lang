#ifndef ENT_PASSES
#define ENT_PASSES

include "mlir/Pass/PassBase.td"

def EntLowerToLoops : Pass<"ent-lower-to-loops", "ModuleOp"> {
  let summary = "Lower systems and schedules to functions over column arrays";
  let description = [{
    Replaces the ECS program by plain functions over the world's storage.

    The world is one arena whose layout is fixed at compile time from the
    archetypes' components and capacities (see `WorldLayout`): one i64
    entity count per archetype, then one column per field at a static
    offset. Every lowered system and schedule takes its own parameters
    followed by the arena (`memref<Nxi8>`), and reads columns through
    statically shaped `memref.view`s. `ent-translate --ent-to-c-header`
    emits the matching C API for hosts.

    - `ent.system` becomes a private `func.func`. Each `ent.query` becomes
      one loop over every archetype that contains all of the query's
      components; `ent.get` and `ent.set` become `memref.load` and
      `memref.store` on the matched archetype's column.
    - With `parallel-entities`, a query whose body only computes and
      touches its own entity may become an `scf.parallel`, ready for
      `--convert-scf-to-openmp`. An archetype whose capacity is below
      `parallel-min-entities` always gets a sequential loop; otherwise the
      count is checked at run time and the parallel loop runs only from
      that many entities on. Whole numbers accumulated into a resource
      that the query reads nowhere are the parallel loop's reductions. A
      query that sends from a loop of its body gets a parallel loop over
      256 slices of its rows, each one after another, and calls of
      `ent_sends_put` (devices/sends.c) for what it sends. Edges
      connected in a loop of the body or in `ent.edges` go the same way,
      in any loop.
    - With `fuse-systems`, every maximal sequence of runs of systems
      without other effects is inlined into the schedule as one loop per
      archetype, holding every matching query body in program order.
      Stages are dissolved first; unused system functions remain for
      `--symbol-dce`.
    - `ent.schedule` becomes a public `func.func` with
      `llvm.emit_c_interface`; each `ent.run` becomes a `func.call`.
    - `ent.stage` is dissolved into its calls, or, with
      `parallel-stages`, a stage of two or more runs becomes an
      `omp.parallel` region with one `omp.section` per run.
    - With `gpu`, the queries of `gpu` systems are for the GPU
      (docs/gpu.md): each becomes an `scf.parallel` whatever its count,
      tiled by 256 and mapped to blocks and threads, for
      `--convert-parallel-loops-to-gpu`, over the GPU's copy of the
      world (`ent_gpu_arena`); fusion keeps them apart from the CPU's
      loops. What a run touches of the pieces both keep is copied to
      where it runs before it (`ent_gpu_to_device`, `ent_gpu_to_host`),
      what it writes is newer there after it (`ent_gpu_wrote`), and a
      schedule waits for the GPU when it ends (devices/gpu.c).
    - Components and archetypes are erased.
  }];
  let options = [
    Option<"parallelStages", "parallel-stages", "bool", /*default=*/"false",
           "Run the systems of a stage in parallel with OpenMP sections">,
    Option<"parallelEntities", "parallel-entities", "bool",
           /*default=*/"false",
           "Emit entity loops as scf.parallel where the body is entity-local">,
    Option<"parallelMinEntities", "parallel-min-entities", "int64_t",
           /*default=*/"1000000",
           "Entity count from which a parallel entity loop runs; on an M4 "
           "Max with default libomp the fused example pays off between 1e5 "
           "and 1e6 entities">,
    Option<"parallelMinEvents", "parallel-min-events", "int64_t",
           /*default=*/"16384",
           "Pending event-log entries from which a reactive query walks its "
           "logs' segments in parallel (with parallel-entities). Values sent "
           "from loops of a query's body may be combined in parallel, by "
           "groups of their targets, from as many on, where that has taken "
           "less (first from eight times as many); edges connected there are "
           "added in parallel from a sixteenth as many">,
    Option<"parallelMinLevel", "parallel-min-level", "int64_t",
           /*default=*/"32768",
           "Entities of one depth of a sorted tree from which a cascading "
           "query visits the depth in parallel (with parallel-entities)">,
    Option<"fuseSystems", "fuse-systems", "bool", /*default=*/"false",
           "Inline the systems of each schedule and fuse their queries into "
           "one loop per archetype">,
    Option<"directApplies", "direct-applies", "bool", /*default=*/"true",
           "Combine an apply whose field nothing else in its query touches "
           "as the loop visits the entities, where the loop never runs in "
           "parallel, instead of buffering it until the query ends (same "
           "result)">,
    Option<"gpu", "gpu", "bool", /*default=*/"false",
           "Run the queries of `gpu` systems on the GPU: as mapped "
           "scf.parallel loops, with the CPU waiting for them where it "
           "needs what they touch">,
    Option<"ranks", "ranks", "bool", /*default=*/"false",
           "Build for many ranks (MPI processes, devices/ranks.c): MPI "
           "started and ended around main, and what a device (an extern "
           "proc) does done by rank 0, what it gives back given every "
           "rank">,
    Option<"partitions", "partitions", "int64_t", /*default=*/"0",
           "With ranks: build for so many partitions (one per rank), each "
           "with rows of its own of the declared distributed archetypes, "
           "into which entities are spawned (placement at spawn); 0 for the "
           "setup run whole by every rank and placement where main's first "
           "loop starts">,
    Option<"explain", "explain", "bool", /*default=*/"false",
           "Emit remarks on lowering decisions, such as reactive queries "
           "that scan every entity instead of walking event logs">,
    Option<"explainStateAccess", "explain-state-access", "bool",
           /*default=*/"false",
           "Emit a remark for each state and access op (ent.sa) where it "
           "is lowered: how it was resolved, and the facts it was "
           "resolved from">,
    Option<"tune", "tune", "std::string", /*default=*/"\"\"",
           "A JSON file of choices for loops over entities, by their sites "
           "(`tools/ent build --tune`, from a trace): whether each runs on "
           "many threads, and whether it deals its pieces out as threads "
           "are free (`schedule`), in place of the rules that guess them">,
  ];
  let dependentDialects = [
    "::mlir::arith::ArithDialect",
    "::mlir::cf::ControlFlowDialect",
    "::mlir::func::FuncDialect",
    "::mlir::gpu::GPUDialect",
    "::mlir::LLVM::LLVMDialect",
    "::mlir::memref::MemRefDialect",
    "::mlir::omp::OpenMPDialect",
    "::mlir::scf::SCFDialect",
  ];
}

def EntInferArchetypes : Pass<"ent-infer-archetypes", "ModuleOp"> {
  let summary = "Derive the archetypes that spawns by components need";
  let description = [{
    Gives every `ent.spawn (@A, @B)(...)` an archetype and adds the
    archetypes this needs, marked `inferred` (see `inferArchetypes`): one per
    distinct set of spawned components, holding as optional the components
    adds and removes reach, sized by the smallest capacity among its
    required components. For a component declared `apart` it adds the
    archetypes entities move to as they are given it and lose it. The
    other passes and `ent-translate` run it
    themselves; on its own it shows what they infer.
  }];
}

def EntGpuKernelNames : Pass<"ent-gpu-kernel-names", "ModuleOp"> {
  let summary = "Name kernels as PTX takes names";
  let description = [{
    `--gpu-kernel-outlining` names a kernel after the function it comes
    from, which for a system of a module is `module.system`; PTX takes no
    `.` in a name, so the driver refuses the kernels. This replaces each
    `.` of a kernel's name by `_`, in the kernel and its launches. Run it
    after the outlining.

    It also makes each `math.sqrt` of a kernel NVVM's own root rounded to
    the nearest (`llvm.nvvm.sqrt.rn.f` and `.d`): the lowering to NVVM
    would call libdevice, whose f32 root is an approximate one unless the
    module asks otherwise. So a root rounds as on the CPU. Each
    `math.fma` becomes LLVM's own (`fma.rn`), not a call of libdevice's.
  }];
  let dependentDialects = ["::mlir::gpu::GPUDialect",
                           "::mlir::LLVM::LLVMDialect"];
}

def EntOmpNowait : Pass<"ent-omp-nowait"> {
  let summary = "Drop the barrier after a parallel region's last loop";
  let description = [{
    A work-sharing loop (`omp.wsloop`) or `omp.sections` ends with a
    barrier unless it is `nowait`; so does every `omp.parallel` region.
    `--convert-scf-to-openmp` turns an `scf.parallel` into a parallel
    region holding nothing but a work-sharing loop, which then waits at two
    barriers back to back, where clang emits one for `parallel for`. With
    the OpenMP runtime's defaults, where waiting threads sleep, the second
    barrier costs about as much as the fork itself (see bench/RESULTS.md).

    This pass marks a work-sharing loop or sections construct `nowait`
    when it is the last op of its parallel region, since the region's own
    barrier follows. Constructs with reductions or private values are left
    alone. Run it after `--convert-scf-to-openmp`.

    It also takes the body of a parallel loop out of the
    `memref.alloca_scope` that conversion puts it in, where the body puts
    nothing on the stack: a body with loops of its own cannot stay in a
    scope when those become blocks, and the canonicalizer keeps the scope
    wherever the body calls a function.

    And a work-sharing loop over at most 1024 pieces (a constant count:
    a query's slices) whose body has a loop inside a loop (rows with loops
    of their own, whose time differs from piece to piece) is scheduled
    `dynamic`, a piece at a time to whichever thread is free, instead of
    fixed shares; other loops keep fixed shares, and with them each
    thread's rows in its cache. On a thread for each core of two speeds
    (4 fast, 8 slow): the epidemic at a million people 12.7 -> 10.9 ms a
    day, the LJ melt of half lists 0.203 -> 0.196 s.
  }];
  let dependentDialects = ["::mlir::omp::OpenMPDialect"];
}

def EntTrace : Pass<"ent-trace", "ModuleOp"> {
  let summary = "Time systems, schedules, sorts and parallel loops as they run";
  let description = [{
    For `ent build --trace`: calls devices/trace.c, which keeps what was
    timed on each thread and, when the program ends, writes it as a trace
    (Chrome's JSON, for ui.perfetto.dev) and says where the time went.

    - A function lowered from a system or a schedule, or the sort of a
      relation (`ent.trace` on it), is timed from its start to each
      return: `ent_trace_begin` and `ent_trace_end` with its site.
    - An `omp.parallel` region is timed where it is entered and left
      (`ent_trace_loop_begin`, `ent_trace_loop_end`), and inside it each
      thread's own time, from its start to the region's end
      (`ent_trace_now`, `ent_trace_work`): so how long the threads worked
      of the time the loop took, at two calls a thread.
    - `main` starts the trace and names each site (`ent_trace_name`): the
      function, and the .ent file and line of the region.

    Run it after `--ent-omp-nowait`, before `--convert-scf-to-cf`.
  }];
  let dependentDialects = ["::mlir::LLVM::LLVMDialect",
                           "::mlir::func::FuncDialect"];
}

def EntLowerViews : Pass<"ent-lower-views", "::mlir::func::FuncOp"> {
  let summary = "Read and write the world's columns by their addresses";
  let description = [{
    After `--ent-lower-to-loops` a column is a `memref.view` of the world's
    memory, and what is read and written of it a `memref.load` or
    `memref.store` through that view. A view is a memref of its own: a
    record of five values once it is lowered, made and taken apart for
    every column a function touches, which is most of what a program with
    many archetypes is lowered to.

    This pass gives each such load and store the address it needs instead
    (`llvm.getelementptr` from the memory's own address, `llvm.load`,
    `llvm.store`), and drops the views nothing else uses. It changes
    nothing in what the program does; run it last before the conversion
    to the LLVM dialect.
  }];
  let dependentDialects = [
    "::mlir::LLVM::LLVMDialect", "::mlir::arith::ArithDialect",
    "::mlir::memref::MemRefDialect"
  ];
}

def EntPrintAccess : Pass<"ent-print-access", "ModuleOp"> {
  let summary = "Report the columns each system reads and writes";
  let description = [{
    Emits one remark per system listing the columns
    (`Archetype.Component.field`) it reads and writes, as derived from its
    `ent.get` and `ent.set` ops, and points at the op that makes a system
    opaque, if any. Meant for tests and for inspecting the analysis that
    `--ent-schedule` relies on.
  }];
}

def EntSchedule : Pass<"ent-schedule", "ModuleOp"> {
  let summary = "Group the runs of each schedule into parallel stages";
  let description = [{
    Two runs conflict if one writes a column the other reads or writes, or
    if either system has effects outside component access. Each run is
    placed in the earliest stage after every earlier run it conflicts with,
    so runs within a stage commute and program order between conflicting
    runs is kept.

    An op in the schedule body with memory effects acts as a barrier:
    runs before it are staged before it, runs after it after it. Ops
    without effects stay where they are. Schedules that already contain
    stages are left unchanged.
  }];
  let options = [
    Option<"explain", "explain", "bool", /*default=*/"false",
           "Emit a remark on each run that has to wait, naming the run it "
           "waits for and the conflict">,
  ];
}

def EntIndexes : Pass<"ent-indexes", "ModuleOp"> {
  let summary = "Keep each index up to date before what reads it";
  let description = [{
    An index (a buffer with an `ent.index` attribute, see `index` in
    docs/syntax.md) holds the entities that have its components in the
    order of a key, with fields copied from them. Its rows are made by a
    system the index declares (`update`), which also sets a unique
    (`fresh`); others (`stale`, `moved`) clear it.

    In every schedule, a run of `update` on the condition that the index
    is not fresh (and that of the run it goes before, if it has one)
    goes before each run of a system that reads the index
    (its rows, or the field each entity keeps its row in); a run of
    `moved` after each run of a system that changes which entities have
    its components (spawns, despawns, moves), and of `stale` after each
    one that writes only the key or the fields it copies. After `stale`
    the update writes each entity's values into its own row and puts the
    rows in order again; after `moved` it makes the index anew. An extern
    system that declares no access reads it and moves. So the index is made again, once, before the first
    read after a change, and between systems: what reads it sees it as it
    is when its system starts.
  }];
}

#endif // ENT_PASSES
