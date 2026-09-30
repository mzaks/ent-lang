// Change detection for bench/reactive: @hit lowers the hp of the units a
// hash of their seed and the frame picks (threshold / 65536 of them), and
// @redraw recomputes the bar of the units whose hp changed. The bar
// depends on hp only, so redrawing an unchanged unit changes nothing but
// its visit count. The work between the WORK markers is the light one; the
// runner replaces it with a 32-step loop for the heavy one.

ecs.component @Hull (hp: f32)
ecs.component @Seed (s: i32)
ecs.component @Bar (width: f32, visits: i32)

ecs.archetype @Unit (@Hull, @Seed, @Bar) capacity 1000000

ecs.system @hit(%frame: i32, %threshold: i32) reads [@Seed] writes [@Hull] {
  ecs.query (%h: !ecs.ref<@Hull, mut>, %s: !ecs.ref<@Seed>) {
    %seed = ecs.get %s "s" : !ecs.ref<@Seed> -> i32
    %a = arith.constant -1640531535 : i32
    %b = arith.constant 40503 : i32
    %x0 = arith.muli %seed, %a : i32
    %x1 = arith.muli %frame, %b : i32
    %x = arith.addi %x0, %x1 : i32
    %c8 = arith.constant 8 : i32
    %mask = arith.constant 65535 : i32
    %shifted = arith.shrui %x, %c8 : i32
    %m = arith.andi %shifted, %mask : i32
    %chosen = arith.cmpi ult, %m, %threshold : i32
    scf.if %chosen {
      %hp = ecs.get %h "hp" : !ecs.ref<@Hull, mut> -> f32
      %one = arith.constant 1.0 : f32
      %less = arith.subf %hp, %one : f32
      ecs.set %h "hp", %less : !ecs.ref<@Hull, mut>, f32
    }
  }
}

ecs.system @redraw() reads [@Hull] writes [@Bar] {
  ecs.query (%h: !ecs.ref<@Hull>, %b: !ecs.ref<@Bar, mut>)
      on [changed @Hull "hp"] {
    %hp = ecs.get %h "hp" : !ecs.ref<@Hull> -> f32
    // BEGIN WORK (run.py swaps this block for the heavy work)
    %half = arith.constant 0.5 : f32
    %base = arith.constant 1.0 : f32
    %scaled = arith.mulf %hp, %half : f32
    %w = arith.addf %scaled, %base : f32
    // END WORK
    ecs.set %b "width", %w : !ecs.ref<@Bar, mut>, f32
    %v = ecs.get %b "visits" : !ecs.ref<@Bar, mut> -> i32
    %one = arith.constant 1 : i32
    %next = arith.addi %v, %one : i32
    ecs.set %b "visits", %next : !ecs.ref<@Bar, mut>, i32
  }
}

ecs.schedule @frame(%frame: i32, %threshold: i32) {
  ecs.run @hit(%frame, %threshold) : i32, i32
  ecs.run @redraw()
}
