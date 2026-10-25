Third party code

* lzma/: LZMA SDK 18.05 decoder, public domain. See lzma/README.md.
* zstddeclib.c: generated unmodified with the upstream v1.5.7 script:
  `cd zstd/build/single_file_libs && python3 combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c zstddeclib-in.c`.
  Source archive: https://codeload.github.com/facebook/zstd/tar.gz/refs/tags/v1.5.7
  Distributed using its BSD licence in zstd-LICENSE; upstream notices retained.
* ../../../lib/zlib: existing repository zlib, licence in its zlib.h.

The new client reads public protobuf wire definitions; no generated SteamKit
code or SteamKit runtime is included. New src/ code is GPL-2.0-only under the
repository's license.txt. Upstream dependency licences stay with their files.

Pinned Zstandard archive SHA-256: 37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3.
Generated decoder SHA-256: 5eb38abe0a7eea13674b312a006df72002d4192f0f0b4d09e6f03fb927a1f3d0.
