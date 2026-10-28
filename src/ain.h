/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

/* ain.h - AIN 2.32 archive format definitions.
 *
 * Format recovered by reverse engineering ain.exe (see README.md).
 */
#ifndef AIN_H
#define AIN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---------------- archive container ---------------- */

#define AIN_MAGIC        0x21          /* header byte 0                          */
#define AIN_HDR_SIZE     24            /* header size; data area starts here     */
#define AIN_HDRXOR       0x5555        /* header[22..23] = sum(hdr[0..21]) ^ this */
#define AIN_FLAG_GARBLE  0x80          /* header[3] bit: data segments encrypted  */
#define AIN_FLAG_MORE_VOLUMES 0x40     /* original AIN volume sequence continues */

/* multi-volume (this port's own layout, stored in otherwise-unused bytes):
 *   hdr[2]      = 1 when the archive is split into fragment volumes
 *   hdr[4..6]   = u24 fragment size in bytes (max 16 MiB)
 *   hdr[7]      = volume count N (2..255); files are <base>.001 ... <base>.NNN
 * volumes are raw byte slices of the complete archive image */
#define AIN_FLAG_VOLS    0x01          /* header[2] marker                       */

/* record flags, record offset +22 */
#define AIN_F_SPAN_START 0x10          /* first file of a solid span             */
#define AIN_F_SPAN_END   0x08          /* last file; comp/chk cover whole span   */
#define AIN_F_STANDALONE 0x18          /* independent file (own segment)         */
#define AIN_F_MORE_FRAGMENTS 0x20      /* more bytes of this member follow       */

#define AIN_REC_SIZE     29            /* fixed-size record part                 */

/* methods (header byte 1 & 0x0F) */
#define AIN_M_STORE      4             /* raw stored, per-file segments          */
/* methods 1..3: LZ77 codec; multiple files share one solid stream             */

/* ---------------- codec (modules 1dd8 / 1cc9, identical) ---------------- */

#define AIN_PRE         0x100          /* zero bytes of pre-history before out[0] */
#define AIN_WINDOW      0x8000         /* LZ window                               */
#define AIN_MAXDIST     0x7FFF
#define AIN_MINMATCH    3
#define AIN_MAXMATCH    256
#define AIN_SYMS_A      0x110          /* tree A: 0..255 literals, 256..271 dist classes */
#define AIN_SYMS_B      0xFE           /* tree B: length = sym + 3                */
#define AIN_EOF_EXTRA   0x3FFF         /* dist class 15 + extra 0x3FFF = EOF      */

/* ---------------- growable buffer ---------------- */

typedef struct {
    uint8_t *p;
    size_t len, cap;
} AinBuf;

void ain_buf_init(AinBuf *b);
void ain_buf_free(AinBuf *b);
int  ain_buf_reserve(AinBuf *b, size_t extra);   /* ensure len+extra <= cap */
int  ain_buf_put(AinBuf *b, const void *d, size_t n);
int  ain_buf_putc(AinBuf *b, int c);

/* ---------------- codec API ---------------- */

/* Decode one AIN codec stream in[0..inlen) (e.g. an index or a solid span).
 * Returns 0 on success; appends decoded bytes to *out. */
int ain_decode_stream(const uint8_t *in, size_t inlen, AinBuf *out);
/* Decode an original AIN volume's possibly truncated data stream up to the
 * exact decoded byte count recorded in its volume-local index. */
int ain_decode_stream_sized(const uint8_t *in, size_t inlen, AinBuf *out,
                            size_t decoded_size);
/* Decode through the same codec while reporting bit ranges, trees and tokens.
 * Bit offsets in the report are relative to the start of this stream. */
int ain_analyze_stream(const uint8_t *in, size_t inlen, AinBuf *out, FILE *report);

/* Encode bytes into an AIN codec stream (appends to *out).
 * Inputs above INT_MAX bytes return -1; use streaming for larger inputs. */
int ain_encode_stream(const uint8_t *in, size_t inlen, AinBuf *out, int speed);
/* speed: 1 = slow, deeper match search; 2 = normal; 3 = fast, looser search.
 * Speeds 1 and 2 compare complete block costs using alternative trees and
 * a bounded bit-cost reparse. Speed 1 additionally tries one optimal parse
 * under refined estimated code costs (128 candidates/position).
 * All levels use the same decoder-compatible format. */

/* Streaming multi-block encoder: feed bytes incrementally; every `chunk`
 * raw bytes close one Huffman block via the format's tree-reload marker
 * (the decoder re-reads both trees, window history continues).  Output is
 * appended to the AinBuf given at open; the caller may drain whole bytes
 * from it between ain_enc_feed() calls. Scratch uses four bytes per pending
 * input byte for tokens plus a fixed AIN_WINDOW-entry chain table.
 * Speeds 1 and 2 also use temporary compressed-block buffers to select the
 * smallest encoding, including tree headers and block markers.
 * Speed 1 additionally uses 12 bytes per pending input byte for optimal
 * parse costs and predecessor tokens, plus fixed chain-table scratch.
 * chunk is clamped to at least 65536; oversized chunks return NULL. */
typedef struct AinEncS AinEncS;
AinEncS *ain_enc_open(AinBuf *out, int speed, size_t chunk);
/* Parallel streaming encoder. threads: 1 = serial, 2..256 = explicit pool,
 * 0 = automatic (up to 256, with a 256 MiB estimated worker-memory budget).
 * The legacy open above remains serial. Pools start at the first full block.
 * Memory scales with threads * chunk; output is byte-identical to serial.
 * feed may defer output until later calls; finish drains all outstanding jobs.
 * Only the calling thread may access out; free joins any outstanding workers.
 * Calls on a single encoder must not overlap. Failures are returned, not
 * silently retried serially. */
AinEncS *ain_enc_open_mt(AinBuf *out, int speed, size_t chunk, unsigned threads);
int      ain_enc_feed(AinEncS *e, const uint8_t *p, size_t n);
int      ain_enc_finish(AinEncS *e);     /* closes last block + bit flush */
/* Encode trial prefixes of the same input using cached full blocks.
 * Use a fresh encoder per input; its output buffer holds the private cache.
 * Trials may grow or shrink; out must differ from the cache buffer. */
int      ain_enc_prefix(AinEncS *e, const uint8_t *input, size_t size, AinBuf *out);
/* Discard the prefix cache for a new input, retaining the idle worker pool. */
int      ain_enc_reset_prefix(AinEncS *e);
/* Consume a block-aligned prefix and retain encoded look-ahead for the next
 * independent stream. input covers all cached blocks, before consumption.
 * Non-block-aligned consumption discards the cache, like reset_prefix. */
int      ain_enc_rebase_prefix(AinEncS *e, const uint8_t *input, size_t consumed);
/* Select the longest completed-block prefix within limit encoded bytes.
 * Returns its input length in used (zero if no block fits), closing with EOF. */
int      ain_enc_prefix_blocks(AinEncS *e, const uint8_t *input, size_t size,
                               size_t limit, size_t *used, AinBuf *out);
void     ain_enc_free(AinEncS *e);

/* ---------------- garble (password) cipher, see garble.c ---------------- */

/* Resumable keystream state: the region is a continuous sequence of LE
 * keystream words; when a slice ends mid-word the high byte waits in
 * `hi` for the next slice (dropped only at the true region end)         */
typedef struct {
    uint16_t a, b, c;       /* 48-bit chain state                        */
    uint16_t hi;            /* pending high byte of the last word        */
    int hi_pending;
} AinGarbleSt;

void     ain_garble_seed(const char *pw, uint16_t seed[3]);
uint16_t ain_garble_hash(const char *pw);   /* stored in header[4..5] (LE) */
void     ain_garble_xor(const uint16_t seed[3], uint8_t *p, size_t n);
void     ain_garble_init(AinGarbleSt *g, const uint16_t seed[3]);
void     ain_garble_run(AinGarbleSt *g, uint8_t *p, size_t n);  /* streaming */

/* ---------------- container API ---------------- */

extern unsigned ain_quiet_level;
/* Suppress status when quiet level reaches the given threshold (1 or 2).
 * Errors, prompts and requested listing/analysis output bypass this helper. */
void ain_status(unsigned threshold, const char *format, ...);

typedef struct {
    const char *label;
    uint64_t total, processed, reported_bytes;
    unsigned percent, reported_percent;
} AinProgress;

void ain_progress_begin(AinProgress *p, const char *label, uint64_t total);
void ain_progress_advance(AinProgress *p, size_t bytes);
void ain_progress_finish(AinProgress *p);
int ain_size_summary(const char *path, uint64_t uncompressed, double started,
                     AinProgress *progress);

typedef struct {
    uint8_t attr;
    uint32_t ts;            /* packed DOS date/time     */
    uint32_t orig, comp, off;
    uint32_t f22;           /* span flags               */
    uint16_t chk;           /* checksum of stored bytes */
    const char *name;       /* points into index buffer */
} AinRec;

typedef struct {
    const char *path;
    uint8_t *img;
    size_t n;
    int method;             /* 1..4                     */
    int garbled;            /* header[3] & 0x80         */
    uint16_t gseed[3];      /* keystream seed, from ain_set_password  */
    int gseed_ok;           /* 1 = data region decrypted              */
    uint8_t *enc;           /* encrypted data-region copy (garbled),  */
                            /* kept so checksums verify correctly     */
    uint32_t frag;          /* fragment size (0 = single volume)      */
    uint16_t nvols;         /* number of volumes (1 if not fragmented)*/
    uint8_t native_volumes; /* normalized original AIN volume set     */
    uint8_t native_method;  /* method used by the original volume set  */
    uint64_t native_comp;   /* compressed bytes across original volumes */
    uint32_t count;
    uint32_t idxoff;
    AinBuf idx;
    AinRec *recs;
    int nrecs;
} AinArchive;

typedef struct {
    char *name;
    uint8_t *data;
    size_t n;
    uint32_t ts;
    uint8_t attr;
} AinEnt;

int  ain_open(AinArchive *a, const char *path);
void ain_close(AinArchive *a);

/* verify a password against header[4..5]; on match decrypts the data region
 * of a->img in place (one keystream pass, index is never encrypted).
 * Plain archives return 0. Already-unlocked archives return 0.
 * Returns -1 on wrong password ("Incorrect password"). */
int  ain_set_password(AinArchive *a, const char *pw);

int  ain_header_check(AinArchive *a);
int  ain_load_index(AinArchive *a);
int  ain_list(AinArchive *a, FILE *out, int verbose,
              const char **pats, int npats);
int  ain_analyze(AinArchive *a, FILE *out, const char **pats, int npats);
/* Existing targets prompt on stdin unless yes is set; nonzero on abort/error.
 * Byte progress for selected members (including skips) is written to stderr. */
int  ain_extract(AinArchive *a, const char *dest,
                 const char **pats, int npats, int flat, int yes);
int  ain_test(AinArchive *a, const char **pats, int npats);

/* entry-based container writer: builds a complete archive from in-memory
 * file entries (used by create/update/delete). frag>0 writes indexed volumes
 * <base>.AIN, .001..NNN. n=0 writes a single empty archive.
 * speed: encoder effort 1..3.
 * pw != NULL && *pw: garble the data region, set header[3] bit 0x80 and
 * header[4..5] = password hash; member checksums then cover the
 * *encrypted* stored bytes (the original's behavior). */
int  ain_write(const char *archpath, int method, const AinEnt *ents, int n,
               unsigned long long frag, int speed, const char *pw);
/* Entry-based rewrite with the same worker control as streaming creation. */
int  ain_write_mt(const char *archpath, int method, const AinEnt *ents, int n,
                  unsigned long long frag, int speed, const char *pw,
                  unsigned threads);
/* convenience: read the named files from disk, then ain_write */
int  ain_create(const char *archpath, int method, const char **names, int nfiles,
                unsigned long long frag, int speed, const char *pw);
/* Streaming creation reports uncompressed input-byte progress on stderr. */
int  ain_create_mt(const char *archpath, int method, const char **names, int nfiles,
                   unsigned long long frag, int speed, const char *pw,
                   unsigned threads);
/* decode every member into newly allocated AinEnt entries */
int  ain_load_entries(AinArchive *a, AinEnt **out, int *nout);
void ain_free_entries(AinEnt *e, int n);

int  ain_set_dostime(const char *path, uint32_t ts);
uint32_t ain_file_dostime(const char *path);
int  ain_read_file(const char *path, AinEnt *out);   /* allocate owned entry */
int  ain_read_file_progress(const char *path, AinEnt *out, uint32_t expected_size,
                            AinProgress *progress);
/* DOS-style case-insensitive wildcard match (* ?, separators included) */
int  ain_match(const char *pat, const char *str);
/* does member/file `name` match filter `pat` (optionally under `base`)? */
int  ain_filter_match(const char *pat, const char *base, const char *name);

#endif /* AIN_H */
