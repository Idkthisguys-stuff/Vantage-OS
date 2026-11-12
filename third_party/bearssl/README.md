# BearSSL

TLS and X.509 library by Thomas Pornin, MIT license (`LICENSE.txt`).

- Upstream: https://www.bearssl.org/git/BearSSL
- Version: `master` at commit `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`
  (2026-04-06, "Fixed bug in handling incoming records with invalid length").

The last tagged release, v0.6 (2018-08-14), lacks later upstream fixes,
including carry propagation bugs in the 64-bit P-256 code, an RSA-PSS
verification bug, a buffer overflow in private key decoding, and the
record-length handling fix above. The commit above is the newest upstream
state.

`inc/`, `src/` and `LICENSE.txt` are the upstream files, unmodified. Upstream
build files, tests, tools, samples and the T0 compiler are not included. The library is configured only through `BR_*` compiler
definitions in `Makefile` (kernel builds) and `cmake/VantageTLS.cmake` (host
builds); see `docs/TLS.md`. `.gitattributes` disables whitespace checks for
these files so they can stay byte-identical to upstream.

## Updating

```sh
git clone https://www.bearssl.org/git/BearSSL
cd BearSSL
git -c core.autocrlf=false archive --format=tar -o bearssl.tar <commit> inc src LICENSE.txt
```

Replace `inc/`, `src/` and `LICENSE.txt` here with the archive contents,
update the commit above, check `BEARSSL_SOURCES` in `mk/sources.mk` against
the new source list, and run the kernel build, host tests and linked ARM
suites listed in `docs/TLS.md`.
