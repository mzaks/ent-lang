// RUN: ent-opt %s -split-input-file -verify-diagnostics

ent.system @s() {
  // expected-error @+1 {{must bind or filter by at least one component}}
  ent.query () {
  }
}

// -----

ent.component @A ()
ent.system @s() {
  // expected-error @+1 {{an 'any' group needs at least two components; use 'with' for one}}
  ent.query () any [@A] {
  }
}

// -----

ent.component @A ()
ent.system @s() {
  // expected-error @+1 {{names component @A in more than one binding or filter}}
  ent.query () with [@A] without [@A] {
  }
}

// -----

ent.component @A ()
ent.component @B ()
ent.system @s() {
  // expected-error @+1 {{names component @A in more than one binding or filter}}
  ent.query (%a: !ent.ref<@A>) any [@A, @B] {
  }
}

// -----

ent.component @A ()
ent.system @s() {
  // expected-error @+1 {{filters by unknown component @B}}
  ent.query () with [@A] without [@B] {
  }
}

// -----

ent.component @A ()
ent.component @B ()
ent.system @s() reads [@A] {
  // expected-error @+1 {{filters by @B but system @s does not declare it in 'reads' or 'writes'}}
  ent.query () with [@A] without [@B] {
  }
}

// -----

ent.component @A ()
ent.system @s() {
  // expected-error @+1 {{reacts to removed @A but requires it; an entity that lost it never matches}}
  ent.query () with [@A] on [removed @A] {
  }
}

// -----

ent.component @A ()
ent.system @s() {
  // expected-error @+1 {{'ent.has' op must be inside an 'ent.query'}}
  %a = ent.has @A
}

// -----

ent.component @A ()
ent.component @B ()
ent.system @s() reads [@A] {
  ent.query () with [@A] {
    // expected-error @+1 {{tests for @B but system @s does not declare it in 'reads' or 'writes'}}
    %b = ent.has @B
  }
}

// -----

ent.resource @R (v: i64)
ent.system @s() {
}
ent.schedule @f() {
  // expected-error @+1 {{condition must end in 'ent.yield' of an i1}}
  ent.run @s() if {
    %v = ent.read @R "v" : i64
    ent.yield %v : i64
  }
}

// -----

ent.resource @R (v: i1)
ent.system @s() {
}
ent.schedule @f() {
  ent.run @s() if {
    %v = ent.read @R "v" : i1
    // expected-error @+1 {{is not allowed in a condition; a condition reads resources and computes with ops free of side effects}}
    ent.write @R "v", %v : i1
    ent.yield %v : i1
  }
}

// -----

ent.resource @R (v: i1)
// expected-error @+1 {{condition must take the schedule's parameters}}
ent.schedule @f(%x: f32) if {
  %v = ent.read @R "v" : i1
  ent.yield %v : i1
} {
}

// -----

ent.resource @R (v: i1)
ent.schedule @f() {
  // expected-error @+1 {{'ent.read' op must be inside an 'ent.system', a condition or 'ent.main'}}
  %v = ent.read @R "v" : i1
}
