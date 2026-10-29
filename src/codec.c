/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

/* codec.c - AIN LZ77/Huffman codec.
 *
 * Decoder: based on an instruction-by-instruction port of ain.exe module 1dd8:
 *   020c bit reader | 02c0 symbol decode | 0379 CLC loader | 03f8 tree loader
 *   04a0 + 05c0 + 0555 table builder | 00cc main loop.
 * Module 1cc9 (per-file / solid data streams) behaves identically, verified
 * bit-exactly against 21 archives created by ain.exe 2.32.
 * The lookup width and hot loop are optimized without changing code order.
 *
 * Encoder: writes streams that the original decoder accepts.  Canonical
 * Huffman codes are assigned in (length asc, symbol desc) order - the order
 * in which the original table builder hands codes to symbols - and codes
 * travel MSB-first; all other fields travel LSB-first.
 */
#include "ain.h"
#include "thread.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#if defined(__aarch64__) || defined(_M_ARM64)
#define AIN_NEON 1
#ifdef _MSC_VER
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define AIN_AVX2 1
#include <immintrin.h>
#else
#error "this codec requires x86 AVX2 or little-endian ARM64 NEON"
#endif
#if defined(__AARCH64EB__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "this codec requires a little-endian target"
#endif
#ifdef _MSC_VER
#include <intrin.h>       /* _BitScanForward, _BitScanForward64            */
#define AIN_NOINLINE __declspec(noinline)
#define AIN_FORCEINLINE __forceinline
#else
#define AIN_NOINLINE __attribute__((noinline))
#define AIN_FORCEINLINE inline __attribute__((always_inline))
#endif

/* ================= growable buffer ================= */

/* optional insertion-hash trace (only active under AIN_TRACE, e.g. in the
 * A/B harness; product builds compile this to nothing) */
#ifdef AIN_TRACE
extern int *ain_hrec;
extern size_t ain_hrec_n;
#define AIN_TR(p, hv) do { if ((size_t)(p) < ain_hrec_n) ain_hrec[(p)] = (hv); } while (0)
#else
#define AIN_TR(p, hv) ((void)0)
#endif

void ain_buf_init(AinBuf *b) { b->p = NULL; b->len = b->cap = 0; }

void ain_buf_free(AinBuf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

int ain_buf_reserve(AinBuf *b, size_t extra)
{
    size_t need, cap;
    uint8_t *np;
    if (extra > SIZE_MAX - b->len)
        return -1;
    need = b->len + extra;
    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 4096;
    while (cap < need) {
        if (cap > SIZE_MAX / 2 || (uint64_t)cap > (UINT64_C(1) << 40))
            return -1;
        cap *= 2;
    }
    np = (uint8_t *)realloc(b->p, cap);
    if (!np)
        return -1;
    b->p = np;
    b->cap = cap;
    return 0;
}

int ain_buf_put(AinBuf *b, const void *d, size_t n)
{
    if (ain_buf_reserve(b, n))
        return -1;
    memcpy(b->p + b->len, d, n);
    b->len += n;
    return 0;
}

int ain_buf_putc(AinBuf *b, int c)
{
    uint8_t v = (uint8_t)c;
    return ain_buf_put(b, &v, 1);
}

/* ================= bit reader (1dd8:020c) ================= */

typedef struct {
    const uint8_t *d;
    size_t n, pos;
    size_t filled_bits;
    uint64_t buf;
    int cnt;
} BitReader;

static void br_init(BitReader *br, const uint8_t *d, size_t n)
{
    br->d = d;
    br->n = n;
    br->pos = 0;
    br->filled_bits = 0;
    br->buf = 0;
    br->cnt = 0;
}

/* Bulk refill; inserts four new bytes above valid bits, bit-for-bit the
 * same order as the original byte refill (02c0/020c).  Only called while
 * cnt <= 32, so the 32-bit chunk can never shift bits out of the 64-bit
 * buffer; bytes past the end are zeros, exactly like br_fill_byte gave. */
static AIN_FORCEINLINE void br_refill32(BitReader *br)
{
    uint32_t v = 0;
    size_t rem = br->n - br->pos;
    if (rem >= 4) {
        memcpy(&v, br->d + br->pos, 4);
        br->pos += 4;
    } else {
        size_t k;
        for (k = 0; k < rem; k++)
            v |= (uint32_t)br->d[br->pos + k] << (8 * k);
        br->pos = br->n;
    }
    br->buf |= (uint64_t)v << br->cnt;
    br->cnt += 32;
    br->filled_bits += 32;
}

/* max n here is 14 (dist class 15 extra), so a single refill suffices */
static AIN_FORCEINLINE uint32_t br_read(BitReader *br, int n)
{
    uint32_t v;
    if (n == 0)
        return 0;
    if (br->cnt < n)
        br_refill32(br);
    v = (uint32_t)(br->buf & (((uint64_t)1 << n) - 1));
    br->buf >>= n;
    br->cnt -= n;
    return v;
}

/* ================= tree builder (based on 04a0+05c0+0555) ================ */

#define TREE_LOOKUP_BITS 10

typedef struct {
    uint16_t lookup[1 << TREE_LOOKUP_BITS]; /* 0 = empty */
    uint16_t nodes[1024];      /* node area, word indexed (byte off >> 1)    */
    unsigned lookup_mask;
    const int *lens;
    int nsyms;
} Tree;

static uint16_t rol16(uint16_t v, int c)
{
    c &= 15;
    if (!c)
        return v;
    return (uint16_t)((v << c) | (v >> (16 - c)));
}

/* One frame of the explicit stack that emulates the original's recursive
 * table filler (0555); the fields carry the values the original kept in
 * registers at each recursion level. */
typedef struct {
    int task;             /* TASK_FILL: place next leaf here;
                           * TASK_NODE: allocate an internal node subtree  */
    int depth2;           /* 2 * current code depth (original BX)          */
    int node_off;         /* byte offset of this slot in t->nodes (DI)     */
    uint16_t revcode;     /* reversed code pattern (AX): the bit to write
                           * next sits in bit 15, shifting right per level */
} BuildFrame;

#define TASK_FILL 0
#define TASK_NODE 1

static int tree_build(Tree *t, const int *lens, int n)
{
    int len_hist[17], i, kraft, kraft_prev;
    int remaining[17], sym_cur;
    int order[AIN_SYMS_A];
    int node_next;                     /* allocation cursor in node arena */
    int lookup_bits = 8;
    BuildFrame stack[64];
    int sp = 0;

    if (n > AIN_SYMS_A)
        return -4;                     /* all callers pass <= AIN_SYMS_A */

    memset(t->nodes, 0, sizeof t->nodes);
    t->lens = lens;
    t->nsyms = n;

    memset(len_hist, 0, sizeof len_hist);
    for (i = 0; i < n; i++)
        len_hist[lens[i] < 0x11 ? lens[i] : 0]++;
    /* Avoid the larger table's setup cost when every code already fits. */
    for (i = 9; i <= 15; i++)
        if (len_hist[i])
            lookup_bits = TREE_LOOKUP_BITS;
    t->lookup_mask = (1u << lookup_bits) - 1;
    memset(t->lookup, 0, (t->lookup_mask + 1) * sizeof t->lookup[0]);
    if (len_hist[0] == n)
        return -1;                       /* empty tree */
    kraft = 0;
    for (i = 1; i <= 15; i++) {          /* length 16 excluded, as in original */
        kraft_prev = kraft;
        kraft = (kraft + (len_hist[i] << (16 - i))) & 0xFFFF;
        if (kraft < kraft_prev && kraft != 0)
            return -2;                   /* over-full */
    }
    if (kraft != 0)
        return -3;                       /* incomplete (Kraft != 1) */

    /* order[] exactly as the original's 05c0 bucket sort (linked lists
     * head-inserted backwards = stable sym-asc within bucket, walked key-
     * asc), reproduced with a stable counting sort on the same key, no
     * malloc and no pointer chasing.  Pops start at n - len_hist[0],
     * which also counts invalid-length symbols, so the FULL array must be
     * faithful: valid trees never touch the spill region, crafted ones
     * must see identical bytes. */
    {
        int cnt2[256], base2[256], k;
        memset(cnt2, 0, sizeof cnt2);
        for (i = 0; i < n; i++)
            cnt2[(~lens[i]) & 0xFF]++;
        k = 0;
        for (i = 0; i < 256; i++) { base2[i] = k; k += cnt2[i]; }
        for (i = 0; i < n; i++)
            order[base2[(~lens[i]) & 0xFF]++] = i;
        (void)k;
    }
    {
        /* fill: pop order is (len asc, sym desc) - same as original */
        memcpy(remaining, len_hist, sizeof remaining);
        remaining[0] = 0;
        sym_cur = n - len_hist[0];
        node_next = 2;                   /* first allocated node slot */
        stack[sp].task = TASK_NODE; stack[sp].depth2 = 0;
        stack[sp].node_off = 0; stack[sp].revcode = 0; sp++;
        while (sp) {
            int task, depth2, node_off; uint16_t revcode;
            sp--;
            task = stack[sp].task; depth2 = stack[sp].depth2;
            node_off = stack[sp].node_off; revcode = stack[sp].revcode;
            if (task == TASK_FILL) {     /* fill() */
                int codelen = depth2 >> 1;
                if (codelen > 15)
                    return -3;           /* runaway walk: not completable */
                if (--remaining[codelen] < 0) {
                    /* no symbol left of this length: branch instead */
                    stack[sp].task = TASK_NODE; stack[sp].depth2 = depth2;
                    stack[sp].node_off = node_off; stack[sp].revcode = revcode;
                    sp++;
                    continue;
                } else {
                    int sym = order[--sym_cur];
                    t->nodes[node_off >> 1] = (uint16_t)((-sym) & 0xFFFF);
                    if (depth2 > 2 * lookup_bits)
                        continue;
                    {   /* replicate the leaf entry over every lookup prefix
                         * that ends with this reversed code */
                        int rot = (depth2 + 2) >> 1, off;
                        uint16_t slot = rol16(revcode, rot);
                        uint16_t entry = (uint16_t)(sym | ((8 * codelen) << 8));
                        int stride = 1 << rot;
                        for (off = slot; off < 2 * (1 << lookup_bits); off += stride)
                            t->lookup[off >> 1] = entry;
                    }
                }
            } else {                     /* TASK_NODE: node_path() */
                int new_off, child_base;
                new_off = node_next;
                t->nodes[node_off >> 1] = (uint16_t)new_off;
                if (depth2 == 2 * lookup_bits) {
                    t->lookup[revcode >> (16 - lookup_bits)] =
                        (uint16_t)(new_off | 0x8000 | (lookup_bits << 11));
                    new_off = (new_off & 0xFF) | (((new_off >> 8) & 7) << 8);
                    node_next = new_off;
                }
                child_base = new_off;    /* 0-branch slot; 1-branch is +2 */
                node_next = new_off + 4;
                stack[sp].task = TASK_FILL; stack[sp].depth2 = depth2 + 2;
                stack[sp].node_off = child_base + 2;
                stack[sp].revcode = (uint16_t)((revcode >> 1) | 0x8000); sp++;
                stack[sp].task = TASK_FILL; stack[sp].depth2 = depth2 + 2;
                stack[sp].node_off = child_base;
                stack[sp].revcode = (uint16_t)(revcode >> 1); sp++;
            }
        }
    }
    return 0;
}

/* Original symbol ordering, with a wider lookup before the node walk. */
static AIN_FORCEINLINE int tree_decode(const Tree *t, BitReader *br)
{
    uint16_t entry;
    int node_slot;
    if (br->cnt < TREE_LOOKUP_BITS)
        br_refill32(br);
    entry = t->lookup[br->buf & t->lookup_mask];
    if (entry == 0)
        return -1;                       /* unmapped slot */
    {
        int ln = (entry >> 11) & 15;
        br->buf >>= ln;
        br->cnt -= ln;
    }
    if (entry < 0x8000) {                /* short code: sym | len<<11 */
        return entry & 0x7FF;
    }
    node_slot = entry & 0x7FF;           /* deep: node walk */
    for (;;) {
        uint32_t bit;
        if (!br->cnt)
            br_refill32(br);
        bit = (uint32_t)(br->buf & 1);
        br->buf >>= 1;
        br->cnt -= 1;
        /* 0-branch = node's own slot, 1-branch = slot + 2 */
        node_slot = (node_slot + 1 + (int)bit) & ~1;
        {
            int v = (int16_t)t->nodes[node_slot >> 1];
            if (v <= 0)
                return -v;               /* negative = leaf (-symbol) */
            node_slot = v;
        }
    }
}

/* ================= tree loaders (0379 / 03f8) ================= */

static void load_clc(BitReader *br, int *lens /* 19 entries */)
{
    int tail, cnt = 19, i;
    tail = (int)br_read(br, 5);          /* trailing lengths left zero */
    for (i = 0; i < cnt; i++)
        lens[i] = 0;
    if (tail >= cnt)
        return;
    for (i = 0; i < cnt - tail; i++) {
        int v = (int)br_read(br, 3);
        if (v > 6)
            while (br_read(br, 1))
                v++;
        lens[i] = v;
    }
}

static int load_tree(BitReader *br, int count, int *lens)
{
    int clc_lens[19], tail, i, limit;
    Tree clc;
    load_clc(br, clc_lens);
    if (tree_build(&clc, clc_lens, 19) != 0)
        return -1;
    tail = (int)br_read(br, 9);          /* trailing lengths left zero */
    for (i = 0; i < count; i++)
        lens[i] = 0;
    if (tail >= count)
        return 0;
    limit = count - tail;
    i = 0;
    while (i < limit) {
        int sym = tree_decode(&clc, br);
        if (sym < 0)
            return -1;
        if (sym == 0)
            i += 1;
        else if (sym == 1)
            i += (int)br_read(br, 4) + 3;
        else if (sym == 2)
            i += (int)br_read(br, 9) + 0x14;
        else {
            lens[i] = sym - 2;
            i += 1;
        }
    }
    return 0;
}

/* overlap-safe LZ77 copy; len <= AIN_MAXMATCH and dst has >= 256 bytes of
 * writable slack. SIMD broadcasts 2/4/8/16-byte periods and copies long-distance
 * or vector-period-aligned matches in vector steps (up to 31 bytes past len).
 * Other short distances retain pattern doubling to avoid store-forwarding stalls.
 * Compiler barriers prevent /O2 from moving overlapping loads before the stores
 * they depend on, as happened in an earlier implementation. The fallback uses non-overlapping
 * memcpy calls to materialize and then double one period.
 * A one-byte period needs no dependent copies: memset broadcasts it. */
static void lz_copy(uint8_t *dst, size_t dist, int len)
{
    if (dist == 1) {
        memset(dst, dst[-1], (size_t)len);
        return;
    }
#ifdef AIN_NEON
    if (len >= 16 && dist <= 16 && (dist & (dist - 1)) == 0) {
        uint8x16_t pattern;
        int i;
        if (dist == 2) {
            uint16_t v;
            memcpy(&v, dst - 2, sizeof v);
            pattern = vreinterpretq_u8_u16(vdupq_n_u16(v));
        } else if (dist == 4) {
            uint32_t v;
            memcpy(&v, dst - 4, sizeof v);
            pattern = vreinterpretq_u8_u32(vdupq_n_u32(v));
        } else if (dist == 8) {
            uint64_t v;
            memcpy(&v, dst - 8, sizeof v);
            pattern = vreinterpretq_u8_u64(vdupq_n_u64(v));
        } else {
            pattern = vld1q_u8(dst - 16);
        }
        for (i = 0; i < len; i += 16)
            vst1q_u8(dst + i, pattern);
        return;
    }
    if (len >= 16 && (dist >= 128 || (dist & 15) == 0)) {
        int i;
        for (i = 0; i < len; i += 16) {
            uint8x16_t v = vld1q_u8(dst + i - dist);
            vst1q_u8(dst + i, v);
#ifdef _MSC_VER
            _ReadWriteBarrier();
#else
            __asm__ __volatile__("" ::: "memory");
#endif
        }
        return;
    }
#elif defined(AIN_AVX2)
    if (len >= 32 && dist <= 16 && (dist & (dist - 1)) == 0) {
        __m256i pattern;
        int i;
        if (dist == 2) {
            uint16_t v;
            memcpy(&v, dst - 2, sizeof v);
            pattern = _mm256_set1_epi16((short)v);
        } else if (dist == 4) {
            uint32_t v;
            memcpy(&v, dst - 4, sizeof v);
            pattern = _mm256_set1_epi32((int)v);
        } else if (dist == 8) {
            pattern = _mm256_broadcastq_epi64(_mm_loadl_epi64((const __m128i *)(dst - 8)));
        } else {
            pattern = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)(dst - 16)));
        }
        for (i = 0; i < len; i += 32)
            _mm256_storeu_si256((__m256i *)(dst + i), pattern);
        return;
    }
    if (len >= 32 && (dist >= 128 || (dist & 31) == 0)) {
        int i;
        for (i = 0; i < len; i += 32) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(dst + i - dist));
            _mm256_storeu_si256((__m256i *)(dst + i), v);
            /* Later loads may depend on this store when the match overlaps. */
#ifdef _MSC_VER
            _ReadWriteBarrier();
#else
            __asm__ __volatile__("" ::: "memory");
#endif
        }
        return;
    }
#endif
    int done = len < (int)dist ? len : (int)dist;
    memcpy(dst, dst - dist, (size_t)done);
    while (done < len) {
        int take = len - done < done ? len - done : done;
        memcpy(dst + done, dst, (size_t)take);
        done += take;
    }
}

/* ================= decoder main loop (1dd8:00cc) ================= */

typedef struct {
    size_t start, data_start, output_start;
    size_t literals, matches, match_bytes, literal_bits, match_bits;
    size_t distances[16], lengths[AIN_MAXMATCH + 1];
    int max_dist, max_len;
} DecodeBlock;

static size_t br_bitpos(const BitReader *br)
{
    return br->filled_bits - (size_t)br->cnt;
}

static void report_tree(FILE *out, const char *name, const int *lens, int n)
{
    int hist[17] = { 0 }, i, active = 0;
    for (i = 0; i < n; i++) {
        if (lens[i] > 0 && lens[i] <= 16) {
            hist[lens[i]]++;
            active++;
        }
    }
    fprintf(out, "    Tree %s: %d/%d symbols; code lengths (bits:count):",
            name, active, n);
    for (i = 1; i <= 16; i++)
        if (hist[i])
            fprintf(out, " %d:%d", i, hist[i]);
    fprintf(out, "\n");
}

static void report_block(FILE *out, size_t number, const DecodeBlock *b,
                         size_t end, size_t output_end, int eof,
                         const int *lensA, const int *lensB)
{
    int i;
    fprintf(out, "  Block %zu: bits [%zu,%zu), decoded [%zu,%zu), %s\n",
            number, b->start, end, b->output_start, output_end,
            eof ? "EOF" : "tree reload");
    fprintf(out, "    Bits: trees=%zu literals=%zu matches=%zu marker=%zu\n",
            b->data_start - b->start, b->literal_bits, b->match_bits,
            end - b->data_start - b->literal_bits - b->match_bits);
    report_tree(out, "A (literals/distance classes)", lensA, AIN_SYMS_A);
    report_tree(out, "B (match lengths)", lensB, AIN_SYMS_B);
    fprintf(out, "    Tokens: literals=%zu matches=%zu matched bytes=%zu"
            " max distance=%d max length=%d\n",
            b->literals, b->matches, b->match_bytes, b->max_dist, b->max_len);
    fprintf(out, "    Distance classes (class:count):");
    for (i = 0; i < 16; i++)
        if (b->distances[i])
            fprintf(out, " %d:%zu", i, b->distances[i]);
    fprintf(out, "\n    Match lengths (bytes:count):");
    for (i = AIN_MINMATCH; i <= AIN_MAXMATCH; i++)
        if (b->lengths[i])
            fprintf(out, " %d:%zu", i, b->lengths[i]);
    fprintf(out, "\n");
}

/* Specialize the token loop for reporting and sized/unsized callers. */
static AIN_FORCEINLINE int decode_stream(const uint8_t *in, size_t inlen, AinBuf *out,
                         FILE *report, size_t expected)
{
    BitReader br;
    int lensA[AIN_SYMS_A], lensB[AIN_SYMS_B];
    Tree tA, tB;
    size_t base;
    size_t budget;
    uint8_t *op;                         /* out->p cached; refreshed on growth */
    size_t len, cap;
    DecodeBlock block;
    size_t blocks = 0, literals = 0, matches = 0, matched = 0;

    if (expected == 0)
        return 0;
    br_init(&br, in, inlen);
    if (ain_buf_reserve(out, AIN_PRE + 512))
        return -1;
    base = out->len;
    memset(out->p + base, 0, AIN_PRE);   /* zero pre-history, see ain.h */
    len = out->len + AIN_PRE;
    op = out->p;
    cap = out->cap;

    budget = inlen * 16 + 4096;          /* every token costs >= 1 bit */

    {
        unsigned init = br_read(&br, 1); /* discarded init bit */
        if (report)
            fprintf(report, "  Codec: init bit=%u (ignored), zero prehistory=%d bytes,"
                    " window=%d bytes, match lengths=%d..%d\n",
                    init, AIN_PRE, AIN_WINDOW, AIN_MINMATCH, AIN_MAXMATCH);
    }
    for (;;) {                           /* block loop */
        if (report) {
            memset(&block, 0, sizeof block);
            block.start = br_bitpos(&br);
            block.output_start = len - base - AIN_PRE;
            blocks++;
        }
        if (load_tree(&br, AIN_SYMS_A, lensA) ||
            tree_build(&tA, lensA, AIN_SYMS_A)) {
            out->len = base;
            return -10;                  /* bad tree A */
        }
        if (load_tree(&br, AIN_SYMS_B, lensB) ||
            tree_build(&tB, lensB, AIN_SYMS_B)) {
            out->len = base;
            return -11;                  /* bad tree B */
        }
        if (report)
            block.data_start = br_bitpos(&br);
        for (;;) {
            int sym, k, dist, length, extra;
            size_t token_start = report ? br_bitpos(&br) : 0;
            if (expected != SIZE_MAX) {
                size_t produced = len - base - AIN_PRE;
                if (produced == expected)
                    goto done;
                if (produced > expected) {
                    out->len = base;
                    return -19;
                }
            }
            if (!budget--) {
                out->len = base;
                return -17;              /* runaway stream */
            }
            if (len + 512 > cap) {       /* slack: 1 literal + 1 full match */
                out->len = len;
                if (ain_buf_reserve(out, 4096)) {
                    out->len = base;
                    return -16;
                }
                op = out->p;
                cap = out->cap;
            }
            sym = tree_decode(&tA, &br);
            if (sym < 0) { out->len = base; return -12; }
            if (sym < 256) {
                op[len++] = (uint8_t)sym;
                if (report) {
                    block.literals++;
                    block.literal_bits += br_bitpos(&br) - token_start;
                    literals++;
                }
                continue;
            }
            k = sym - 256;
            if (k <= 1) {
                dist = k + 1;
            } else {
                extra = (int)br_read(&br, k - 1);
                if (extra == AIN_EOF_EXTRA) {
                    int eof = (int)br_read(&br, 1);
                    if (report) {
                        if (br_bitpos(&br) / 8 > inlen ||
                            (br_bitpos(&br) / 8 == inlen && br_bitpos(&br) % 8))
                            return -18;
                        report_block(report, blocks, &block, br_bitpos(&br),
                                     len - base - AIN_PRE, eof, lensA, lensB);
                    }
                    if (eof)
                        goto done;
                    break;               /* flag 0: reload both trees */
                }
                dist = ((1 << (k - 1)) | extra) + 1;
            }
            length = tree_decode(&tB, &br);
            if (length < 0) { out->len = base; return -14; }
            length += AIN_MINMATCH;
            if ((size_t)dist > len - base) {
                out->len = base;
                return -15;              /* beyond window + zero pre-history */
            }
            lz_copy(op + len, (size_t)dist, length);
            len += (size_t)length;
            if (expected != SIZE_MAX && len - base - AIN_PRE > expected) {
                out->len = base;
                return -19;
            }
            if (report) {
                block.matches++;
                block.match_bytes += (size_t)length;
                block.match_bits += br_bitpos(&br) - token_start;
                block.distances[k]++;
                block.lengths[length]++;
                if (dist > block.max_dist) block.max_dist = dist;
                if (length > block.max_len) block.max_len = length;
                matches++;
                matched += (size_t)length;
            }
        }
    }
done:
    len -= AIN_PRE;
    memmove(op + base, op + base + AIN_PRE, len - base);
    out->len = len;
    if (report) {
        size_t used = br_bitpos(&br);
        size_t bytes = used / 8 + (used % 8 != 0);
        fprintf(report, "  Stream totals: blocks=%zu decoded=%zu bytes"
                " literals=%zu matches=%zu matched bytes=%zu\n",
                blocks, len - base, literals, matches, matched);
        fprintf(report, "  Encoded: %zu bytes, consumed=%zu bits (%zu bytes),"
                " final-byte padding=%zu bits, trailing=%zu bytes\n",
                inlen, used, bytes, (8 - used % 8) % 8, inlen - bytes);
        if (len > base)
            fprintf(report, "  Stored/decoded ratio: %.2f%% (includes trees and markers)\n",
                    100.0 * (double)inlen / (double)(len - base));
        else
            fprintf(report, "  Stored/decoded ratio: n/a (empty decoded stream)\n");
    }
    return 0;
}

int ain_decode_stream(const uint8_t *in, size_t inlen, AinBuf *out)
{
    return decode_stream(in, inlen, out, NULL, SIZE_MAX);
}

int ain_decode_stream_sized(const uint8_t *in, size_t inlen, AinBuf *out,
                            size_t decoded_size)
{
    return decode_stream(in, inlen, out, NULL, decoded_size);
}

int ain_analyze_stream(const uint8_t *in, size_t inlen, AinBuf *out, FILE *report)
{
    size_t base = out->len;
    int rc = decode_stream(in, inlen, out, report, SIZE_MAX);
    if (rc) {
        out->len = base;
        fprintf(report, "  ERROR: invalid codec stream (%d)\n", rc);
    }
    return rc || ferror(report) ? -1 : 0;
}

/* ================= bit writer (mirror of reader, LSB-first) ================ */

typedef struct { uint64_t buf; AinBuf *out; int cnt; int err; } BitWriter;

static void bw_init(BitWriter *w, AinBuf *out)
{
    w->buf = 0;
    w->cnt = 0;
    w->out = out;
    w->err = 0;
}

static int AIN_NOINLINE bw_grow(BitWriter *w)
{
    if (ain_buf_reserve(w->out, 128)) {
        w->err = 1;
        return -1;
    }
    return 0;
}

static AIN_FORCEINLINE void bw_spill(BitWriter *w)
{
    AinBuf *o = w->out;
    uint32_t lo = (uint32_t)w->buf;
    if (o->cap - o->len < 4 && bw_grow(w))
        return;
    memcpy(o->p + o->len, &lo, 4);
    o->len += 4;
    w->buf >>= 32;
    w->cnt -= 32;
}

static __inline void bw_write(BitWriter *w, int n, uint32_t v)
{
    if (n == 0 || w->err)
        return;
    w->buf |= (uint64_t)v << w->cnt;
    w->cnt += n;
    /* cnt is at most 31 + 32 = 63 bits here (block assembly writes 32),
     * so one spill always suffices and the 64-bit accumulator cannot lose
     * bits; flushing only at >= 32 keeps every spilled byte fully valid */
    if (w->cnt >= 32)
        bw_spill(w);
}

static void bw_flush(BitWriter *w)
{
    if (w->err)
        return;
    /* drain the remaining < 32 bits; bytes leave in the same increasing
     * order as the old eager per-byte writer, and the final partial byte
     * carries the same zero padding (nothing above cnt was ever set) */
    while (w->cnt > 0) {
        if (ain_buf_putc(w->out, (int)(w->buf & 0xFF))) {
            w->err = 1;
            return;
        }
        w->buf >>= 8;
        w->cnt -= 8;
    }
    w->buf = 0;                  /* normalize: all valid bits are out, the
                                  * rest was padding; leaves the same 0/0
                                  * end state as the original writer */
    w->cnt = 0;
}

#if defined(__GNUC__) || defined(__clang__)
    #define bswap32(x) __builtin_bswap32(x)
#elif defined(_MSC_VER)
    #include <stdlib.h>
    #define bswap32(x) _byteswap_ulong(x)
#endif

static uint32_t bitrev(uint32_t v, int n)
{
    /* Reverse all 32 bits (SWAR + hardware byte swap) and keep the top n:
     * in-bit i lands at 32-1-i, the shift puts it at n-1-i, which is the
     * per-bit result of the original loop; bits above n-1 shift out. */
    uint32_t r;
    if (n == 0)
        return 0;
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);

    r = bswap32(v) >> (32 - n);
    return r;
}

/* canonical codes matching the original builder's assignment:
 * lengths ascending; within a length, LARGER symbol gets the SMALLER code. */
static void canon_codes(const int *lens, int n, uint32_t *code)
{
    int cnt[17], i, l;
    uint32_t next[18];
    memset(cnt, 0, sizeof cnt);
    memset(code, 0, (size_t)n * sizeof *code);
    for (i = 0; i < n; i++)
        if (lens[i] > 0 && lens[i] <= 16)
            cnt[lens[i]]++;
    next[1] = 0;
    for (l = 1; l < 16; l++)
        next[l + 1] = (next[l] + (uint32_t)cnt[l]) << 1;
    /* The original re-scanned the array once per length (16 passes); the
     * per-length counters are independent, so ONE backward pass assigns
     * each next[l]++ in the same sym-descending order per length. */
    for (i = n - 1; i >= 0; i--) {
        int l2 = lens[i];
        if (l2 > 0 && l2 <= 16)
            code[i] = next[l2]++;
    }
}

/* ---- length assignment: Shannon-Fano split with capacity guard.
 * Produces a complete (Kraft == 1) tree with max depth maxdepth. */
static void sf_rec(const int *syms, const uint64_t *fw, int i, int j, int depth,
                   int maxdepth, int *lens, const uint64_t *pre)
{
    int split, lo, hi, cap, c0;
    uint64_t total, half, target;
    (void)fw;
    if (j - i == 1) {
        lens[syms[i]] = depth;
        return;
    }
    cap = 1 << (maxdepth - depth - 1);   /* capacity of each child */
    lo = j - cap;                        /* split-point bounds */
    hi = i + cap;
    if (lo < i + 1) lo = i + 1;
    if (hi > j - 1) hi = j - 1;
    total = pre[j] - pre[i];
    half = total / 2;
    /* |pre[cand] - (pre[i]+half)| is V-shaped in cand (pre[] is strictly
     * increasing: only freq>0 symbols are listed), so the original's
     * linear scan finds its argmin at the lower_bound of target or one
     * before; the scan kept the FIRST strict minimum, i.e. a tie between
     * the two neighbours resolves to the earlier one. */
    target = pre[i] + half;
    {
        int a = lo, b = hi + 1;
        while (a < b) {
            int mid = (a + b) >> 1;
            if (pre[mid] >= target) b = mid;
            else a = mid + 1;
        }
        c0 = a;                          /* first cand with pre >= target */
    }
    if (c0 > hi)
        split = hi;                      /* never crossed: distance falls to hi */
    else if (c0 == lo)
        split = lo;                      /* crossed at the left edge */
    else {
        uint64_t l1 = pre[c0 - 1] - pre[i], l2 = pre[c0] - pre[i];
        uint64_t d1 = l1 > half ? l1 - half : half - l1;
        uint64_t d2 = l2 > half ? l2 - half : half - l2;
        split = (d1 <= d2) ? c0 - 1 : c0;
    }
    sf_rec(syms, fw, i, split, depth + 1, maxdepth, lens, pre);
    sf_rec(syms, fw, split, j, depth + 1, maxdepth, lens, pre);
}

/* freq[] indexed by symbol; writes lens[] (complete, depth <= maxdepth).
 * Returns number of assigned symbols. */
static int sf_lengths(const uint64_t *freq, int nsyms, int maxdepth,
                      int *lens, int *lens2 /* scratch none */)
{
    int syms[300], n = 0, i;
    uint64_t pre[301];
    (void)lens2;
    memset(lens, 0, (size_t)nsyms * sizeof(int));
    for (i = 0; i < nsyms; i++)
        if (freq[i] > 0)
            syms[n++] = i;
    if (n == 0)
        return 0;
    /* sort syms by freq desc (simple insertion; n <= 272) */
    for (i = 1; i < n; i++) {
        int s = syms[i];
        uint64_t f = freq[s];
        int k = i - 1;
        while (k >= 0 && freq[syms[k]] < f) { syms[k + 1] = syms[k]; k--; }
        syms[k + 1] = s;
    }
    pre[0] = 0;
    for (i = 0; i < n; i++)
        pre[i + 1] = pre[i] + freq[syms[i]];
    if (n == 1) { lens[syms[0]] = 1; return 1; }   /* single symbol -> len 1 */
    sf_rec(syms, freq, 0, n, 0, maxdepth, lens, pre);
    return n;
}

static int huff_lengths(const uint64_t *freq, int nsyms, int maxdepth, int *lens)
{
    uint64_t weight[2 * AIN_SYMS_A - 1];
    int parent[2 * AIN_SYMS_A - 1], symbol[AIN_SYMS_A];
    int leaves = 0, nodes, i;
    memset(lens, 0, (size_t)nsyms * sizeof *lens);
    for (i = 0; i < nsyms; i++) {
        if (freq[i]) {
            symbol[leaves] = i;
            weight[leaves] = freq[i];
            parent[leaves++] = -1;
        }
    }
    if (leaves < 2) {
        if (leaves)
            lens[symbol[0]] = 1;
        return leaves;
    }
    nodes = leaves;
    while (nodes < 2 * leaves - 1) {
        int a = -1, b = -1;
        for (i = 0; i < nodes; i++) {
            if (parent[i] >= 0)
                continue;
            if (a < 0 || weight[i] < weight[a]) {
                b = a;
                a = i;
            } else if (b < 0 || weight[i] < weight[b]) {
                b = i;
            }
        }
        weight[nodes] = weight[a] + weight[b];
        parent[nodes] = -1;
        parent[a] = parent[b] = nodes++;
    }
    for (i = 0; i < leaves; i++) {
        int depth = 0, p = i;
        while (parent[p] >= 0) {
            depth++;
            p = parent[p];
        }
        /* Keep the format's depth limit for very skewed distributions. */
        if (depth > maxdepth)
            return sf_lengths(freq, nsyms, maxdepth, lens, NULL);
        lens[symbol[i]] = depth;
    }
    return leaves;
}

static int encode_lengths(const uint64_t *freq, int nsyms, int *lens, int huffman)
{
    return huffman ? huff_lengths(freq, nsyms, 15, lens) :
                     sf_lengths(freq, nsyms, 15, lens, NULL);
}

/* ---- tree emission -------------------------------------------------------- */

/* RLE chunk chooser for the zero-run coding of the tree body:
 * sym0: single zero; sym1: run 3..18 (4 extra bits); sym2: run 20..531
 * (9 extra bits).  Run 19 is emitted as 18 + 1. */
static int run_chunk(int run)
{
    if (run >= 20) return run > 531 ? 531 : run;  /* sym2 */
    if (run == 19) return 18;                     /* sym1 */
    if (run >= 3) return run;                     /* sym1 */
    return 1;                                   /* sym0 */
}
static int run_sym(int chunk)
{
    if (chunk == 1) return 0;
    if (chunk <= 18) return 1;
    return 2;
}

/* scan (hist) or write the body; clc_code must arrive bit-reversed
 * (emit_tree does that once per tree instead of once per token) */
static void emit_tree_body(BitWriter *w, const int *lens, int limit,
                           const uint32_t *clc_code, const int *clc_len,
                           int hist_only, uint64_t *clc_freq)
{
    int i = 0;
    while (i < limit) {
        if (lens[i] == 0) {
            int run = 0, run_left;
            while (i + run < limit && lens[i + run] == 0)
                run++;
            run_left = run;
            while (run_left > 0) {
                int chunk = run_chunk(run_left), sym = run_sym(chunk);
                if (hist_only) {
                    clc_freq[sym]++;
                } else {
                    bw_write(w, clc_len[sym], clc_code[sym]);
                    if (sym == 1) bw_write(w, 4, (uint32_t)(chunk - 3));
                    else if (sym == 2) bw_write(w, 9, (uint32_t)(chunk - 20));
                }
                run_left -= chunk;
            }
            i += run;
        } else {
            int sym = lens[i] + 2;
            if (hist_only) {
                clc_freq[sym]++;
            } else {
                bw_write(w, clc_len[sym], clc_code[sym]);
            }
            i++;
        }
    }
}

/* write one tree (like load_tree reads it) */
static int emit_tree(BitWriter *w, const int *lens, int count, int huffman)
{
    int tail = 0, i, lastnz = -1;
    int clc_lens[19];
    uint32_t clc_code[19];
    uint64_t clc_freq[19];

    /* lastnz = index of highest used length (backward scan exits at once
     * for the usual dense tree; identical to a full forward scan) */
    for (i = count - 1; i >= 0; i--)
        if (lens[i] > 0)
            break;
    lastnz = i;
    tail = count - 1 - lastnz;           /* trailing zeros */
    if (tail < 0)
        return -1;                       /* empty tree cannot be encoded */

    memset(clc_freq, 0, sizeof clc_freq);
    emit_tree_body(w, lens, count - tail, NULL, NULL, 1, clc_freq);

    /* build the CLC tree (19 symbols); pad so a complete tree exists */
    {
        int used = 0;
        for (i = 0; i < 19; i++)
            if (clc_freq[i]) used++;
        if (used == 1) {
            for (i = 0; i < 19; i++)
                if (!clc_freq[i]) { clc_freq[i] = 1; break; }
        } else if (used == 0) {
            clc_freq[0] = 1;
            clc_freq[3] = 1;
        }
        if (encode_lengths(clc_freq, 19, clc_lens, huffman) <= 0)
            return -2;
        canon_codes(clc_lens, 19, clc_code);
        /* deliver the codes already bit-reversed: emit_tree_body writes
         * them directly (same values the per-token bitrev produced) */
        for (i = 0; i < 19; i++)
            clc_code[i] = bitrev(clc_code[i], clc_lens[i]);
    }

    /* CLC header (mirrors load_clc) */
    {
        int lastc = -1;
        for (i = 0; i < 19; i++)
            if (clc_lens[i]) lastc = i;
        bw_write(w, 5, (uint32_t)(18 - lastc));
        for (i = 0; i <= lastc; i++) {
            int v = clc_lens[i];
            if (v < 7) {
                bw_write(w, 3, (uint32_t)v);
            } else {
                int k;
                bw_write(w, 3, 7);
                for (k = 0; k < v - 7; k++)
                    bw_write(w, 1, 1);
                bw_write(w, 1, 0);
            }
        }
    }
    /* tree body (mirrors load_tree) */
    bw_write(w, 9, (uint32_t)tail);
    emit_tree_body(w, lens, count - tail, clc_code, clc_lens, 0, NULL);
    return w->err ? -3 : 0;
}

/* ================= LZ77 matcher ================= */

/* Literals: low byte. Matches: tag | class[26:23] | (len-3)[22:15] | dist[14:0]. */
typedef uint32_t Tok;
#define TOK_MATCH 0x80000000u
#define TOK_LEN_SHIFT 15
#define TOK_CLASS_SHIFT 23

static __inline int lz_hash(const uint8_t *p)
{
    uint32_t key = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    return (int)((key * 2654435761u) >> 19);
}

static void lz_insert_range(const uint8_t *in, size_t n, size_t p, size_t end,
                            int *head, int *prev)
{
    for (; end - p >= 8 && n - p >= 10; p += 8) {
        uint32_t hashes[8];
        int lane;
#ifdef AIN_NEON
        uint16x8_t a = vmovl_u8(vld1_u8(in + p));
        uint16x8_t b = vmovl_u8(vld1_u8(in + p + 1));
        uint16x8_t c = vmovl_u8(vld1_u8(in + p + 2));
        uint32x4_t low = vorrq_u32(vorrq_u32(
            vshlq_n_u32(vmovl_u16(vget_low_u16(a)), 16),
            vshlq_n_u32(vmovl_u16(vget_low_u16(b)), 8)),
            vmovl_u16(vget_low_u16(c)));
        uint32x4_t high = vorrq_u32(vorrq_u32(
            vshlq_n_u32(vmovl_u16(vget_high_u16(a)), 16),
            vshlq_n_u32(vmovl_u16(vget_high_u16(b)), 8)),
            vmovl_u16(vget_high_u16(c)));
        vst1q_u32(hashes, vshrq_n_u32(vmulq_n_u32(low, 2654435761u), 19));
        vst1q_u32(hashes + 4, vshrq_n_u32(vmulq_n_u32(high, 2654435761u), 19));
#else
        __m256i a = _mm256_cvtepu8_epi32(
            _mm_loadl_epi64((const __m128i *)(in + p)));
        __m256i b = _mm256_cvtepu8_epi32(
            _mm_loadl_epi64((const __m128i *)(in + p + 1)));
        __m256i c = _mm256_cvtepu8_epi32(
            _mm_loadl_epi64((const __m128i *)(in + p + 2)));
        __m256i keys = _mm256_or_si256(_mm256_or_si256(
            _mm256_slli_epi32(a, 16), _mm256_slli_epi32(b, 8)), c);
        __m256i hashes8 = _mm256_srli_epi32(_mm256_mullo_epi32(
            keys, _mm256_set1_epi32((int)2654435761u)), 19);
        _mm256_storeu_si256((__m256i *)hashes, hashes8);
#endif
        /* Equal hashes must still link in position order. */
        for (lane = 0; lane < 8; lane++) {
            size_t q = p + (size_t)lane;
            int h = (int)hashes[lane];
            prev[q & (AIN_WINDOW - 1)] = head[h];
            head[h] = (int)q;
            AIN_TR(q, h);
        }
    }
    for (; p < end && n - p >= 3; p++) {
        int h = lz_hash(in + p);
        prev[p & (AIN_WINDOW - 1)] = head[h];
        head[h] = (int)p;
        AIN_TR(p, h);
    }
}

static __inline int dist_class(int d)
{
    if (d <= 2)
        return d - 1;                    /* class 0 -> dist 1, class 1 -> dist 2 */
#if defined(_MSC_VER)
    {
        unsigned long idx;
        _BitScanReverse(&idx, (unsigned long)(d - 1));
        return (int)idx + 1;             /* bits(d-1) */
    }
#else
    return 32 - __builtin_clz((unsigned int)(d - 1));
#endif
}

/* v must be nonzero. */
static __inline int ctz64_nonzero(uint64_t v)
{
#if defined(_MSC_VER)
    unsigned long idx;
#ifdef _M_IX86
    if ((uint32_t)v) {
        _BitScanForward(&idx, (uint32_t)v);
        return (int)idx;
    }
    _BitScanForward(&idx, (uint32_t)(v >> 32));
    return (int)idx + 32;
#else
    _BitScanForward64(&idx, v);
    return (int)idx;
#endif
#else
    return __builtin_ctzll((unsigned long long)v);
#endif
}

/* Length of the common prefix of a and b, capped at maxl - exactly the
 * value the original byte loop produced, using bounded SIMD/word loads. */
static int match_len(const uint8_t *a, const uint8_t *b, int maxl)
{
    int l = 0;
    if (maxl >= 8) {                     /* reject hash strays before SIMD */
        uint64_t x, y;
        memcpy(&x, a, 8);
        memcpy(&y, b, 8);
        if (x != y)
            return ctz64_nonzero(x ^ y) >> 3;
        l = 8;
    }
#ifdef AIN_NEON
    for (; l + 16 <= maxl; l += 16) {
        uint64x2_t diff = vreinterpretq_u64_u8(veorq_u8(
            vld1q_u8(a + l), vld1q_u8(b + l)));
        uint64_t low = vgetq_lane_u64(diff, 0);
        uint64_t high = vgetq_lane_u64(diff, 1);
        if (low)
            return l + (ctz64_nonzero(low) >> 3);
        if (high)
            return l + 8 + (ctz64_nonzero(high) >> 3);
    }
#else
    for (; l + 32 <= maxl; l += 32) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(a + l));
        __m256i y = _mm256_loadu_si256((const __m256i *)(b + l));
        unsigned eq = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, y));
        if (eq != 0xFFFFFFFFu) {
#ifdef _MSC_VER
            unsigned long z;
            _BitScanForward(&z, ~eq);
            return l + (int)z;
#else
            return l + __builtin_ctz(~eq);
#endif
        }
    }
#endif
    while (l + 8 <= maxl) {
        uint64_t x, y;
        memcpy(&x, a + l, 8);
        memcpy(&y, b + l, 8);
        if (x != y)
            return l + (ctz64_nonzero(x ^ y) >> 3);
        l += 8;
    }
    while (l < maxl && a[l] == b[l])
        l++;
    return l;
}

static __inline int candidate_matches(const uint8_t *a, int bestlen,
                                      uint8_t refbyte, uint64_t refword)
{
    uint64_t word;
    if (a[bestlen] != refbyte)
        return 0;
    if (bestlen < 7)
        return 1;
    memcpy(&word, a + bestlen - 7, sizeof word);
    return word == refword;
}

/* parse `in`, emitting tokens (and frequencies) only for positions >=
 * emit_from; earlier positions are still indexed into the match chains.
 * toks has room for n-emit_from entries; prev has min(n, AIN_WINDOW).
 * Used by the multi-block encoder to carry window history across blocks. */
static int lz_parse_ex(const uint8_t *in, size_t n, size_t emit_from,
                       Tok *toks, int *prev,
                       uint64_t *freqA, uint64_t *freqB, int speed,
                       size_t *out_ntok)
{
    size_t ntok = 0;
    int head[8192];
    size_t i;
    int maxchain, lazychain;
    int hcarry = -1;                     /* HASH at current i, carried spec */
    int prev_lit = 1;                    /* previous token position was a
                                          * literal (only then can the i+1
                                          * speculation be consumed) */

    /* update-speed / search-effort levels (see -u1..-u3) */
    if (speed <= 1) {          /* 1: slow, higher compression */
        maxchain = 4096;
        lazychain = 256;
    } else if (speed == 2) {   /* 2: normal */
        maxchain = 128;
        lazychain = 16;
    } else {                   /* 3: fast, lower compression */
        maxchain = 12;
        lazychain = 0;         /* no lazy evaluation */
    }
    memset(head, 0xFF, sizeof head);     /* -1 */

    lz_insert_range(in, n, 0, emit_from, head, prev);
    for (i = emit_from; i < n; i++) {
        int bestlen = 0, bestdist = 0;
        int hnext = -1, hval = -1;

        /* speculation for position i+1: its hash (carried: the scan /
         * insert / literal path at i+1 reuses it, it is never wasted when
         * i emits a literal) and a prefetch of the first candidate byte,
         * issued so the miss overlaps the scan at i.  Only useful while
         * literals chain: when i emits a match, position i+1 is consumed
         * by the skip and the speculation is thrown away, so it is gated
         * on the previous position being a literal (prev_lit). */
        if (prev_lit && i + 3 < n) {
            hnext = lz_hash(in + i + 1);
            {
                int cn = head[hnext];
                if (cn >= 0) {
#if defined(AIN_NEON) && !defined(_MSC_VER)
                    __builtin_prefetch(in + cn, 0, 3);
#elif defined(AIN_NEON)
                    __prefetch(in + cn);
#else
                    _mm_prefetch((const char *)(in + cn), _MM_HINT_T0);
#endif
                }
            }
        }

        if (i + 2 < n) {
            int h = hcarry >= 0 ? hcarry : lz_hash(in + i);
            int cand = head[h];
            int checks = 0;
            int maxl = (int)(n - i);     /* depends only on i: hoisted */
            uint64_t refword = 0;
            uint8_t refb;                /* == in[i + bestlen], kept current:
                                          * MSVC reloads in[i+bestlen] every
                                          * hop because prev[] stores may
                                          * alias uint8_t reads */
            hval = h;                    /* same hash reused on insert/literal */
            if (maxl > AIN_MAXMATCH) maxl = AIN_MAXMATCH;
            refb = in[i];
            while (cand >= 0 && i - (size_t)cand <= AIN_MAXDIST && checks++ < maxchain) {
                int l = 0;
                /* A winner must match through bestlen, including the word
                 * ending there when bestlen >= 7. */
                if (candidate_matches(in + cand, bestlen, refb, refword)) {
                    l = match_len(in + cand, in + i, maxl);
                    if (l > bestlen && l >= AIN_MINMATCH) {
                        int d = (int)i - cand;
                        if (d <= AIN_MAXDIST) {
                            bestlen = l;
                            bestdist = d;
                            if (l < maxl) {
                                refb = in[i + l];
                                if (l >= 7)
                                    memcpy(&refword, in + i + l - 7, sizeof refword);
                            }
                        }
                    }
                    if (l >= maxl) break;
                }
                /* effort-1 keeps long chains bounded, but never cuts the
                 * scan shorter than the level-2 cutoff (so -u1 can only
                 * find more than -u2, never less) */
                if (maxchain > 128 && checks > 128 &&
                    (bestlen >= 128 || (bestlen >= 64 && checks > 512)))
                    break;
                cand = prev[cand & (AIN_WINDOW - 1)];
            }
        }

        /* lazy evaluation: is the match at i+1 strictly better? */
        if (lazychain > 0 && bestlen > 0 && bestlen < AIN_MAXMATCH && i + 3 < n) {
            int h2 = hnext >= 0 ? hnext       /* speculation ran; otherwise
                                          * compute here (lazy runs exactly
                                          * after matches, where the spec
                                          * is gated off - hnext is -1!) */
                                : lz_hash(in + i + 1);
            int cand2 = head[h2];
            int checks2 = 0, best2 = 0;
            int maxl = (int)(n - i - 1);  /* depends only on i: hoisted */
            uint64_t refword2 = 0;
            uint8_t refb2;                /* == in[i + 1 + best2] */
            if (maxl > AIN_MAXMATCH) maxl = AIN_MAXMATCH;
            refb2 = in[i + 1];
            while (cand2 >= 0 && i + 1 - (size_t)cand2 <= AIN_MAXDIST && checks2++ < lazychain) {
                int l = 0;
                if (candidate_matches(in + cand2, best2, refb2, refword2)) {
                    l = match_len(in + cand2, in + i + 1, maxl);
                    if (l > best2) {
                        best2 = l;
                        if (l < maxl) {
                            refb2 = in[i + 1 + l];
                            if (l >= 7)
                                memcpy(&refword2, in + i + 1 + l - 7, sizeof refword2);
                        }
                    }
                    if (l >= maxl) break;
                }
                if (lazychain > 16 && checks2 > 16 &&
                    (best2 >= 128 || (best2 >= 64 && checks2 > 64)))
                    break;
                cand2 = prev[cand2 & (AIN_WINDOW - 1)];
            }
            if (best2 > bestlen + 1)
                bestlen = 0;             /* emit literal instead */
        }

        if (bestlen >= AIN_MINMATCH) {
            int k = dist_class(bestdist);
            toks[ntok++] = TOK_MATCH | ((uint32_t)k << TOK_CLASS_SHIFT) |
                ((uint32_t)(bestlen - AIN_MINMATCH) << TOK_LEN_SHIFT) |
                (uint32_t)bestdist;
            freqA[256 + k]++;
            freqB[bestlen - AIN_MINMATCH]++;
            {   /* index all covered positions; j == 0 is peeled out (its
                 * hash equals hval whenever valid - same i + 2 < n guard),
                 * keeping the hot per-byte loop free of that test */
                int j = 0;
                if (hval >= 0) {
                    prev[i & (AIN_WINDOW - 1)] = head[hval];
                    head[hval] = (int)i;
                    AIN_TR(i, hval);
                    j = 1;
                }
                lz_insert_range(in, n, i + (size_t)j, i + (size_t)bestlen,
                                head, prev);
            }
            i += (size_t)bestlen - 1;
            hcarry = -1;                 /* skip invalidates the i+1 spec */
            prev_lit = 0;
        } else {
            toks[ntok++] = in[i];
            freqA[in[i]]++;
            if (i + 2 < n) {
                int h = hval;            /* the scan computed the same hash */
                prev[i & (AIN_WINDOW - 1)] = head[h];
                head[h] = (int)i;
                AIN_TR(i, h);
            }
            hcarry = hnext;              /* i advances to i+1: carry it */
            prev_lit = 1;
        }
    }
    *out_ntok = ntok;
    return 0;
}

static int lz_parse(const uint8_t *in, size_t n, Tok **out_toks, size_t *out_ntok,
                    uint64_t *freqA, uint64_t *freqB, int speed)
{
    Tok *toks;
    int *prev;
    size_t ntok = 0;
    if (n > INT_MAX || n > SIZE_MAX / sizeof(Tok))
        return -1;
    toks = (Tok *)malloc((n ? n : 1) * sizeof(Tok));
    prev = (int *)malloc((n < AIN_WINDOW ? (n ? n : 1) : AIN_WINDOW) * sizeof(int));
    if (!toks || !prev) { free(toks); free(prev); return -1; }
    if (lz_parse_ex(in, n, 0, toks, prev, freqA, freqB, speed, &ntok)) {
        free(toks); free(prev);
        return -1;
    }
    free(prev);
    *out_toks = toks;
    *out_ntok = ntok;
    return 0;
}

/* ================= stream encoder ================= */

/* Emit one complete block: tree A, tree B, symbols, then the block
 * terminator (class-15 symbol + extra 0x3FFF + flag: 1 = end of stream,
 * 0 = reload both trees and continue).  freq arrays are modified in place
 * (degenerate-case padding). */
static int emit_block_body(BitWriter *w, const Tok *toks, size_t ntok,
                           uint64_t *freqA, uint64_t *freqB, int final,
                           int huffman)
{
    int lensA[AIN_SYMS_A] = {0}, lensB[AIN_SYMS_B] = {0};
    uint32_t codeA[AIN_SYMS_A], codeB[AIN_SYMS_B];
    uint32_t emitA[AIN_SYMS_A], emitB[AIN_SYMS_B];   /* (len << 16) | reversed code */
    size_t i;
    int usedA, usedB, k;

    freqA[271]++;                        /* terminator (dist class 15) */

    /* build trees; pad degenerate cases so the tree is complete */
    usedA = 0;
    for (i = 0; i < AIN_SYMS_A; i++) if (freqA[i]) usedA++;
    if (usedA == 1) {
        for (i = 0; i < AIN_SYMS_A; i++)
            if (!freqA[i]) { freqA[i] = 1; break; }
    }
    if (encode_lengths(freqA, AIN_SYMS_A, lensA, huffman) <= 0)
        return -2;
    canon_codes(lensA, AIN_SYMS_A, codeA);

    usedB = 0;
    for (i = 0; i < AIN_SYMS_B; i++) if (freqB[i]) usedB++;
    if (usedB == 0) {
        lensB[0] = 1; lensB[1] = 1;      /* never decoded; complete filler tree */
        memset(codeB, 0, sizeof codeB);
    } else {
        if (usedB == 1) {
            for (i = 0; i < AIN_SYMS_B; i++)
                if (!freqB[i]) { freqB[i] = 1; break; }
        }
        if (encode_lengths(freqB, AIN_SYMS_B, lensB, huffman) <= 0)
            return -3;
        canon_codes(lensB, AIN_SYMS_B, codeB);
    }

    if (emit_tree(w, lensA, AIN_SYMS_A, huffman) ||
        emit_tree(w, lensB, AIN_SYMS_B, huffman))
        return -4;
    /* per-block code table: bit reversal is a property of the symbol's
     * code, not of the token stream, so pay it ~530x per block instead of
     * once per token (lengths are <= 15, reversed codes fit the low half) */
    for (i = 0; i < AIN_SYMS_A; i++)
        emitA[i] = ((uint32_t)lensA[i] << 16) | bitrev(codeA[i], lensA[i]);
    for (i = 0; i < AIN_SYMS_B; i++)
        emitB[i] = ((uint32_t)lensB[i] << 16) | bitrev(codeB[i], lensB[i]);
    for (i = 0; i < ntok; i++) {
        uint32_t e;
        Tok tok = toks[i];
        if (!(tok & TOK_MATCH)) {
            e = emitA[tok];
            bw_write(w, (int)(e >> 16), e & 0xFFFFu);
        } else {
            int len_sym = (int)((tok >> TOK_LEN_SHIFT) & 0xFFu);
            int d = (int)(tok & 0x7FFFu);
            k = (int)((tok >> TOK_CLASS_SHIFT) & 0xFu);
            e = emitA[256 + k];
            bw_write(w, (int)(e >> 16), e & 0xFFFFu);
            if (k >= 2)
                bw_write(w, k - 1, (uint32_t)(d - 1 - (1 << (k - 1))));
            e = emitB[len_sym];
            bw_write(w, (int)(e >> 16), e & 0xFFFFu);
        }
    }
    /* terminator: class-15 symbol + 0x3FFF + flag */
    {
        uint32_t e = emitA[271];
        bw_write(w, (int)(e >> 16), e & 0xFFFFu);
    }
    bw_write(w, 14, AIN_EOF_EXTRA);
    bw_write(w, 1, final ? 1 : 0);
    return 0;
}

typedef struct { int len, dist, saved; } CostMatch;

static CostMatch lz_cost_match(const uint8_t *in, size_t n, size_t pos,
                               const int *head, const int *prev,
                               const int *costA, const int *costB, int maxchain)
{
    CostMatch best = {0, 0, 0};
    int seen[16] = {0}, prefix[AIN_MAXMATCH + 1], filled = 0;
    int cand, checks = 0;
    int maxl = n - pos < AIN_MAXMATCH ? (int)(n - pos) : AIN_MAXMATCH;
    if (maxl < AIN_MINMATCH)
        return best;
    prefix[0] = 0;
    cand = head[lz_hash(in + pos)];
    while (cand >= 0 && pos - (size_t)cand <= AIN_MAXDIST && checks++ < maxchain) {
        int dist = (int)pos - cand, k = dist_class(dist);
        int len, l;
        if (seen[k] == maxl || in[cand + seen[k]] != in[pos + (size_t)seen[k]]) {
            cand = prev[cand & (AIN_WINDOW - 1)];
            continue;
        }
        len = match_len(in + cand, in + pos, maxl);
        if (len >= AIN_MINMATCH && len > seen[k]) {
            for (; filled < len; filled++)
                prefix[filled + 1] = prefix[filled] + costA[in[pos + (size_t)filled]];
            l = seen[k] + 1;
            if (l < AIN_MINMATCH)
                l = AIN_MINMATCH;
            for (; l <= len; l++) {
                int bits = costA[256 + k] + (k >= 2 ? k - 1 : 0) +
                           costB[l - AIN_MINMATCH];
                int saved = prefix[l] - bits;
                if (saved > best.saved || (saved == best.saved && best.saved &&
                                           l > best.len)) {
                    best.len = l;
                    best.dist = dist;
                    best.saved = saved;
                }
            }
            seen[k] = len;
            if (len == maxl)
                break;
        }
        cand = prev[cand & (AIN_WINDOW - 1)];
    }
    return best;
}

static void lz_estimate_costs(const uint64_t *freqA, const uint64_t *freqB,
                              int *costA, int *costB)
{
    uint64_t estimateA[AIN_SYMS_A];
    int i;
    memcpy(estimateA, freqA, sizeof estimateA);
    estimateA[271]++;
    huff_lengths(estimateA, AIN_SYMS_A, 15, costA);
    huff_lengths(freqB, AIN_SYMS_B, 15, costB);
    for (i = 0; i < AIN_SYMS_A; i++)
        if (!costA[i]) costA[i] = 15;
    for (i = 0; i < AIN_SYMS_B; i++)
        if (!costB[i]) costB[i] = 15;
}

static int lz_reparse(const uint8_t *in, size_t n, size_t emit_from,
                      Tok *toks, size_t *ntok, uint64_t *freqA, uint64_t *freqB)
{
    int costA[AIN_SYMS_A], costB[AIN_SYMS_B], head[8192];
    int *prev = (int *)malloc(AIN_WINDOW * sizeof *prev);
    size_t pos, count = 0;
    if (!prev)
        return -1;
    lz_estimate_costs(freqA, freqB, costA, costB);
    memset(freqA, 0, AIN_SYMS_A * sizeof *freqA);
    memset(freqB, 0, AIN_SYMS_B * sizeof *freqB);
    memset(head, 0xFF, sizeof head);
    lz_insert_range(in, n, 0, emit_from, head, prev);
    for (pos = emit_from; pos < n;) {
        CostMatch match = lz_cost_match(in, n, pos, head, prev, costA, costB, 128);
        size_t take = 1;
        if (match.len && match.len < AIN_MAXMATCH && n - pos > 3) {
            CostMatch next = lz_cost_match(in, n, pos + 1, head, prev,
                                          costA, costB, 16);
            if (next.saved > match.saved)
                match.len = 0;
        }
        if (match.len) {
            int k = dist_class(match.dist), sym = match.len - AIN_MINMATCH;
            toks[count++] = TOK_MATCH | ((uint32_t)k << TOK_CLASS_SHIFT) |
                ((uint32_t)sym << TOK_LEN_SHIFT) | (uint32_t)match.dist;
            freqA[256 + k]++;
            freqB[sym]++;
            take = (size_t)match.len;
        } else {
            toks[count++] = in[pos];
            freqA[in[pos]]++;
        }
        lz_insert_range(in, n, pos, pos + take, head, prev);
        pos += take;
    }
    free(prev);
    *ntok = count;
    return 0;
}

/* Shortest path under fixed estimated code lengths. Each position is
 * indexed regardless of the chosen path, so match availability is stable. */
static int lz_optimal_reparse(const uint8_t *in, size_t n, size_t emit_from,
                              Tok *toks, size_t *ntok,
                              uint64_t *freqA, uint64_t *freqB)
{
    size_t size = n - emit_from, pos, count = 0;
    uint64_t *cost;
    Tok *choice;
    int *prev;
    int costA[AIN_SYMS_A], costB[AIN_SYMS_B], head[8192];
    int base_cost[16], suffix_cost[16], k;
    if (size == SIZE_MAX || size + 1 > SIZE_MAX / sizeof *cost ||
        size + 1 > SIZE_MAX / sizeof *choice)
        return -1;
    cost = (uint64_t *)malloc((size + 1) * sizeof *cost);
    choice = (Tok *)malloc((size + 1) * sizeof *choice);
    prev = (int *)malloc(AIN_WINDOW * sizeof *prev);
    if (!cost || !choice || !prev) {
        free(cost); free(choice); free(prev);
        return -1;
    }
    lz_estimate_costs(freqA, freqB, costA, costB);
    for (k = 15; k >= 0; k--) {
        base_cost[k] = costA[256 + k] + (k >= 2 ? k - 1 : 0);
        suffix_cost[k] = base_cost[k];
        if (k < 15 && suffix_cost[k + 1] < suffix_cost[k])
            suffix_cost[k] = suffix_cost[k + 1];
    }
    memset(cost, 0xFF, (size + 1) * sizeof *cost);
    cost[0] = 0;
    memset(head, 0xFF, sizeof head);
    lz_insert_range(in, n, 0, emit_from, head, prev);
    for (pos = emit_from; pos < n; pos++) {
        size_t at = pos - emit_from;
        uint64_t literal = cost[at] + costA[in[pos]];
        int maxl = n - pos < AIN_MAXMATCH ? (int)(n - pos) : AIN_MAXMATCH;
        if (literal < cost[at + 1]) {
            cost[at + 1] = literal;
            choice[at + 1] = in[pos];
        }
        if (maxl >= AIN_MINMATCH) {
            int seen[16] = {0}, checks = 0;
            int prices[AIN_MAXMATCH + 1], longest = 0, full_cost = INT_MAX;
            Tok edges[AIN_MAXMATCH + 1];
            int cand = head[lz_hash(in + pos)];
            int l;
            for (l = AIN_MINMATCH; l <= maxl; l++) prices[l] = INT_MAX;
            while (cand >= 0 && pos - (size_t)cand <= AIN_MAXDIST &&
                   checks++ < 128) {
                int dist = (int)pos - cand, k = dist_class(dist);
                if (full_cost <= suffix_cost[k])
                    break;              /* older distance classes cannot improve any edge */
                if (base_cost[k] < full_cost && seen[k] < maxl &&
                    in[cand + seen[k]] == in[pos + (size_t)seen[k]]) {
                    int len = match_len(in + cand, in + pos, maxl);
                    int l = seen[k] + 1;
                    Tok edge = TOK_MATCH | ((uint32_t)k << TOK_CLASS_SHIFT) |
                        (uint32_t)dist;
                    if (l < AIN_MINMATCH) l = AIN_MINMATCH;
                    for (; l <= len; l++) {
                        if (base_cost[k] < prices[l]) {
                            prices[l] = base_cost[k];
                            edges[l] = edge;
                        }
                    }
                    if (len > seen[k]) seen[k] = len;
                    if (len > longest) longest = len;
                    if (len == maxl) full_cost = base_cost[k];
                }
                cand = prev[cand & (AIN_WINDOW - 1)];
            }
            for (l = AIN_MINMATCH; l <= longest; l++) {
                uint64_t bits = cost[at] + prices[l] + costB[l - AIN_MINMATCH];
                if (bits < cost[at + (size_t)l]) {
                    cost[at + (size_t)l] = bits;
                    choice[at + (size_t)l] = edges[l] |
                        ((uint32_t)(l - AIN_MINMATCH) << TOK_LEN_SHIFT);
                }
            }
        }
        lz_insert_range(in, n, pos, pos + 1, head, prev);
    }
    memset(freqA, 0, AIN_SYMS_A * sizeof *freqA);
    memset(freqB, 0, AIN_SYMS_B * sizeof *freqB);
    for (pos = size; pos;) {
        Tok tok = choice[pos];
        toks[count++] = tok;
        if (tok & TOK_MATCH) {
            int sym = (tok >> TOK_LEN_SHIFT) & 255;
            freqA[256 + ((tok >> TOK_CLASS_SHIFT) & 15)]++;
            freqB[sym]++;
            pos -= (size_t)sym + AIN_MINMATCH;
        } else {
            freqA[tok]++;
            pos--;
        }
    }
    for (pos = 0; pos < count / 2; pos++) {
        Tok tmp = toks[pos];
        toks[pos] = toks[count - 1 - pos];
        toks[count - 1 - pos] = tmp;
    }
    *ntok = count;
    free(cost); free(choice); free(prev);
    return 0;
}

static int emit_block(BitWriter *w, const uint8_t *in, size_t n,
                      size_t emit_from, Tok *toks, size_t ntok,
                      uint64_t *freqA, uint64_t *freqB, int final, int speed)
{
    AinBuf best, trial;
    BitWriter best_writer = *w;
    uint64_t best_bits = UINT64_MAX;
    int pass, rc = 0;
    if (w->err)
        return -5;
    if (speed == 3)
        return emit_block_body(w, toks, ntok, freqA, freqB, final, 0);
    ain_buf_init(&best);
    ain_buf_init(&trial);
    for (pass = 0; pass < (speed == 1 ? 6 : 4); pass++) {
        uint64_t fa[AIN_SYMS_A], fb[AIN_SYMS_B], bits;
        BitWriter candidate = *w;
        if (pass == 2) {
            uint64_t matches = 0, short_matches = 0;
            int k;
            for (k = 256; k < AIN_SYMS_A; k++)
                matches += freqA[k];
            for (k = 0; k <= 32 - AIN_MINMATCH; k++)
                short_matches += freqB[k];
            /* Avoid a second match search for near-literal streams and
             * streams dominated by long matches with little choice. */
            if (ntok < (n - emit_from) / 32 || matches <= ntok / 64 ||
                short_matches * 4 < matches)
                break;
            if (lz_reparse(in, n, emit_from, toks, &ntok, freqA, freqB)) {
                rc = -1;
                break;
            }
        }
        if (pass == 4) {
            if (lz_optimal_reparse(in, n, emit_from, toks, &ntok, freqA, freqB)) {
                rc = -1;
                break;
            }
        }
        trial.len = 0;
        candidate.out = &trial;
        memcpy(fa, freqA, sizeof fa);
        memcpy(fb, freqB, sizeof fb);
        rc = emit_block_body(&candidate, toks, ntok, fa, fb, final, pass & 1);
        if (rc || candidate.err) {
            if (!rc)
                rc = -5;
            break;
        }
        /* Include headers, terminator, and pending bits, not just payload. */
        bits = (uint64_t)trial.len * 8 + (unsigned)candidate.cnt;
        if (bits < best_bits) {
            AinBuf swap = best;
            best = trial;
            trial = swap;
            best_writer = candidate;
            best_bits = bits;
        }
    }
    if (!rc) {
        if (ain_buf_put(w->out, best.p, best.len)) {
            w->err = 1;
            rc = -5;
        } else {
            w->buf = best_writer.buf;
            w->cnt = best_writer.cnt;
        }
    }
    ain_buf_free(&best);
    ain_buf_free(&trial);
    return rc;
}

int ain_encode_stream(const uint8_t *in, size_t inlen, AinBuf *out, int speed)
{
    Tok *toks = NULL;
    size_t ntok = 0;
    uint64_t freqA[AIN_SYMS_A], freqB[AIN_SYMS_B];
    BitWriter w;
    int rc;

    memset(freqA, 0, sizeof freqA);
    memset(freqB, 0, sizeof freqB);
    if (speed < 1 || speed > 3)
        speed = 2;
    if (lz_parse(in, inlen, &toks, &ntok, freqA, freqB, speed))
        return -1;

    bw_init(&w, out);
    bw_write(&w, 1, 0);                  /* init bit */
    rc = emit_block(&w, in, inlen, 0, toks, ntok, freqA, freqB, 1, speed);
    bw_flush(&w);
    free(toks);
    if (rc < 0)
        return rc;
    return w.err ? -5 : 0;
}

/* ============== streaming multi-block encoder ==============
 * Feed bytes incrementally; input is split into blocks of up to `chunk`
 * raw bytes, each with its own Huffman trees.  Blocks are separated by the
 * format's own tree-reload marker (class-15 symbol + 0x3FFF + flag bit 0),
 * which the decoder (original included) handles by re-reading both trees;
 * window history carries across blocks through the retained last
 * AIN_WINDOW bytes. Smaller blocks adapt Huffman codes to local symbol
 * frequencies more closely, at the cost of more tree headers.
 * Output is appended to the AinBuf passed at open; the BitWriter only
 * pushes whole bytes, so the caller may drain that buffer between
 * ain_enc_feed() calls (bits in flight stay inside the writer). */

typedef struct {
    AinWorker *worker;
    uint8_t *input;
    Tok *toks;
    int *prev;
    size_t n, history;
    int speed, final, rc;
    AinBuf output;
    BitWriter writer;
#ifdef AIN_TEST_MT
    size_t sequence;
#endif
} EncJob;

typedef struct {
    size_t bytes;
    uint64_t bits;
    int count;
} EncCheckpoint;

#ifdef AIN_TEST_MT
extern int ain_test_pool_failure(unsigned started);
extern int ain_test_worker_begin(size_t sequence);
extern void ain_test_worker_end(size_t sequence);
extern int ain_test_output_failure(void);
#endif

struct AinEncS {
    AinBuf     *out;
    BitWriter   w;
    int         speed;
    uint8_t    *buf;        /* [history <= 32K | pending block bytes]   */
    size_t      cap;        /* AIN_WINDOW + chunk                       */
    size_t      H;          /* valid history bytes at start of buf      */
    size_t      len;        /* pending bytes after history              */
    Tok        *toks;       /* parse scratch, chunk entries             */
    int        *prev;       /* chain scratch, AIN_WINDOW entries        */
    int         blocks;     /* blocks flushed so far                    */
    unsigned    threads, started;
    EncJob     *jobs;
    size_t      submitted, collected;
    int         failed, finished;
    EncCheckpoint *checkpoints;
    size_t checkpoint_count, checkpoint_cap;
    int prefix_mode;
    AinBuf rebase_cache;
};

static int enc_checkpoint(AinEncS *e)
{
    EncCheckpoint *point;
    if (!e->prefix_mode)
        return 0;
    if (e->checkpoint_count == e->checkpoint_cap) {
        size_t cap = e->checkpoint_cap ? e->checkpoint_cap * 2 : 64;
        EncCheckpoint *grown = (EncCheckpoint *)realloc(e->checkpoints,
                                                       cap * sizeof *grown);
        if (!grown)
            return -1;
        e->checkpoints = grown;
        e->checkpoint_cap = cap;
    }
    point = &e->checkpoints[e->checkpoint_count++];
    point->bytes = e->out->len;
    point->bits = e->w.buf;
    point->count = e->w.cnt;
    return 0;
}

AinEncS *ain_enc_open(AinBuf *out, int speed, size_t chunk)
{
    return ain_enc_open_mt(out, speed, chunk, 1);
}

AinEncS *ain_enc_open_mt(AinBuf *out, int speed, size_t chunk, unsigned threads)
{
    AinEncS *e = (AinEncS *)calloc(1, sizeof *e);
    if (!e)
        return NULL;
    if (speed < 1 || speed > 3)
        speed = 2;
    if (chunk < 65536)
        chunk = 65536;                   /* keep reload overhead sane */
    if (chunk > INT_MAX - AIN_WINDOW || chunk > SIZE_MAX / sizeof(Tok)) {
        free(e);
        return NULL;
    }
    if (!threads) {
        size_t budget;
        threads = ain_thread_cpu_count();
        if (!threads) {
            free(e);
            return NULL;
        }
        if (threads > 256) threads = 256;
        /* Speed 1 also keeps shortest-path costs and predecessor tokens. */
        budget = ((size_t)256 << 20) /
            ((speed == 1 ? 32 : 16) * (chunk + AIN_WINDOW));
        if (threads > budget) threads = (unsigned)budget;
        if (!threads) threads = 1;
    }
    if (threads > 256) {
        free(e);
        return NULL;
    }
    e->threads = threads;
    e->cap = AIN_WINDOW + chunk;
    e->buf  = (uint8_t *)malloc(e->cap);
    e->toks = (Tok *)malloc(chunk * sizeof(Tok));
    e->prev = (int *)malloc(AIN_WINDOW * sizeof(int));
    if (!e->buf || !e->toks || !e->prev) {
        ain_enc_free(e);
        return NULL;
    }
    e->out = out;
    e->speed = speed;
    bw_init(&e->w, out);
    return e;
}

void ain_enc_free(AinEncS *e)
{
    unsigned i;
    if (!e)
        return;
    if (e->jobs) {
        for (i = 0; i < e->threads; i++) {
            EncJob *job = &e->jobs[i];
            ain_worker_free(job->worker);
            free(job->input);
            free(job->toks);
            free(job->prev);
            ain_buf_free(&job->output);
        }
        free(e->jobs);
    }
    free(e->buf);
    free(e->toks);
    free(e->prev);
    free(e->checkpoints);
    ain_buf_free(&e->rebase_cache);
    free(e);
}

static void enc_worker(void *arg)
{
    EncJob *job = (EncJob *)arg;
    uint64_t freqA[AIN_SYMS_A] = {0}, freqB[AIN_SYMS_B] = {0};
    size_t ntok = 0;
    job->output.len = 0;
    bw_init(&job->writer, &job->output);
    job->rc = 0;
#ifdef AIN_TEST_MT
    job->rc = ain_test_worker_begin(job->sequence);
#endif
    if (!job->rc && job->n > job->history)
        job->rc = lz_parse_ex(job->input, job->n, job->history,
                              job->toks, job->prev, freqA, freqB,
                              job->speed, &ntok);
    if (!job->rc)
        job->rc = emit_block(&job->writer, job->input, job->n, job->history,
                             job->toks, ntok, freqA, freqB, job->final, job->speed);
#ifdef AIN_TEST_MT
    ain_test_worker_end(job->sequence);
#endif
}

static int enc_start_workers(AinEncS *e)
{
    unsigned i;
#ifdef AIN_TEST_MT
    if (ain_test_pool_failure(0))
        return -1;
#endif
    e->jobs = (EncJob *)calloc(e->threads, sizeof *e->jobs);
    if (!e->jobs)
        return -1;
    for (i = 0; i < e->threads; i++) {
        EncJob *job = &e->jobs[i];
#ifdef AIN_TEST_MT
        if (ain_test_pool_failure(e->started))
            return -1;
#endif
        job->input = (uint8_t *)malloc(e->cap);
        job->toks = (Tok *)malloc((e->cap - AIN_WINDOW) * sizeof(Tok));
        job->prev = (int *)malloc(AIN_WINDOW * sizeof(int));
        if (!job->input || !job->toks || !job->prev)
            return -1;
        job->worker = ain_worker_create(enc_worker, job);
        if (!job->worker)
            return -1;
        e->started++;
    }
    /* The coordinator no longer needs serial parsing scratch. */
    if (!e->prefix_mode) {
        free(e->toks);
        free(e->prev);
        e->toks = NULL;
        e->prev = NULL;
    }
    return 0;
}

static int enc_collect(AinEncS *e, int wait)
{
    while (e->collected < e->submitted) {
        EncJob *job = &e->jobs[e->collected % e->threads];
        int result = ain_worker_wait(job->worker, wait);
        size_t pos = 0;
        if (!result)
            return 0;
        if (result < 0 || job->rc || job->writer.err)
            return -1;
#ifdef AIN_TEST_MT
        if (ain_test_output_failure()) {
            e->w.err = 1;
            return -1;
        }
#endif
        /* Append without padding: block boundaries need not be byte aligned. */
        while (job->output.len - pos >= 4) {
            uint32_t v;
            memcpy(&v, job->output.p + pos, sizeof v);
            bw_write(&e->w, 32, v);
            pos += 4;
        }
        while (pos < job->output.len)
            bw_write(&e->w, 8, job->output.p[pos++]);
        bw_write(&e->w, job->writer.cnt, (uint32_t)job->writer.buf);
        if (e->w.err)
            return -1;
        if (enc_checkpoint(e))
            return -1;
        e->collected++;
        /* Only block for the oldest result; then collect ready successors. */
        wait = 0;
    }
    return 0;
}

static int enc_flush_block(AinEncS *e, int final)
{
    size_t n = e->H + e->len;
    size_t ntok = 0, keep;
    uint64_t freqA[AIN_SYMS_A], freqB[AIN_SYMS_B];

    if (e->threads > 1 && (!final || e->jobs)) {
        EncJob *job;
        if (!e->jobs && enc_start_workers(e))
            return -1;
        if (e->submitted - e->collected == e->threads && enc_collect(e, 1))
            return -1;
        job = &e->jobs[e->submitted % e->threads];
        memcpy(job->input, e->buf, n);
        job->n = n;
        job->history = e->H;
        job->speed = e->speed;
        job->final = final;
#ifdef AIN_TEST_MT
        job->sequence = e->submitted;
#endif
        if (ain_worker_submit(job->worker))
            return -1;
        e->submitted++;
    } else {
        memset(freqA, 0, sizeof freqA);
        memset(freqB, 0, sizeof freqB);
        if (e->len &&
            lz_parse_ex(e->buf, n, e->H, e->toks, e->prev, freqA, freqB,
                        e->speed, &ntok))
            return -1;
    }
    if (!e->blocks)
        bw_write(&e->w, 1, 0);           /* stream init bit, once only */
    if (!e->jobs && emit_block(&e->w, e->buf, n, e->H, e->toks, ntok,
                               freqA, freqB, final, e->speed))
        return -2;
    if (!e->jobs && enc_checkpoint(e))
        return -2;
    if (e->jobs && enc_collect(e, 0))
        return -2;
    e->blocks++;
    /* keep the tail as window history for the next block */
    keep = n < AIN_WINDOW ? n : AIN_WINDOW;
    if (keep)
        memmove(e->buf, e->buf + n - keep, keep);
    e->H = keep;
    e->len = 0;
    return 0;
}

int ain_enc_feed(AinEncS *e, const uint8_t *p, size_t n)
{
    size_t chunkmax = e->cap - AIN_WINDOW;
    if (e->failed || e->finished)
        return -1;
    while (n) {
        size_t take = chunkmax - e->len;
        if (take > n)
            take = n;
        memcpy(e->buf + e->H + e->len, p, take);
        e->len += take;
        p += take;
        n -= take;
        if (e->len == chunkmax)
            if (enc_flush_block(e, 0)) {
                e->failed = 1;
                return -1;
            }
    }
    if (e->w.err || (e->jobs && enc_collect(e, 0))) {
        e->failed = 1;
        return -2;
    }
    return 0;
}

int ain_enc_finish(AinEncS *e)
{
    /* last block; if pending is empty but a reload marker closed the
     * previous block, this emits the closing trees + end flag */
    if (e->failed || e->finished)
        return -1;
    e->finished = 1;
    if (enc_flush_block(e, 1)) {
        e->failed = 1;
        return -1;
    }
    while (e->collected < e->submitted) {
        if (enc_collect(e, 1)) {
            e->failed = 1;
            return -1;
        }
    }
    bw_flush(&e->w);
    if (e->w.err)
        e->failed = 1;
    return e->w.err ? -2 : 0;
}

int ain_enc_prefix(AinEncS *e, const uint8_t *input, size_t size, AinBuf *out)
{
    size_t chunk = e->cap - AIN_WINDOW;
    size_t blocks = size / chunk, offset = blocks * chunk;
    size_t cached = (size_t)e->blocks * chunk;
    size_t history = offset < AIN_WINDOW ? offset : AIN_WINDOW;
    size_t ntok = 0;
    uint64_t freqA[AIN_SYMS_A] = { 0 }, freqB[AIN_SYMS_B] = { 0 };
    BitWriter writer;
    if (e->failed || e->finished || e->len || out == e->out ||
        (!e->prefix_mode && e->blocks))
        return -1;
    if (!input) {
        if (size)
            return -1;
        input = e->buf;
    }
    e->prefix_mode = 1;
    if (offset > cached && ain_enc_feed(e, input + cached, offset - cached))
        return -1;
    while (e->collected < e->submitted) {
        if (enc_collect(e, 1)) {
            e->failed = 1;
            return -1;
        }
    }
    out->len = 0;
    bw_init(&writer, out);
    if (blocks) {
        const EncCheckpoint *point = &e->checkpoints[blocks - 1];
        if (ain_buf_put(out, e->out->p, point->bytes))
            return -1;
        writer.buf = point->bits;
        writer.cnt = point->count;
    } else {
        bw_write(&writer, 1, 0);
    }
    if (size > offset &&
        lz_parse_ex(input + offset - history, size - offset + history,
                    history, e->toks, e->prev, freqA, freqB, e->speed, &ntok))
        return -1;
    if (emit_block(&writer, input + offset - history,
                   size - offset + history, history, e->toks, ntok,
                   freqA, freqB, 1, e->speed))
        return -1;
    bw_flush(&writer);
    return writer.err ? -1 : 0;
}

int ain_enc_reset_prefix(AinEncS *e)
{
    if (!e->prefix_mode || e->failed || e->finished || e->len ||
        e->submitted != e->collected)
        return -1;
    e->out->len = 0;
    e->H = e->blocks = 0;
    e->submitted = e->collected = e->checkpoint_count = 0;
    bw_init(&e->w, e->out);
    return 0;
}

static void bw_append_range(BitWriter *writer, const uint8_t *input,
                             size_t first, size_t end)
{
    size_t pos = first / 8, bits = end - first;
    int shift = (int)(first % 8);
    if (shift && bits) {
        int take = bits < (size_t)(8 - shift) ? (int)bits : 8 - shift;
        bw_write(writer, take, (input[pos++] >> shift) & ((1u << take) - 1));
        bits -= take;
    }
    while (bits >= 32) {
        uint32_t value;
        memcpy(&value, input + pos, sizeof value);
        bw_write(writer, 32, value);
        pos += 4;
        bits -= 32;
    }
    while (bits >= 8) {
        bw_write(writer, 8, input[pos++]);
        bits -= 8;
    }
    if (bits)
        bw_write(writer, (int)bits, input[pos] & ((1u << bits) - 1));
}

int ain_enc_rebase_prefix(AinEncS *e, const uint8_t *input, size_t consumed)
{
    size_t chunk = e->cap - AIN_WINDOW;
    size_t blocks = (size_t)e->blocks, skip = consumed / chunk, i;
    AinBuf old;
    BitWriter old_writer;
    if (e->failed || e->finished || !e->prefix_mode || e->len ||
        e->submitted != e->collected || !consumed || !input)
        return -1;
    if (consumed % chunk || skip >= blocks)
        return ain_enc_reset_prefix(e);
    old = *e->out;
    old_writer = e->w;
    old_writer.out = &old;
    bw_flush(&old_writer);
    if (old_writer.err) {
        *e->out = old;
        e->failed = 1;
        return -1;
    }
    *e->out = e->rebase_cache;
    ain_buf_init(&e->rebase_cache);
    if (ain_enc_reset_prefix(e) ||
        ain_enc_feed(e, input + consumed, chunk))
        goto failed;
    while (e->collected < e->submitted) {
        if (enc_collect(e, 1))
            goto failed;
    }
    /* Only the first retained block can reference the discarded history.
     * Later blocks' entire 32 KiB windows lie within the retained prefix.
     * New checkpoint slots precede the source slots, so rebuild in place. */
    for (i = skip + 1; i < blocks; i++) {
        EncCheckpoint start = e->checkpoints[i - 1];
        EncCheckpoint end = e->checkpoints[i];
        bw_append_range(&e->w, old.p, start.bytes * 8 + start.count,
                        end.bytes * 8 + end.count);
        if (e->w.err || enc_checkpoint(e))
            goto failed;
    }
    e->blocks = (int)(blocks - skip);
    e->H = AIN_WINDOW;
    memcpy(e->buf, input + blocks * chunk - AIN_WINDOW, AIN_WINDOW);
    old.len = 0;
    e->rebase_cache = old;
    return 0;
failed:
    ain_buf_free(&old);
    e->failed = 1;
    return -1;
}

int ain_enc_prefix_blocks(AinEncS *e, const uint8_t *input, size_t size,
                          size_t limit, size_t *used, AinBuf *out)
{
    size_t chunk = e->cap - AIN_WINDOW;
    size_t blocks = size / chunk, cached = (size_t)e->blocks * chunk;
    BitWriter writer;
    if (e->failed || e->finished || e->len || out == e->out ||
        (!e->prefix_mode && e->blocks) || (!input && size))
        return -1;
    e->prefix_mode = 1;
    if (blocks * chunk > cached &&
        ain_enc_feed(e, input + cached, blocks * chunk - cached))
        return -1;
    while (e->collected < e->submitted) {
        if (enc_collect(e, 1)) {
            e->failed = 1;
            return -1;
        }
    }
    while (blocks) {
        const EncCheckpoint *point = &e->checkpoints[blocks - 1];
        if (point->bytes + (point->count + 7) / 8 <= limit)
            break;
        blocks--;
    }
    *used = blocks * chunk;
    out->len = 0;
    if (!blocks)
        return 0;
    {
        const EncCheckpoint *point = &e->checkpoints[blocks - 1];
        if (ain_buf_put(out, e->out->p, point->bytes))
            return -1;
        bw_init(&writer, out);
        writer.buf = point->bits;
        writer.cnt = point->count;
        /* A block's last bit selects EOF (1) rather than tree reload (0). */
        if (writer.cnt)
            writer.buf |= UINT64_C(1) << (writer.cnt - 1);
        else
            out->p[out->len - 1] |= 0x80;
        bw_flush(&writer);
    }
    return writer.err ? -1 : 0;
}
