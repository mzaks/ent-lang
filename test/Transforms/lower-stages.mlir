// RUN: ecs-opt %s --ecs-lower-to-loops | FileCheck %s --check-prefix=SEQ
// RUN: ecs-opt %s --ecs-lower-to-loops=parallel-stages=1 | FileCheck %s --check-prefix=PAR

ecs.component @P (x: f32, y: f32)
ecs.archetype @A (@P) capacity 1000

ecs.system @writeX(%c: f32) writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>) {
    ecs.set %p "x", %c : !ecs.ref<@P, mut>, f32
  }
}
ecs.system @writeY(%c: f32) writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>) {
    ecs.set %p "y", %c : !ecs.ref<@P, mut>, f32
  }
}

// Sequential: stages dissolve into calls in program order.
// SEQ-LABEL: func.func @frame(
// SEQ-NEXT:    call @writeX
// SEQ-NEXT:    call @writeY
// SEQ-NEXT:    call @writeX
// SEQ-NEXT:    return

// Parallel: a stage with several runs becomes one section per run; a
// stage with a single run stays a plain call.
// PAR-LABEL: func.func @frame(
// PAR-NEXT:    omp.parallel {
// PAR-NEXT:      omp.sections nowait {
// PAR-NEXT:        omp.section {
// PAR-NEXT:          func.call @writeX
// PAR-NEXT:          omp.terminator
// PAR-NEXT:        }
// PAR-NEXT:        omp.section {
// PAR-NEXT:          func.call @writeY
// PAR-NEXT:          omp.terminator
// PAR-NEXT:        }
// PAR-NEXT:        omp.terminator
// PAR-NEXT:      }
// PAR-NEXT:      omp.terminator
// PAR-NEXT:    }
// PAR-NEXT:    call @writeX
// PAR-NEXT:    return
ecs.schedule @frame(%c: f32) {
  ecs.stage {
    ecs.run @writeX(%c) : f32
    ecs.run @writeY(%c) : f32
  }
  ecs.stage {
    ecs.run @writeX(%c) : f32
  }
}
