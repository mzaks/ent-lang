// Reactive queries: a ship's health bar is redrawn only when its hp
// changed, an alarm counts shields going down, and a counter counts
// shields coming back. Each reactive query records how often it visited a
// ship, so the host can check exactly which ships each frame collected.
// The host deals damage (or repairs, with negative damage) through Hit.
// The systems declare no access: the compiler infers it from their bodies.

ent.component @Hull (hp: f32)
ent.component @Hit (damage: f32)
ent.component @Bar (width: f32, redraws: i32, alarms: i32, restores: i32)
ent.component @Shield ()

ent.archetype @Ship (@Hull, @Hit, @Bar, optional @Shield) capacity 16

// Takes the damage waiting in Hit; repairs restore the shield.
ent.system @absorb() {
  ent.query (%h: !ent.ref<@Hull, mut>, %hit: !ent.ref<@Hit, mut>) {
    %d = ent.get %hit "damage" : !ent.ref<@Hit, mut> -> f32
    %zero = arith.constant 0.0 : f32
    %any = arith.cmpf one, %d, %zero : f32
    scf.if %any {
      %hp = ent.get %h "hp" : !ent.ref<@Hull, mut> -> f32
      %left = arith.subf %hp, %d : f32
      ent.set %h "hp", %left : !ent.ref<@Hull, mut>, f32
      ent.set %hit "damage", %zero : !ent.ref<@Hit, mut>, f32
      %repair = arith.cmpf olt, %d, %zero : f32
      scf.if %repair {
        ent.add @Shield()
      }
    }
  }
}

// A shield fails below 50 hp.
ent.system @fail() {
  ent.query (%h: !ent.ref<@Hull>, %s: !ent.ref<@Shield>) {
    %hp = ent.get %h "hp" : !ent.ref<@Hull> -> f32
    %limit = arith.constant 50.0 : f32
    %low = arith.cmpf olt, %hp, %limit : f32
    scf.if %low {
      ent.remove @Shield
    }
  }
}

ent.system @redraw() {
  ent.query (%h: !ent.ref<@Hull>, %b: !ent.ref<@Bar, mut>)
      on [changed @Hull "hp"] {
    %hp = ent.get %h "hp" : !ent.ref<@Hull> -> f32
    ent.set %b "width", %hp : !ent.ref<@Bar, mut>, f32
    %n = ent.get %b "redraws" : !ent.ref<@Bar, mut> -> i32
    %one = arith.constant 1 : i32
    %next = arith.addi %n, %one : i32
    ent.set %b "redraws", %next : !ent.ref<@Bar, mut>, i32
  }
}

ent.system @alarm() {
  ent.query (%b: !ent.ref<@Bar, mut>) on [removed @Shield] {
    %n = ent.get %b "alarms" : !ent.ref<@Bar, mut> -> i32
    %one = arith.constant 1 : i32
    %next = arith.addi %n, %one : i32
    ent.set %b "alarms", %next : !ent.ref<@Bar, mut>, i32
  }
}

ent.system @restored() {
  ent.query (%b: !ent.ref<@Bar, mut>, %s: !ent.ref<@Shield>)
      on [added @Shield] {
    %n = ent.get %b "restores" : !ent.ref<@Bar, mut> -> i32
    %one = arith.constant 1 : i32
    %next = arith.addi %n, %one : i32
    ent.set %b "restores", %next : !ent.ref<@Bar, mut>, i32
  }
}

ent.schedule @frame() {
  ent.run @absorb()
  ent.run @fail()
  ent.run @redraw()
  ent.run @alarm()
  ent.run @restored()
}
