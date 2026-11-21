# QuickJS-ng (vendored)

[QuickJS-ng](https://github.com/quickjs-ng/quickjs) v0.17.0 (Fabrice
Bellard, Charlie Gordon, Ben Noordhuis, Saúl Ibarra Corretgé and the
QuickJS-ng contributors; MIT License, `LICENSE` here): the release's
`quickjs-amalgam.zip` (sha256 `a0955463c74809173a253ff87365095e6972e8b171cfb6aa8a1e781adf35cfeb`),
its three files as they are. `devices/js.c` includes `quickjs-amalgam.c`
without `QJS_BUILD_LIBC`, so its `std` and `os` modules (files, processes,
the network) are not compiled in: a script reaches only what `js.c` gives
it.

Measured (Apple M-series, clang -O2): about 5 s to compile, 1.4 MB of
object; `tools/ent` keeps the object in its cache (`objects/`), so a
program is not compiled again with it until this folder changes.

To update: unzip `quickjs-amalgam.zip` of a newer release over these
files, update the version and digest here and in THIRD_PARTY_NOTICES, and
run `test/Integration/js_device.test`.

Not yet built for the web here (`js.web.link` gives the page a 1 MB
stack, and `js.c` QuickJS a 512 KB limit within it).
