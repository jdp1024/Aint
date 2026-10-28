# AIN WCX plugin for Total Commander

A native C packer plugin using the shared AINT archive/codec implementation.
No external executable, DOSBox, or command-line password is used.

## Features

- List, test, and extract AIN members, including solid and split archives.
- Create archives and add/replace members in existing archives. Replacements
  are unconditional, even when the source has an older timestamp.
- Delete selected members. Deleting the last member leaves a valid empty
  archive (unlike the AINT CLI, which removes an empty archive).
- Native configuration GUI for compression method, split volume size, and
  compression worker count; a separate password dialog with confirmation.
- x86 (`ain.wcx`) and x64 (`ain.wcx64`) builds, with undecorated WCX exports.

## Build and test

Requires Windows, Visual Studio C++ Build Tools, a Windows SDK, and an
**AVX2-capable CPU and OS** (the shared codec uses AVX2 on x86/x64).
The static MSVC runtime is linked into the plugin.

From the project root:

Build both architectures and package in one command:

```powershell
.\wcx\b.bat
```

Or build, test, and package separately:

```powershell
.\wcx\build.bat x64
.\wcx\build.bat x86
.\wcx\test.ps1 -SkipBuild
.\wcx\package.ps1
```

Or run `.\wcx\test.ps1` to build and test both architectures. Test executables
simulate the WCX host, operate the real configuration dialog automatically,
and verify roundtrips for methods 1..4, serial/automatic/explicit workers,
passwords, split-volume size limits, updates, deletion, cancellation, checksum
errors, rollback, and the original split-volume fixtures.

CMake can also generate a Visual Studio solution:

```powershell
cmake -S .\wcx -B .\wcx\build\cmake-x64 -A x64
cmake --build .\wcx\build\cmake-x64 --config Release
ctest --test-dir .\wcx\build\cmake-x64 -C Release --output-on-failure
```

Use a separate build directory and `-A Win32` for x86. CMake does not require
the separately downloaded WCX SDK; the small ABI header is maintained here.

## Install

Open `build\ain-wcx.zip` in Total Commander to use its plugin installer.
The ZIP includes both architectures; Total Commander selects the matching
plugin. Alternatively:

1. Copy the matching binary to a permanent plugin directory. Keep both
   `ain.wcx` and `ain.wcx64` together when installing for both TC editions.
2. Open **Configuration > Options > Packer > Configure packer extension DLLs**.
3. Add extension `ain`, then select `ain.wcx` (32-bit) or `ain.wcx64` (64-bit).
4. Open an `.ain` archive normally. Use **Alt+F5** and choose `ain` to pack.
   Packing into an existing archive replaces matching members and retains
   unselected members. Internal archive subdirectories are supported.
5. In the pack dialog, select AIN and use **Configure** to change its settings.

## Configuration

| Setting | Values |
|---|---|
| Compression | 1 best/slow; 2 normal (default); 3 fast; 4 store |
| Volume size | KiB; 0 = single archive (default), 1..16383 = split |
| Worker threads | 0 = automatic (default), 1 = serial, 2..256 = explicit |

Split archives use `name.ain`, `name.001`, `name.002`, etc. Every volume,
including its header and compressed index, fits the configured size.
Very small limits can fail if required metadata will not fit.
All volumes must be adjacent; open the `.ain` first volume. Creation/update
requires an `.ain` filename. Missing volumes return an archive error; the
plugin does not ask for removable disks or alternate volume locations.

Both dialogs are centered in the active monitor's work area. Password fields
appear only in the separate password dialog. Setting a password for compression
requires matching password and repeat-password entries; opening, testing, or
extracting an existing archive shows only one password field.

Method, volume size, and worker count are saved to
`%APPDATA%\AinWCX\settings.ini`. The last confirmed compression level is restored
when the dialog is reopened, including after restarting Total Commander.
No password is stored in configuration or cached globally.
Select Total Commander's **Encrypt** checkbox to create a protected archive:
packing prompts for a password and confirmation for that operation.
Cancelling or submitting an empty password does not create an encrypted archive.
The obsolete `RequirePassword` setting is ignored and removed on configuration save.
When needed, the same masked dialog prompts for an existing archive's password
without confirmation, including when unlocking it for an update.
Cancelling leaves the archive unchanged. Listing a normal encrypted
archive does not require a password; listing original split sets can require
one because combining their fragments involves decoding member data.

An encrypted archive stays encrypted with its current password when updated.
Its current password must be entered for each update operation.
The selected method, volume size, and worker count apply to the entire
rewritten archive, including retained members. To change/remove an existing
password, use AINT's `y -gOLD=NEW` / `y -gOLD=` command.
AIN's legacy "garble" cipher is compatible obfuscation, **not modern secure
encryption**.

## Behavior and limits

- The ANSI WCX interface and Windows ANSI filesystem APIs are used. Paths
  must be representable in the active Windows code page. AIN does not record
  Unicode encodings; Unicode-only filenames and long-path support are not
  advertised. Practical filesystem paths are limited by Windows `MAX_PATH`.
- The existing archive reader uses signed 32-bit file positions on Windows;
  individual archive images must stay below 2 GiB. Member sizes are 32-bit
  format fields. Updates/extraction cache decoded contents in memory, and
  creation also buffers its input. Large solid archives can require several
  times their uncompressed size in RAM, especially in the x86 build.
- Empty directories are not stored. Move-packing is explicitly unsupported;
  source files are never removed.
- Member paths are normalized to relative DOS names. Drive prefixes and
  leading separators are stripped; traversal, device names, ADS, and unsafe
  components are rejected. Extraction uses the destination supplied by TC
  and refuses reparse-point parent directories.
- Extraction checks stored-byte sums and decoded sizes. AIN does not provide
  per-member CRC32 values, so the CRC field in WCX headers is zero. Packed
  sizes in solid archives follow AIN's records: the final member carries
  the compressed size for its span.
- Extraction writes a temporary sibling file and only replaces the requested
  destination after success. DOS modification times and file attributes are
  restored.
- Rewrites are staged beside the destination, on the same filesystem.
  Existing volumes are backed up before promoting new ones. Detected commit
  errors trigger rollback; unrelated colliding volume files are not replaced.
  This is not a crash-atomic multi-file transaction: if rollback itself fails,
  a dialog identifies the retained recovery directory containing `old.ain`
  and its numbered volumes. Do not delete it before recovering your archive.
- WCX progress callbacks run on the calling thread. Cancellation discards
  staged output and preserves the original archive. Compression runs on a
  worker while the caller polls cancellation; an in-flight encoding finishes
  before cancellation returns. Initial solid decoding and input-file reads
  can also delay cancellation.

## Source layout

- `plugin.c`: WCX entry points, member mapping, extraction, and staged rewrites.
- `settings.c`, `ain_wcx.rc`: native dialogs and per-user settings.
- `wcx.h`: ANSI WCX structures, callbacks, and function declarations.
- `ain_wcx*.def`: x64/x86 export names.
- `tests\wcx_test.c`: native WCX host integration tests.
- `..\src`: shared AIN container, codec, cipher, filesystem, and worker code.
