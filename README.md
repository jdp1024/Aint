# Aint — A modern AIN reimplementation

Aint stands for **A**IN **I**s **N**ow **T**hreaded.

It is a modern reimplementation of AIN, the DOS archiver, created through
reverse engineering.

## Special Greeting

If you are the author(s) of the original AIN and happen to read this file, please
leave me a message.

# Introduction

I was a big fan of AIN back in the DOS era. Hundreds of floppy disks were saved
thanks to its compression ratio, speed, and volume-splitting feature. I
decided to revive it in 2026.

Here it is: a C reimplementation of the `AIN` archive format used by **AIN 2.32**
(Transas Marine, 1993-96, DOS), which is no longer maintained.

The format was recovered by reverse engineering `ain.exe` (the decoder
was ported instruction-by-instruction from the original table builder +
decoder routines), so this tool reads every real-world `.AIN` file it
has been tested against and writes files the original DOS tool can read
back.

Byte-for-byte identical output is not the goal of this project: my goal is
a modern, **improved** reimplementation of AIN.

# (Reverse) Engineering Work

[TR 2.52](https://www.sac.sk/download/utilprog/tr252.zip) is used to unpack
ain.exe, which was compressed with the EXE compressor `AINEXE`, with the
following simple script:

```
exe1
reload
step
g 0
wexe1
exe2
reload
step
g 0
wexe2
q
mkexe
```

This produces `dosexe/ain_unpacked.exe`, which prints garbage if executed without
any parameters. This is because the original compressed help message was appended
to the compressed EXE and is absent from the unpacked EXE. It works fine for
creating, listing, and extracting AIN files.

With TR's powerful debugging facilities, it is a joy to write simple scripts
for dynamic memory probing, dumping and analysis.

[DOSBox-X](https://github.com/joncampbell123/dosbox-x) is used to run all
DOS executables and to perform binary comparisons and verification.

The (reverse) engineering work uses a semi-vibe-coding approach:

1. `REA` is used for most of the preparation, with the help of Ghidra, to recover
the archive format, codec layout, and garble structures.

2. The debug scripts, memory management, file I/O, threading model,
LZ field restoration, SIMD rewrite, and compression-ratio improvement are
implemented by hand.

3. AI helped write the test framework to verify and preserve archive-format
compatibility. It also provided several valuable suggestions to make the code
more understandable and efficient.

4. AI helped the ARM NEON portation of the AVX2 functions.

# Enhancements

The original AIN was released in 1996 for 16-bit DOS. After 30 years, there is
no reason not to improve it: 64-bit computing and multithreading have been around for
many years.

While preserving the compression ratio and file-format compatibility, the
following optimizations have been made:

* SIMD acceleration. I see a 2x speedup in most cases versus the original
C version. See the Performance section.

* Faster decompression and verification: adaptive 8/10-bit Huffman lookups and
specialized repeated-byte matches.

* Multithreading to greatly speed up compression; see `-jN`. It scales quite
well thanks to its advanced architectural design.

* Algorithms such as insertion and match finding have been rewritten using
modern techniques. I see a 10% improvement in compression ratio in `-m1`
mode. Creation with `-m1` and `-m2` also uses smaller Huffman blocks to
improve adaptation to mixed data. Both compare tree encodings and a bounded
bit-cost reparse, keeping the smallest complete block. `-m1` additionally
tries one optimal parse with refined code costs and a bounded match search,
trading compression time for smaller output while retaining the previous
encoding as a fallback. `-m2` retains its normal initial match-search
budget, while `-m3` retains its fast single-pass algorithm; created
archives are decoder-compatible but are not guaranteed to be byte-identical
to those produced by the original tool.

* Stream analysis to dump detailed information: everything you need to
know about the internals of an AIN file. See the `s` command.

## Performance

The vanilla [AIN 2.32 for DOS](https://www.sac.sk/download/pack/ain232.exe) and
[another AIN-resurrection project](https://github.com/seb3773/AIN) (referred to as ainX)
are used for comparisons of speed and compression ratio:

| VC10 Lite, Binary + Text |       400,532,570 |        |          |
|:-------------------------|------------------:|-------:|---------:|
| AIN 2.32 Vanilla -m1     |        92,765,088 | 23.16% |     -    |
| aint -m1 -j1             |        89,943,928 | 22.46% | 104.76 s |
| aint -m1 -j2             |                   |        |  54.49 s |
| aint -m1 -j4             |                   |        |  28.81 s |
| aint -m1 -j8             |                   |        |  15.52 s |
| aint -m1 -j16            |                   |        |   9.06 s |
| aint -m1 -j32            |                   |        |   4.92 s |
| aint -m1 -j64            |                   |        |   2.79 s |
| aint -m1 -j128           |                   |        |   2.09 s |
| ainX -m1                 |       182,437,373 | 45.54% |  29.48 s |

| VC10 Lite, Binary + Text |       400,532,570 |        |          |
|:-------------------------|------------------:|-------:|---------:|
| AIN 2.32 Vanilla -m2     |        93,240,526 | 23.27% |     -    |
| aint -m2 -j1             |        92,777,376 | 23.16% |  15.28 s |
| aint -m2 -j2             |                   |        |   7.93 s |
| aint -m2 -j4             |                   |        |   4.33 s |
| aint -m2 -j8             |                   |        |   2.47 s |
| aint -m2 -j16            |                   |        |   1.58 s |
| aint -m2 -j32            |                   |        |   0.98 s |
| aint -m2 -j64            |                   |        |   0.70 s |
| aint -m2 -j128           |                   |        |   0.58 s |
| ainX -m2                 |       198,788,526 | 49.63% |  10.79 s |

| VC10 Lite, Binary + Text |       400,532,570 |        |          |
|:-------------------------|------------------:|-------:|---------:|
| AIN 2.32 Vanilla -m3     |       109,433,918 | 27.32% |     -    |
| aint -m3 -j1             |       100,465,879 | 25.08% |   2.21 s |
| aint -m3 -j2             |                   |        |   1.11 s |
| aint -m3 -j4             |                   |        |   0.72 s |
| aint -m3 -j8             |                   |        |   0.54 s |
| aint -m3 -j16            |                   |        |   0.48 s |
| aint -m3 -j32            |                   |        |   0.44 s |
| aint -m3 -j64            |                   |        |   0.43 s |
| aint -m3 -j128           |                   |        |   0.41 s |
| ainX -m3                 |   (*) 110,903,421 | 27.68% |   2.86 s |


| Borland C++ 3.1, Binary + Text |      26,704,962 |        |        |
|:-------------------------------|----------------:|-------:|-------:|
| AIN 2.32 Vanilla -m1           |       8,911,766 | 33.37% |   -    |
| aint -m1 -j1                   |       8,604,194 | 32.22% | 6.22 s |
| aint -m1 -j2                   |                 |        | 3.20 s |
| aint -m1 -j4                   |                 |        | 1.74 s |
| aint -m1 -j8                   |                 |        | 0.96 s |
| aint -m1 -j16                  |                 |        | 0.62 s |
| aint -m1 -j32                  |                 |        | 0.38 s |
| aint -m1 -j64                  |                 |        | 0.28 s |
| aint -m1 -j128                 |                 |        | 0.25 s |
| ainX -m1                       |      13,775,604 | 51.58% | 1.56 s |

| Borland C++ 3.1, Binary + Text |      26,704,962 |        |        |
|:-------------------------------|----------------:|-------:|-------:|
| AIN 2.32 Vanilla -m2           |       8,941,769 | 33.48% |   -    |
| aint -m2 -j1                   |       8,794,835 | 32.93% | 1.28 s |
| aint -m2 -j2                   |                 |        | 0.66 s |
| aint -m2 -j4                   |                 |        | 0.37 s |
| aint -m2 -j8                   |                 |        | 0.23 s |
| aint -m2 -j16                  |                 |        | 0.17 s |
| aint -m2 -j32                  |                 |        | 0.14 s |
| aint -m2 -j64                  |                 |        | 0.12 s |
| aint -m2 -j128                 |                 |        | 0.11 s |
| ainX -m2                       |      13,920,059 | 52.12% | 0.79 s |

| Borland C++ 3.1, Binary + Text |      26,704,962 |        |        |
|:-------------------------------|----------------:|-------:|-------:|
| AIN 2.32 Vanilla -m3           |       9,213,440 | 34.50% |   -    |
| aint -m3 -j1                   |       9,338,753 | 34.97% | 0.27 s |
| aint -m3 -j2                   |                 |        | 0.15 s |
| aint -m3 -j4                   |                 |        | 0.12 s |
| aint -m3 -j8                   |                 |        | 0.11 s |
| aint -m3 -j16                  |                 |        | 0.11 s |
| aint -m3 -j32                  |                 |        | 0.11 s |
| aint -m3 -j64                  |                 |        | 0.11 s |
| aint -m3 -j128                 |                 |        | 0.10 s |
| ainX -m3                       |  (*) 10,969,868 | 41.07% | 0.42 s |

(*) Yes, `-m3` < `-m2` in compressed size.

AIN 2.32 Vanilla runs under DOSBox-X, so its speed numbers are meaningless.

ainX was compiled with `-O3 -flto -mavx2 -mbmi -bmi2`. It wins on speed in
single-threaded mode, but its compression ratio is poor.

So, basically, we win on both speed (thanks to multithreading) and compression
ratio.

## Known Issues

* Creating multi-volume archives with `-fN` is 20%–30% slower than creating
single-volume archives. I also do not try to reproduce exactly the volume-splitting
policy of the original AIN.

* Only compatibility with the original AIN 2.32 is maintained. I have tested a lot of
archives created by AIN 2.2/2.22, but only AIN 2.32 has been tested extensively.

## Version

With these enhancements, the project deserves a version number starting
at `3`.

## Build

AINT is written in C. Threading and filesystem operations have separate
Win32 and POSIX backends. File contents use standard C buffered I/O;
Linux/macOS directory and metadata operations use POSIX APIs, and workers
use pthreads. The codec uses AVX2 on x86-64 and NEON on little-endian
ARM64 (including Apple Silicon). SIMD selection is at compile time,
not through runtime CPU dispatch.

Windows / MSVC (no other dependencies):

```bat
build.bat            :: produces aint.exe
```

Note: I used Visual Studio 2026 Community, you may have to modify `build.bat` if
you use older version of Visual Studio.

Windows / MinGW:

```
make -f Makefile.gcc
```

On Windows, `aint.exe` built with GCC 16.2 runs 3% faster overall than
the executable built with MSVC 19.51.

Linux / macOS, x86-64 or ARM64 (GCC or Clang, with make):

```
make -f Makefile.gcc                 # produces aint
make -f Makefile.gcc CC=clang        # explicit Clang selection
make -f Makefile.gcc test-threads test-fs test-simd test-decode test-checksum
```

The makefile queries the compiler target: x86-64 adds `-mavx2`, while
ARM64 uses baseline NEON without x86 compiler flags. A cross-compiler
can be selected with `CC`; test binaries must run on their target host.
The MSVC batch build remains x86-64.

## Usage

Windows accepts both `-` and `/` switch prefixes (e.g. `-r` or `/R`).
Linux/macOS accept only `-`, so absolute paths are not mistaken for switches.
Quote wildcards to let AINT, rather than the shell, expand them.

Archive member names retain DOS backslash separators; extraction converts
them to host-native separators. On Linux/macOS, DOS drive prefixes and
leading separators are removed from extracted members, placing them under
the current directory or `-o` directory. Windows retains its original
drive-qualified extraction behavior. DOS hidden attributes are read from
dotfile names on POSIX but do not rename extracted files; no DOS attribute
can be applied there. Read-only attributes are recorded but, as on Windows,
are not restored during extraction.

Recursive scans do not follow directory symlinks/reparse points (to avoid
cycles). File symlinks are archived as their target contents, not as links.
DOS-style filename matching remains case-insensitive and `*.*` includes
extensionless files. Member names are also compared case-insensitively on
update; names differing only by case cannot be represented independently.

```
aint <command> {<switch>} <archive>[.AIN] [<base dir>\] [@list|wild ...]

commands:
  a  Add files to an archive (creates a new one; -y to overwrite an existing one)
  d  Delete files from an archive (rewrites the rest using the same method)
  e  Extract files from an archive (base names only — flat, like the original)
  l  List the contents of an archive
  s  Dump detailed stream analysis, including archive header fields
  t  Test the integrity of an archive
  u  Update files in an archive (replace older copies, add new files;
     with no file specs: freshen every member against the same-named disk file;
     for a missing archive: behaves like a)
  v  Verbosely list contents (adds a checksum column)
  x  eXtract files with full pathnames
  y  copY archive, reproducing it with the given options (method, speed,
     password, fragmentation) — the original's idiom for changing or
     cancelling a password

switches:
  -f<n>[K|M] | -fa   fragment size (plain number or K = KB, M = MB;
                     archives larger than it are split into
                     <name>.AIN, .001 ... .NNN volumes; a = no limit)
  -m1..-m4           compression method (m1 = slow/best search, m2 =
                     normal, m3 = fast, m4 = stored; default -m2)
  -o<dir>            output dir: a/u = where the archive is written,
                     x = where files are extracted
  -r                 Recurse into subdirectories (a, u)
  -s                 set the archive's timestamp to that of its newest member
  -u1..-u3           update/search speed: 1 = slow, higher compression,
                     2 = normal, 3 = fast, looser; overrides the method's
                     inherent search effort
  -j0..-j256         creation workers: 0 automatic (default),
                     1 serial; rewrites remain serial
  -q0..-q2           quiet level: 0 normal (default), 1 no per-file status/progress,
                     2 no status, progress, or summaries
  -x<wild|@file>     eXclude files from processing (a, u)
  -y                 answer Yes to requests (archive/file overwrite)
  -g<pass>[=<new>]   Garble with a password; =<new> changes it and a bare
                     -g<pass>= cancels encryption (with a, u, y);
                     -g / -g? prompts for the password on stdin
  -                  a bare "-" disables switch parsing
```

* `-q1` hides per-file add/update/extract/delete/verification messages,
  directory-creation messages, individual volume-writing messages, and
  progress lines. Overall operation messages and summaries remain visible.
  `-q2` also hides those remaining status messages and summaries.
  Neither level hides errors, warnings, overwrite/password prompts, help,
  or requested `l`/`v` listing and `s` stream-analysis output. Quiet mode does
  not answer prompts; use `-y` to allow overwrites without prompting.
  Use `-q0` to restore normal output; the last `-qN` option wins.
* `a`, `u`, `e`, and `x` report byte progress on stderr, starting at 0%.
  Intermediate lines print only after more than 1 MiB (1,048,576 bytes)
  has been processed and at least one percentage point has advanced since
  the last report: `Adding: 42% (44040192/104857600 bytes)`.
  Totals use uncompressed file sizes, not compressed archive sizes or file
  counts. Each intermediate byte count is the actual processed count;
  percentages may be skipped. Per-file percentages remain current.
  Add progress advances as input is fed to the encoder or stored output;
  update progress counts only files being added/replaced, as they are read
  into memory (unchanged members are excluded). Extraction progress counts
  selected members as they are written; skipped existing files also advance
  progress. Solid spans must be decoded before their files can be written.
  Progress holds at 99% during finalization, and 100% is printed only after
  success. Empty selections/files and no-op updates print 0% and 100% with
  `0/0 bytes`. Cancellation or failure does not print 100%. Updates to a
  missing archive use add progress.
* After successful add/update operations, a summary prints
  `Total processed: <n> bytes, compressed: <n> bytes, ratio: aa.bb%, elapsed: n.nn s`.
  Uncompressed bytes cover all members in the resulting archive, including
  unchanged members on update. Compressed bytes are the full archive size
  (including headers and index), summed across all volumes for split archives.
  No-op updates also show the existing archive's totals.
  Ratio is compressed bytes divided by uncompressed bytes, as a percentage
  with two decimal places (lower is better; overhead can make it exceed
  100%). Empty input displays `N/A`. Elapsed time uses a monotonic wall clock
  and is shown in seconds with two decimal places, including archive
  finalization. Creation timing starts before input-size collection;
  existing-archive update timing starts before opening the archive.
* `@list` files contain one file/pattern per line (`;` starts a comment).
* File specs may be literal paths, DOS wildcards (`?`, `*`), or
  directories (expanded to their files; `-r` recurses). A directory
  argument directly after the archive name acts as the base directory.
* On `l / s / v / t / x / d / u`, trailing wildcards filter members; a
  pattern without path separators matches the member's base name or any
  directory component (`sub` matches `x\sub\file`; `*.log` matches at any level).
* `-m1/-m2/-m3` compress all files as **one solid stream** (one per volume
  when using `-f`); the method
  sets the encoder's search effort: `m1` = deepest chain (smallest
  archives), `m2` = normal, `m3` = shallow greedy (fastest, looser).
  `-u1/-u2/-u3` override that effort for the run (the original's
  "archive update speed"). The method nibble is recorded in the header
  either way, and all levels decode identically in the original tool.
  Creation at speeds 1 and 2 (`-m1`/`-m2` or `-u1`/`-u2`) refreshes
  Huffman trees every 64 KiB to adapt to changing data; speed 3 retains
  2 MiB blocks.
  The solid-stream match history continues across these boundaries.
* `-m4` stores files **uncompressed**, one independent segment per file
  (one raw span per volume when using `-f`).
* File names are stored verbatim as given on the command line (this
  matches the original). On extraction, a drive-qualified member name
  (`C:\A\M.BAK`, as found in `A.AIN`) is restored to that **exact
  absolute path** on Windows, just like the original tool; giving `-o<dir>`
  instead strips the drive prefix and nests the member tree under the
  output directory (the safe mode for archives with absolute members).
  Linux/macOS always strip the drive prefix and leading separators.
* `e` vs `x` (verified against the original in DOSBox): `x` keeps each
  member's full pathname (relative members nest under the cwd/output
  dir, absolute ones restore verbatim); `e` always drops the directory
  part and writes plain base names into the cwd/`-o` dir — for `e`,
  even `C:\SRC2\W.TXT` becomes just `W.TXT` there.
* Per-file add/update messages include the current overall byte progress:
  `Adding <file> (32%)` or `Updating <file> (32%)`.
  As with the separate progress lines, the percentage stays at 99% until finalization.
* Successful `e`/`x` extractions print
  `Extracting <member> to <full destination path> (32%)`.
  The `e` command displays only the member's base name before `to`.
* `e`/`x` show the target path and prompt before overwriting an existing
  target: `Y` overwrites it, `N` skips it, `AY` overwrites this and all later
  conflicts, `AN` skips this and all later conflicts, and `Q` stops extraction
  with a nonzero exit status. Answers are case-insensitive. `AY`/`AN` apply
  only to the current run; new files still extract after `AN`. Use `-y` to
  overwrite without prompting. Invalid answers re-prompt; end-of-input
  aborts with an error. Files already extracted are kept when stopping.
* `d`/`u` decode the archive and rewrite it in one pass (same method
  unless `-m` is given); deleting all members removes the archive file.
* `s` prints the raw 24-byte header and all field offsets/values, including
  method/flags, volume metadata, password hash (not the password), DOS timestamp,
  index offset, checksums, and uninterpreted bytes. It analyzes the compressed
  index and each selected data stream: member layout, encoded/decoded sizes,
  checksums, per-block bit/output ranges, Huffman code-length distributions,
  literal/match counts and bit costs, distance-class and match-length
  histograms, EOF/tree-reload markers, padding, and trailing bytes.
  Bit ranges are relative to each stream; byte ranges refer to the reassembled
  archive. Ranges are half-open. Filters select entire solid spans, including
  nonmatching members that share their stream; the index is always analyzed.
  Stored (`-m4`) segments have no codec blocks. Garbled archives require `-g`
  or an interactive password; data checksums are verified against ciphertext.
  Corrupt checksums, invalid streams, and decoded-size mismatches return a
  nonzero exit status. This read-only command is distinct from the `-s`
  timestamp switch.
* `-f` volume sets: `l`/`s`/`v`/`t`/`x`/`d`/`u` accept the base name, the
  `.AIN` name, or any single volume file (`<name>.001`, `.002`, ...) —
  each part has its own AIN header, data, and compressed index. Split members
  record their uncompressed file offsets so the original DOS AIN can reconstruct
  them. All parts must sit side by side. Creation and rewrites use this
  original-compatible indexed layout, including encrypted sets; each part
  stays within the requested size, and rewrites retain the first part's size
  as their limit. Legacy AINT raw-slice and final-index-only stream-sliced
  sets remain readable but are no longer written. The writer searches for
  a fitting input-fragment size using the actual encoded data and index
  sizes, rather than halving fragments and leaving large gaps. Final
  volumes and metadata-only volumes may be smaller than the requested size.
  Split creation shares one raw buffer between input chunks and packing
  trials, reading up to 64 MiB at a time.
  Chunks cross member boundaries: packing and index compression run per
  input batch, not after each file in a recursive tree. The local index maps
  each accepted batch prefix back to member sizes and fragment offsets.
  Packing trials reuse cached Huffman blocks and the worker pool. When a
  completed block leaves at most 2% unused, the writer closes it directly
  without recompressing a partial block. Exact-prefix trials handle smaller
  volumes and cases where a block boundary cannot meet that target.
  Encoded look-ahead survives block-aligned volume boundaries; only its
  first block is re-encoded without the previous volume's history.
* Garble (`-g`, fully implemented): `a`/`u`/`y` encrypt the new archive
  with the password. Every command that touches member data (`t/e/x/d/u/y`)
  requires it; `l`/`v` read the (never encrypted) index. A failed password
  prints `Incorrect password` followed by `Continue? [Y/AY/Q]` and the
  `Enter password: ` prompt — both read stdin, and input is masked with one
  `"` per character, exactly as in the original's redirected-stdin
  behavior. `-gOLD=NEW` re-encrypts with a new password, `-gOLD=` removes
  encryption (`y -gOLD[=NEW] archive` mirrors the original's examples
  18/19); `y` without `-g` keeps the existing password.

## Total Commander plugin

The native C [WCX plugin](wcx/README.md) lists, tests, extracts, creates,
updates, and deletes AIN members. Its Windows configuration dialog selects
compression method, split volume size, and thread count. Passwords are entered
per operation in a separate dialog with confirmation, not in configuration.
Both 32-bit and 64-bit Total Commander builds are provided.
It links the shared AIN core directly and needs no external archiver.

```powershell
.\wcx\test.ps1       # build and test x86 and x64
.\wcx\package.ps1    # create an installable ZIP
```

See the plugin documentation for installation, limits, and update semantics.

## Layout

```
dosexe/ain_original.exe   The original EXE.
dosexe/ain_unpacked.exe   The unpacked EXE.
src/ain.h       format constants, APIs
src/codec.c     optimized format-compatible stream decoder + encoder,
                incl. streaming multi-block encoder (ain_enc_*)
src/thread.h    private worker interface
src/thread.c    Win32 / pthread workers
src/fs.h        host filesystem interface
src/fs.c        Win32 / POSIX directories, metadata and path helpers
src/garble.c    password keystream cipher (exact port of original)
src/archive.c   container: open/list/test/extract/create
src/main.c      CLI
wcx/            Total Commander C packer plugin, dialogs, builds and tests
build.bat       MSVC build
Makefile.gcc    MinGW / Linux / Intel macOS GCC or Clang build
```

## Future

* Big-endian targets and x86 CPUs without AVX2 are not supported.
* The CLI build scripts target 64-bit platforms; the WCX project also builds
  a 32-bit x86 plugin.
