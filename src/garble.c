/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

/* garble.c - AIN "garble" password cipher (bit-exact emulation of ain.exe 2.32).
 *
 * Reverse engineered from the original: a 48-bit carry chain (state a,b,c words,
 * LE) advances one step per output word:
 *
 *     k1 = carry(a + 1)
 *     b += a_old + k1        k2 = carry of that sum
 *     c += b_old + k2        output word = c
 *
 * Seeding runs the same chain once over a 96-byte NUL-padded copy of the
 * password with initial state (a = sext(pw[1]), b = sext(pw[0]), c = 0x1234):
 * every buffer word is XORed with the output c, nine rounds of 48 words each,
 * the state persisting across rounds.  The buffer is then folded into three
 * carry lanes by word index modulo 3:
 *
 *     lane lo: words 3g+2   lane mid: words 3g+1   lane hi: words 3g   (g = 0..15)
 *
 * and (a,b,c) = (lo, mid, hi) becomes the generator seed.  The archive data
 * region (everything between the 24-byte header and the index) is XORed with
 * the LE bytes of successive c words, starting at the first data byte and
 * continuing across member boundaries; an odd final byte gets the low byte of
 * the next c.  header[4..5] stores (a+b+c) & 0xFFFF, which the extractor
 * recomputes from the entered password ("Incorrect password" on mismatch).
 * The index itself is never encrypted.  In garbled archives the per-member
 * checksums cover the *encrypted* stored bytes (verified against the
 * original: its sums over the decrypted region do not match the records).
 */
#include <string.h>
#include "ain.h"

/* advance the chain one word; returns the new c (output word) */
static uint16_t chain_step(uint16_t *pa, uint16_t *pb, uint16_t *pc)
{
    unsigned nd = (unsigned)*pa + 1;
    unsigned k1 = nd >> 16;
    unsigned long long nb = (unsigned long long)*pb + *pa + k1;
    unsigned k2 = (unsigned)(nb >> 16);
    unsigned nc = (unsigned)(*pc) + *pb + k2;
    *pa = (uint16_t)nd;
    *pb = (uint16_t)nb;
    *pc = (uint16_t)nc;
    return *pc;
}

void ain_garble_seed(const char *pw, uint16_t seed[3])
{
    uint8_t buf[96];
    uint16_t a, b, c;
    size_t n = strlen(pw), i;
    int r, g;

    memset(buf, 0, sizeof buf);
    memcpy(buf, pw, n < sizeof buf ? n : sizeof buf);
    c = 0x1234;
    b = (uint16_t)(int8_t)buf[0];            /* mov al,byte / cbw */
    a = (uint16_t)(int8_t)buf[1];
    for (r = 0; r < 9; r++)
        for (i = 0; i < 48; i++) {
            uint16_t w = (uint16_t)(buf[2*i] | (buf[2*i+1] << 8)) ^ chain_step(&a, &b, &c);
            buf[2*i]     = (uint8_t)w;
            buf[2*i + 1] = (uint8_t)(w >> 8);
        }
    /* fold into 3 carry lanes: lo=words 3g+2, mid=words 3g+1, hi=words 3g */
    {
        uint16_t lo = 0, mid = 0, hi = 0;
        unsigned t;
        for (g = 0; g < 16; g++) {
            const uint8_t *q = buf + 6*g;
            uint16_t w0 = (uint16_t)(q[0] | (q[1] << 8));
            uint16_t w1 = (uint16_t)(q[2] | (q[3] << 8));
            uint16_t w2 = (uint16_t)(q[4] | (q[5] << 8));
            t = (unsigned)lo + w2;        lo = (uint16_t)t;
            t = (unsigned)mid + w1 + (t >> 16); mid = (uint16_t)t;
            hi = (uint16_t)(hi + w0 + (unsigned)(t >> 16));
        }
        seed[0] = lo; seed[1] = mid; seed[2] = hi;
    }
}

uint16_t ain_garble_hash(const char *pw)
{
    uint16_t s[3];
    ain_garble_seed(pw, s);
    return (uint16_t)(s[0] + s[1] + s[2]);
}

void ain_garble_init(AinGarbleSt *g, const uint16_t seed[3])
{
    g->a = seed[0];
    g->b = seed[1];
    g->c = seed[2];
    g->hi = 0;
    g->hi_pending = 0;
}

/* XOR n bytes continuing the keystream; slices may end at any byte, an
 * unconsumed word half carries over in the state                     */
void ain_garble_run(AinGarbleSt *g, uint8_t *p, size_t n)
{
    size_t i = 0;
    if (g->hi_pending && n) {
        p[0] ^= (uint8_t)g->hi;
        i = 1;
        g->hi_pending = 0;
    }
    while (i < n) {
        uint16_t w = chain_step(&g->a, &g->b, &g->c);
        p[i++] ^= (uint8_t)w;
        if (i < n)
            p[i++] ^= (uint8_t)(w >> 8);
        else {
            g->hi = (uint8_t)(w >> 8);
            g->hi_pending = 1;
        }
    }
}

/* XOR n bytes at p, keystream restarted at the beginning of the stream;
 * caller passes region-aligned slices (region start == first data byte) */
void ain_garble_xor(const uint16_t seed[3], uint8_t *p, size_t n)
{
    AinGarbleSt g;
    ain_garble_init(&g, seed);
    ain_garble_run(&g, p, n);
}
