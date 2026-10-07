# LZO1X decoder provenance

Adapted from the decompression state machine in AxioDL/lzokay's `lzokay.cpp`:
https://github.com/AxioDL/lzokay/blob/db2df1fcbebc2ed06c10f727f72567d40f06a2be/lzokay.cpp

Copyright (c) 2018 Jack Andersen; MIT license retained in LICENSE.

The C adaptation uses checked offsets instead of speculative pointers and
unaligned reads. It validates extension lengths, input consumption, end markers,
back-references, and output capacity. A count-only mode supports allocation-free
size probing bounded by the existing decompression limit. No compressor,
dictionary, LZO-RLE variant, lzop container, or application framing is included.
