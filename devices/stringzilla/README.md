# StringZilla (vendored)

The headers of [StringZilla](https://github.com/ashvardanian/StringZilla)
v5.3.1 (Ash Vardanian, Apache License 2.0, `LICENSE` here) that
`stringzilla/find.h` and `stringzilla/memory.h` include, as they are, for
`devices/text.c`: a text's `find` (`sz_find`) and `lower` (`sz_lookup`).
Each picks its code when compiled, by the processor flags `ENT_CFLAGS`
gives (`-march=native`: AVX-512 where there is some); without them the
code for any machine of its kind. `tools/ent` puts `include` here on the
include path of C next to modules; `devices/text.c` falls back to its own
loops where the headers are not found.

Measured (bench/http/RESULTS.md, 2026-11-08): the live crawl at 10^5
pages, the user time 4.6% less (`memchr`, `memcmp` gone), the reports the
same.

To update: copy the same headers from a newer release, and say which.
