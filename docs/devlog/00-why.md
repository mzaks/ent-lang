# 00 · Why build ent-lang

*Devlog, written November 2026, looking back over the first five weeks.*

## Disclosure first

ent-lang is being built with an AI. I pick the direction, the ideas and the
trade-offs. Claude (through Claude Code) writes most of the code, the tests and
the benchmarks, and tells me when a number looks wrong. Every commit says so:
the early ones carry `Assisted-by: AI`, the later ones a `Co-Authored-By` line.
These posts are drafted the same way. Claude reads the commit history and its
own notes from our sessions, asks me why I wanted things, and I edit the result.
When a post says "we", it means the two of us. When it says "I", it means me,
Maxim.

That way of working runs through this whole log. Some posts show where it helped
and some show where it didn't.

## Thirteen years of ECS

I started working with the Entity Component System pattern in 2012 and never
stopped liking it. In ECS, entities are just ids, components are plain data,
and systems are functions that run over every entity holding a given set of
components. I have talked about it a lot:

- Unite Europe 2015: <https://youtu.be/1wvMXur19M4>
- Unite Europe 2016: <https://youtu.be/lNTaC-JWmdI>
- Code Star 2019: <https://youtu.be/lt4eL4RSx7k>

I have also tried to turn ECS into a language several times before:

- [ECS-Lang](https://github.com/mzaks/ECS-Lang) and
  [Entitas-Lang](https://github.com/mzaks/Entitas-Lang), two DSLs that
  generate code for existing engines;
- [a sketch of a full language syntax](https://gist.github.com/mzaks/d2d410f8768aa2c6db69c4218d53cd80),
  which never got a compiler.

None of these got past the point where a hobby project needs a few months of
compiler work nobody pays for. That's the honest history.

## Why a language and not another library

There are plenty of good ECS libraries. They all hit the same ceiling. The
library knows the pattern, but the compiler beneath it doesn't. ECS gives you
three things a compiler could use:

- **Performance.** Data sits in columns. Who reads what is known before the
  program runs. Systems that don't share data can run side by side.
- **Decomposition.** Behaviour is split into small systems over plain data.
  Nothing hides state in an object graph.
- **Predictability.** A frame is a fixed sequence of systems, so the same input
  gives the same output.

A library can only *ask* you to follow the rules. A compiler can *know* you
did. Once it knows, two of the usual problems of general-purpose languages
mostly disappear:

- *Memory management*: the world's storage is laid out by the compiler, with
  no `malloc` for every entity.
- *The level of abstraction*: you write what happens to entities, and the
  compiler decides about loops, threads, layouts, and later GPUs and indexes.

## Pure and looping computation

My talks always drew a line between two kinds of programs:

- **Pure computation:** input, one big pass of transformations, output, done.
  Most languages are built for this, functional ones most of all.
- **Looping computation:** input, transformation, output, and then *the output
  becomes the next input*. Round and round, sixty times a second or a few
  million times per experiment. Games, simulations and models of epidemics or
  beehives all work like this.

You can write the loop in any procedural, OO or functional language. People
do it every day. The aim of ent-lang is that the loop is the *natural* thing
to write, not something you build by hand around a pure core.

## Why now, and why MLIR

I'm active in the Mojo community and Modular names me a Mojo champion. Mojo is
built on MLIR, so I knew its general ideas. Shortly before ent-lang began, Chris
Lattner posted on the Mojo Discord that he had met Rustan Leino. They had talked about
Dafny, and about how Mojo could gain from contracts and verification built
into the language. The Mojo compiler had just been fully open-sourced, and
Chris asked whether someone in the community wanted to explore that. I said
yes and started prototyping with Claude Code.

The prototyping went surprisingly well. I took that as a sign: the compiler
work that had stopped my earlier ECS-language attempts was no longer the
bottleneck. So I could finally try all the crazy ideas I'd been collecting
since 2012.

## Why the dialect came first and the syntax later

The first five days of ent-lang had no syntax at all. Programs were MLIR,
written by hand. There were two reasons:

1. **The semantics were what I cared about.** What a system may touch, when an
   effect becomes visible, and what parallel means without being
   nondeterministic. An MLIR dialect makes you answer those questions and lets
   you leave the spelling for later.
2. **I've never been one for bikeshedding syntax.** Code should say clearly
   what it does. Tabs against spaces is not a debate, it's a waste of an
   afternoon. With an LLM writing most of the code anyway, the semantics carry
   even more of the weight. I'm even considering several syntax styles for
   ent-lang, so people can pick the one they read best.

## The name

*Ent*-ity plus *lang*, and also German *entlang*, "along": systems run *along*
the entities their queries match. It was called ecs-lang for its first four
days. The rename is in [post 03](03-from-dialect-to-language.md).

## Where it stands

On 4 November ent-lang was tagged v0.1.0 (and v0.1.1 the same day). That's
631 commits after the first one on 28 September. By then it had:

- a language with a parser, an LSP and editor support;
- deterministic parallel execution on all cores, and on the GPU for systems
  that fit;
- indexes as a language feature;
- devices written in C, Rust or Mojo;
- ports that match their reference implementations, often bit for bit: Doom,
  Pushover, Clay's UI examples, an epidemic model, a honeybee colony, traffic,
  molecular dynamics, a SAT solver and more;
- a game made for a hackathon, Pixalorien.

The rest of this log covers how that happened, roughly in order.

Next: [01 · Day one, an ECS as an MLIR dialect](01-day-one.md).
