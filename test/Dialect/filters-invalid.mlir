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
