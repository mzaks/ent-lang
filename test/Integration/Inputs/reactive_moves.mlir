// examples/reactive.mlir with Shield stored in separate archetypes: losing
// or regaining it moves the ship between @Ship and @Shielded. The systems
// are the same, and so must be what every reactive query collects: stamps
// move with their entities, and a move that removes or adds Shield is the
// event.

ecs.component @Hull (hp: f32)
ecs.component @Hit (damage: f32)
ecs.component @Bar (width: f32, redraws: i32, alarms: i32, restores: i32)
ecs.component @Shield ()

ecs.archetype @Ship (@Hull, @Hit, @Bar) capacity 16
ecs.archetype @Shielded (@Hull, @Hit, @Bar, @Shield) capacity 16

// Takes the damage waiting in Hit; repairs restore the shield.
ecs.system @absorb() {
  ecs.query (%h: !ecs.ref<@Hull, mut>, %hit: !ecs.ref<@Hit, mut>) {
    %d = ecs.get %hit "damage" : !ecs.ref<@Hit, mut> -> f32
    %zero = arith.constant 0.0 : f32
    %any = arith.cmpf one, %d, %zero : f32
    scf.if %any {
      %hp = ecs.get %h "hp" : !ecs.ref<@Hull, mut> -> f32
      %left = arith.subf %hp, %d : f32
      ecs.set %h "hp", %left : !ecs.ref<@Hull, mut>, f32
      ecs.set %hit "damage", %zero : !ecs.ref<@Hit, mut>, f32
      %repair = arith.cmpf olt, %d, %zero : f32
      scf.if %repair {
        ecs.add @Shield()
      }
    }
  }
}

// A shield fails below 50 hp.
ecs.system @fail() {
  ecs.query (%h: !ecs.ref<@Hull>, %s: !ecs.ref<@Shield>) {
    %hp = ecs.get %h "hp" : !ecs.ref<@Hull> -> f32
    %limit = arith.constant 50.0 : f32
    %low = arith.cmpf olt, %hp, %limit : f32
    scf.if %low {
      ecs.remove @Shield
    }
  }
}

ecs.system @redraw() {
  ecs.query (%h: !ecs.ref<@Hull>, %b: !ecs.ref<@Bar, mut>)
      on [changed @Hull "hp"] {
    %hp = ecs.get %h "hp" : !ecs.ref<@Hull> -> f32
    ecs.set %b "width", %hp : !ecs.ref<@Bar, mut>, f32
    %n = ecs.get %b "redraws" : !ecs.ref<@Bar, mut> -> i32
    %one = arith.constant 1 : i32
    %next = arith.addi %n, %one : i32
    ecs.set %b "redraws", %next : !ecs.ref<@Bar, mut>, i32
  }
}

ecs.system @alarm() {
  ecs.query (%b: !ecs.ref<@Bar, mut>) on [removed @Shield] {
    %n = ecs.get %b "alarms" : !ecs.ref<@Bar, mut> -> i32
    %one = arith.constant 1 : i32
    %next = arith.addi %n, %one : i32
    ecs.set %b "alarms", %next : !ecs.ref<@Bar, mut>, i32
  }
}

ecs.system @restored() {
  ecs.query (%b: !ecs.ref<@Bar, mut>, %s: !ecs.ref<@Shield>)
      on [added @Shield] {
    %n = ecs.get %b "restores" : !ecs.ref<@Bar, mut> -> i32
    %one = arith.constant 1 : i32
    %next = arith.addi %n, %one : i32
    ecs.set %b "restores", %next : !ecs.ref<@Bar, mut>, i32
  }
}

ecs.schedule @frame() {
  ecs.run @absorb()
  ecs.run @fail()
  ecs.run @redraw()
  ecs.run @alarm()
  ecs.run @restored()
}
