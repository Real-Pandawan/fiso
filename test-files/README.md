# Sample FISO images

All audio here is synthetic (sines, noise, digital silence, full-scale values) and the artwork is generated; everything may be redistributed freely (CC0).

| File | Covers |
|---|---|
| `simple-album-no-cover.fiso` | 3 tracks from WAV, gap-less transition between tracks 1 and 2, full-scale samples, silence, noise; no artwork |
| `full-metadata-unicode-cover.fiso` | FLAC pass-through source, Unicode metadata, front cover |
| `indexes-pregaps-htoa-isrc-mcn.fiso` | hidden track one audio, INDEX 00 pre-gap, INDEX 02, ISRC, MCN, pre-emphasis/copy flags |
| `gapless-synthesized-pregap.fiso` | two-FILE CUE with a `PREGAP` (digital silence flagged as synthesized) |
| `multi-disc/` | three discs sharing one `group_id` |
| `hires-48k-24bit-6ch-not-cdda.fiso` | non CD-DA audio (6 channels: cannot be exported to an audio CD, no downmix) |
| `unknown-noncritical-chunk.fiso` | an unknown non-critical chunk readers must ignore |
| `invalid/` | files that must be rejected: bad signature, header CRC, chunk table, truncated, future major version, unknown critical chunk, corrupted audio |
