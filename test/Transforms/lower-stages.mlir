// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s --check-prefix=SEQ
// RUN: ent-opt %s --ent-lower-to-loops=parallel-stages=1 | FileCheck %s --check-prefix=PAR

ent.component @P (x: f32, y: f32)
ent.archetype @A (@P) capacity 1000

ent.system @writeX(%c: f32) writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>) {
    ent.set %p "x", %c : !ent.ref<@P, mut>, f32
  }
}
ent.system @writeY(%c: f32) writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>) {
    ent.set %p "y", %c : !ent.ref<@P, mut>, f32
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
ent.schedule @frame(%c: f32) {
  ent.stage {
    ent.run @writeX(%c) : f32
    ent.run @writeY(%c) : f32
  }
  ent.stage {
    ent.run @writeX(%c) : f32
  }
}
