// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * daa2iso.cpp  –  Convert PowerISO DAA / gBurner GBI disk images to ISO
 *
 * Original reverse-engineering and algorithms by Luigi Auriemma (aluigi.org)
 *
 * C++ Linux port + Library API (no Windows dependencies) — 2026
 *
 * Enhancements:
 *  - Full 64-bit architecture support for files > 4GB (size_t/u64 migration)
 *  - Thread-safe refactor: all global/static state moved to per-instance
 *    context for concurrent processing.
 *  - Removed legacy Windows dependencies and password/encryption logic.
 *
 * License: GPL-2.0 or (at your option) any later version (same as original)
 *
 */

/*
    Copyright 2007,2008,2009 Luigi Auriemma

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

    http://www.gnu.org/licenses/gpl-2.0.txt
*/

// ─── large-file support ────────────────────────────────────────────────────

#define _FILE_OFFSET_BITS 64

// ___________________________________________________________________________

#include <stddef.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

import std;
import isocmd.converters.daa2iso;
import isocmd.core.cancellation;

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════
//  LZMA decoder (Igor Pavlov / 7-zip SDK, minimal subset)
// ═══════════════════════════════════════════════════════════════════════════

#define LZMA_PROPS_SIZE 5
#define SZ_OK           0
#define SZ_ERROR_DATA   1

typedef size_t SizeT;
typedef int    SRes;

typedef void *(*ISzAlloc_Alloc)(void *, size_t);
typedef void  (*ISzAlloc_Free) (void *, void *);
struct ISzAlloc { ISzAlloc_Alloc Alloc; ISzAlloc_Free Free; };
static void *SzAlloc(void *, size_t sz) { return malloc(sz);  }
static void  SzFree (void *, void *p)   { free(p);            }
static ISzAlloc g_Alloc = { SzAlloc, SzFree };

#define kNumBitModelTotalBits 11
#define kBitModelTotal        (1 << kNumBitModelTotalBits)
#define kNumMoveBits          5

typedef uint16_t CLzmaProb;

struct CLzmaDec {
    CLzmaProb  *probs;
    u8         *dic;
    SizeT       dicBufSize;
    SizeT       dicPos;
    const u8   *buf;
    u32         range, code;
    SizeT       processedPos;
    SizeT       checkDicSize;
    unsigned    state;
    u32         reps[4];
    unsigned    remainLen;
    int         needFlush, needInitState, needInitProbs;
    u32         numProbs;
    unsigned    tempBufSize;
    u8          tempBuf[LZMA_PROPS_SIZE];
    unsigned    lc, pb, lp;
};

#define LZMA_BASE_SIZE   1846
#define LZMA_LIT_SIZE    768
#define LzmaProps_GetNumProbs(lc,lp) (LZMA_BASE_SIZE + (LZMA_LIT_SIZE << ((lc)+(lp))))

static void LzmaDec_Construct(CLzmaDec *p) { p->dic = nullptr; p->probs = nullptr; }

static SRes LzmaDec_AllocateProbs(CLzmaDec *p, const u8 *props, unsigned propsSize, ISzAlloc *alloc) {
    if (propsSize < LZMA_PROPS_SIZE) return SZ_ERROR_DATA;
    unsigned lc = props[0] % 9;
    unsigned rem = props[0] / 9;
    unsigned lp = rem % 5;
    unsigned pb = rem / 5;
    if (pb > 4) return SZ_ERROR_DATA;
    p->lc = lc; p->lp = lp; p->pb = pb;
    u32 num = LzmaProps_GetNumProbs(lc, lp);
    if (p->probs && p->numProbs == num) return SZ_OK;
	if (p->probs) {
		alloc->Free(alloc, p->probs);
		p->probs = nullptr;
	}
	p->probs = (CLzmaProb *)alloc->Alloc(alloc, num * sizeof(CLzmaProb));
    if (!p->probs) return SZ_ERROR_DATA;
    p->numProbs = num;
    return SZ_OK;
}

static SRes LzmaDec_Allocate(CLzmaDec *p, const u8 *props, unsigned propsSize, ISzAlloc *alloc) {
    return LzmaDec_AllocateProbs(p, props, propsSize, alloc);
}

static void LzmaDec_Free(CLzmaDec *p, ISzAlloc *alloc) {
    alloc->Free(alloc, p->probs); p->probs = nullptr;
}

static void LzmaDec_InitProbs(CLzmaDec *p) {
    for (u32 i = 0; i < p->numProbs; i++) p->probs[i] = kBitModelTotal >> 1;
    p->needInitProbs = 0;
}

static void LzmaDec_InitState(CLzmaDec *p) {
    p->state = 0;
    p->reps[0] = p->reps[1] = p->reps[2] = p->reps[3] = 1;
    p->remainLen = 0;
    p->needInitState = 0;
}

static void LzmaDec_Init(CLzmaDec *p) {
    p->dicPos = 0;
    p->needFlush = 1;
    p->remainLen = 0;
    p->tempBufSize = 0;
    p->processedPos = 0;
    p->checkDicSize = 0;
    LzmaDec_InitProbs(p);
    LzmaDec_InitState(p);
}

#define NORMALIZE if (p->range < (1 << 24)) { p->range <<= 8; p->code = (p->code << 8) | (*p->buf++); }

#define GET_BIT2(prob, mi, A0, A1) \
    { u32 ttt = *(prob); \
      u32 bound = (p->range >> kNumBitModelTotalBits) * ttt; \
      if (p->code < bound) { p->range = bound; ttt += (kBitModelTotal - ttt) >> kNumMoveBits; *(prob) = (CLzmaProb)ttt; mi = (mi + mi); A0; } \
      else { p->range -= bound; p->code -= bound; ttt -= ttt >> kNumMoveBits; *(prob) = (CLzmaProb)ttt; mi = (mi + mi + 1); A1; } \
      NORMALIZE; }

#define GET_BIT(prob, mi)   GET_BIT2(prob, mi, ; , ; )

#define TREE_GET_BIT(probs, mi)  GET_BIT((probs) + mi, mi)
#define TREE_DECODE(probs, limit, mi) \
  { mi = 1; do { TREE_GET_BIT(probs, mi); } while (mi < limit); mi -= limit; }

#define kNumPosBitsMax      4
#define kNumPosStatesMax    (1 << kNumPosBitsMax)
#define kLenNumLowBits      3
#define kLenNumLowSymbols   (1 << kLenNumLowBits)
#define kLenNumMidBits      3
#define kLenNumMidSymbols   (1 << kLenNumMidBits)
#define kLenNumHighBits     8
#define kLenNumHighSymbols  (1 << kLenNumHighBits)

#define IsMatch       0
#define IsRep         192
#define IsRepG0       204
#define IsRepG1       216
#define IsRepG2       228
#define IsRep0Long    240
#define PosSlot       272
#define SpecPos       400
#define Align         616
#define LenCoder      632
#define RepLenCoder   792
#define Literal       952

#define kNumStates             12
#define kNumLenToBitModelTotal 10
#define kMatchMinLen           2

/**
 * FIXED LZMA DECODER
 * Supports volumes > 4GB and fixes Double-Free crashes.
 */

static u32 lzma_decode_len(CLzmaDec *p, CLzmaProb *prob, u32 posState) {
    u32 ttt, bound, sym;
    CLzmaProb *pc = prob;

    ttt = *pc;
    bound = (p->range >> kNumBitModelTotalBits) * ttt;
    if (p->code < bound) {
        p->range = bound;
        ttt += (kBitModelTotal - ttt) >> kNumMoveBits;
        *pc = (CLzmaProb)ttt;
        NORMALIZE;
        pc = prob + 1 + (posState << kLenNumLowBits);
        TREE_DECODE(pc, kLenNumLowSymbols, sym);
        return sym;
    }

    p->range -= bound;
    p->code -= bound;
    ttt -= ttt >> kNumMoveBits;
    *pc = (CLzmaProb)ttt;
    NORMALIZE;

    pc = prob + 1 + (kNumPosStatesMax << kLenNumLowBits);
    ttt = *pc;
    bound = (p->range >> kNumBitModelTotalBits) * ttt;
    if (p->code < bound) {
        p->range = bound;
        ttt += (kBitModelTotal - ttt) >> kNumMoveBits;
        *pc = (CLzmaProb)ttt;
        NORMALIZE;
        pc = prob + 1 + (kNumPosStatesMax << kLenNumLowBits) + 1 + (posState << kLenNumMidBits);
        TREE_DECODE(pc, kLenNumMidSymbols, sym);
        return kLenNumLowSymbols + sym;
    }

    p->range -= bound;
    p->code -= bound;
    ttt -= ttt >> kNumMoveBits;
    *pc = (CLzmaProb)ttt;
    NORMALIZE;

    pc = prob + 1 + (kNumPosStatesMax << kLenNumLowBits) + 1 + (kNumPosStatesMax << kLenNumMidBits);
    TREE_DECODE(pc, kLenNumHighSymbols, sym);
    return kLenNumLowSymbols + kLenNumMidSymbols + sym;
}

static size_t lzma_decode_distance(CLzmaDec *p, CLzmaProb *probs, int len) {
    u32 posSlot;
    int numDirectBits;
    u32 distance;
    unsigned lenState = (unsigned)(len < kNumLenToBitModelTotal - 1 ? len : kNumLenToBitModelTotal - 1);
    CLzmaProb *prob = probs + PosSlot + (lenState << 6);

    TREE_DECODE(prob, 1 << 6, posSlot);

    if (posSlot < 4) {
        distance = posSlot;
    } else {
        numDirectBits = (int)(((posSlot >> 1) - 1));
        distance = (2 | (posSlot & 1));
        if (posSlot < 14) {
            distance <<= numDirectBits;
            prob = probs + SpecPos + distance - posSlot - 1;
            u32 mask = 1;
            distance += mask;
            for (int i = numDirectBits - 1; i >= 4; i--) {
                u32 tt2 = *(prob + (distance >> 1));
                u32 bd = (p->range >> kNumBitModelTotalBits) * tt2;
                if (p->code < bd) {
                    p->range = bd;
                    tt2 += (kBitModelTotal - tt2) >> kNumMoveBits;
                    *(prob + (distance >> 1)) = (CLzmaProb)tt2;
                    NORMALIZE;
                } else {
                    p->range -= bd;
                    p->code -= bd;
                    tt2 -= tt2 >> kNumMoveBits;
                    *(prob + (distance >> 1)) = (CLzmaProb)tt2;
                    NORMALIZE;
                    distance |= mask;
                }
                mask <<= 1;
                distance <<= 1;
            }
            prob = probs + Align;
            distance <<= 4;
            u32 sym2 = 1;
            for (int i = 0; i < 4; i++) { TREE_GET_BIT(prob, sym2); }
            distance |= sym2 & 0xF;
        } else {
            numDirectBits -= 4;
            u32 directBitsResult = 0;
            for (int i = numDirectBits - 1; i >= 0; i--) {
                p->range >>= 1;
                if (p->code >= p->range) {
                    p->code -= p->range;
                    directBitsResult |= (1 << i);
                }
                NORMALIZE;
            }
            distance = ((distance << numDirectBits) | directBitsResult) << 4;
            prob = probs + Align;
            u32 sym2 = 1;
            for (int i = 0; i < 4; i++) { TREE_GET_BIT(prob, sym2); }
            distance |= sym2 & 0xF;
        }
    }
    return (size_t)distance + 1;
}

typedef enum {
    LZMA_STEP_NEXT,   /* symbol fully handled, go to next loop iteration */
    LZMA_STEP_MATCH,  /* a match must be copied (v->len / v->rep0 are set) */
    LZMA_STEP_DONE    /* stop decoding and return v->result */
} LzmaStep;

typedef struct {
    unsigned state;
    size_t   rep0, rep1, rep2, rep3;
    int      len;
    size_t   result;
} LzmaVars;

/* Decode one adaptive bit (the update / normalize pattern repeated everywhere). */
static unsigned lzma_decode_bit(CLzmaDec *p, CLzmaProb *prob) {
    u32 ttt = *prob;
    u32 bound = (p->range >> kNumBitModelTotalBits) * ttt;

    if (p->code < bound) {
        p->range = bound;
        ttt += (kBitModelTotal - ttt) >> kNumMoveBits;
        *prob = (CLzmaProb)ttt;
        NORMALIZE;
        return 0;
    }
    p->range -= bound; p->code -= bound;
    ttt -= ttt >> kNumMoveBits; *prob = (CLzmaProb)ttt;
    NORMALIZE;
    return 1;
}

/* Position in the dictionary that is `rep0` bytes behind the write position. */
static size_t lzma_dic_src(const CLzmaDec *p, size_t rep0) {
    return (p->dicPos >= rep0) ? (p->dicPos - rep0)
                               : (p->dicPos + p->dicBufSize - rep0);
}

static void lzma_init(CLzmaDec *p, const u8 *in, u8 *out, size_t outsz) {
    // Basic Initialization
    p->dic         = out;
    p->dicBufSize  = outsz;
    p->dicPos      = 0;
    p->buf         = in;
    p->code        = 0;
    p->range       = 0xFFFFFFFF;

    // Load first 5 bytes
    for (int i = 0; i < 5; i++) p->code = (p->code << 8) | (*p->buf++);
}

static CLzmaProb *lzma_literal_probs(CLzmaDec *p, CLzmaProb *probs) {
    CLzmaProb *prob = probs + Literal;
    if (p->processedPos != 0 || p->checkDicSize != 0) {
        u8 prevByte = (p->dicPos == 0) ? 0 : p->dic[p->dicPos - 1];
        prob += (LZMA_LIT_SIZE * (((p->processedPos & ((1 << p->lp) - 1)) << p->lc)
                + (prevByte >> (8 - p->lc))));
    }
    return prob;
}

/* Literal decoding when the previous symbol was a match (uses the match byte). */
static u32 lzma_decode_matched_literal(CLzmaDec *p, CLzmaProb *prob, u8 matchByte) {
    u32 symbol = 1;

    do {
        u32 bit;
        CLzmaProb *probLit;
        matchByte <<= 1;
        bit = matchByte & 0x100;
        probLit = prob + 0x100 + bit + symbol;
        GET_BIT2(probLit, symbol, bit = 0, bit = 0x100);
        if ((matchByte & 0x100) != bit) {
            while (symbol < 0x100) { GET_BIT(prob + symbol, symbol); }
            break;
        }
    } while (symbol < 0x100);
    return symbol;
}

static LzmaStep lzma_step_literal(CLzmaDec *p, CLzmaProb *probs, LzmaVars *v) {
    CLzmaProb *prob = lzma_literal_probs(p, probs);
    u32 symbol = 1;

    if (v->state >= 7) {
        u8 matchByte = p->dic[lzma_dic_src(p, v->rep0)];
        symbol = lzma_decode_matched_literal(p, prob, matchByte);
    }
    while (symbol < 0x100) { GET_BIT(prob + symbol, symbol); }

    p->dic[p->dicPos++] = (u8)symbol;
    p->processedPos++;
    v->state = (v->state < 4) ? 0 : (v->state < 10) ? v->state - 3 : v->state - 6;
    return LZMA_STEP_NEXT;
}

/* IsRep == 0: a normal match with a freshly decoded distance. */
static LzmaStep lzma_step_simple_match(CLzmaDec *p, CLzmaProb *probs, LzmaVars *v, u32 posState) {
    v->rep3 = v->rep2; v->rep2 = v->rep1; v->rep1 = v->rep0;
    v->len = (int)lzma_decode_len(p, probs + LenCoder, posState);
    v->state = (v->state < 7) ? 7 : 10;

    v->rep0 = lzma_decode_distance(p, probs, v->len);
    if (v->rep0 == 0) {
        v->result = p->dicPos;
        return LZMA_STEP_DONE;
    }
    return LZMA_STEP_MATCH;
}

/* IsRepG0 == 0 and IsRep0Long == 0: single byte copied from rep0. */
static LzmaStep lzma_step_short_rep(CLzmaDec *p, LzmaVars *v) {
    if (p->dicPos == 0 && p->checkDicSize == 0) {
        v->result = 0;
        return LZMA_STEP_DONE;
    }
    size_t srcPos = lzma_dic_src(p, v->rep0);
    if (p->dicPos >= p->dicBufSize) {
        v->result = p->dicPos;
        return LZMA_STEP_DONE;
    }
    p->dic[p->dicPos++] = p->dic[srcPos];
    p->processedPos++;
    v->state = (v->state < 7) ? 9 : 11;
    return LZMA_STEP_NEXT;
}

/* IsRepG0 == 1: choose rep1 / rep2 / rep3 and rotate the rep history. */
static void lzma_select_rep_distance(CLzmaDec *p, CLzmaProb *probs, LzmaVars *v) {
    size_t distance;

    if (lzma_decode_bit(p, probs + IsRepG1 + v->state) == 0) {
        distance = v->rep1;
    } else {
        if (lzma_decode_bit(p, probs + IsRepG2 + v->state) == 0) {
            distance = v->rep2;
        } else {
            distance = v->rep3;
            v->rep3 = v->rep2;
        }
        v->rep2 = v->rep1;
    }
    v->rep1 = v->rep0;
    v->rep0 = distance;
}

/* IsRep == 1: repeated match. */
static LzmaStep lzma_step_rep_match(CLzmaDec *p, CLzmaProb *probs, LzmaVars *v, u32 posState) {
    if (lzma_decode_bit(p, probs + IsRepG0 + v->state) == 0) {
        CLzmaProb *prob = probs + IsRep0Long + (v->state << kNumPosBitsMax) + posState;
        if (lzma_decode_bit(p, prob) == 0)
            return lzma_step_short_rep(p, v);
    } else {
        lzma_select_rep_distance(p, probs, v);
    }

    v->len = (int)lzma_decode_len(p, probs + RepLenCoder, posState);
    v->state = (v->state < 7) ? 8 : 11;
    return LZMA_STEP_MATCH;
}

/* Copies the match; returns 0 if the distance is invalid. */
static int lzma_copy_match(CLzmaDec *p, size_t rep0, int len) {
    // FINAL MATCH COPY
    if (rep0 > p->dicPos + p->checkDicSize) return 0;
    while (len--) {
        if (p->dicPos >= p->dicBufSize) break;
        size_t srcPos = lzma_dic_src(p, rep0);
        p->dic[p->dicPos++] = p->dic[srcPos];
        p->processedPos++;
    }
    return 1;
}

static size_t lzma_decode_full(CLzmaDec *p, const u8 *in, size_t insz, u8 *out, size_t outsz) {
    lzma_init(p, in, out, outsz);
    (void)insz;

    LzmaVars v = { 0, 1, 1, 1, 1, 0, 0 };
    CLzmaProb *probs = p->probs;

    for (;;) {
        if (p->dicPos >= p->dicBufSize) return p->dicPos;

        u32 posState = (u32)((size_t)p->dicPos & ((1 << p->pb) - 1));
        CLzmaProb *prob = probs + IsMatch + (v.state << kNumPosBitsMax) + posState;

        LzmaStep step;
        if (lzma_decode_bit(p, prob) == 0) {
            step = lzma_step_literal(p, probs, &v);
        } else if (lzma_decode_bit(p, probs + IsRep + v.state) == 0) {
            step = lzma_step_simple_match(p, probs, &v, posState);
        } else {
            step = lzma_step_rep_match(p, probs, &v, posState);
        }

        if (step == LZMA_STEP_DONE) return v.result;
        if (step == LZMA_STEP_NEXT) continue;

        v.len += kMatchMinLen;
        if (!lzma_copy_match(p, v.rep0, v.len)) return 0;
    }
    return p->dicPos;
}

// ═══════════════════════════════════════════════════════════════════════════
//  tinflate – Luigi Auriemma's modified tiny inflate (btype table swapped)
//  NOTE: All previously static/global tinflate state is now per-instance,
//        stored in DaaContext, and passed explicitly to every function.
// ═══════════════════════════════════════════════════════════════════════════

#define TINF_OK         0
#define TINF_DATA_ERROR (-3)

struct TINF_TREE {
    unsigned short table[16];
    unsigned short trans[288];
};

struct TINF_DATA {
    const u8      *source;
    unsigned int   tag;
    unsigned int   bitcount;
    u8            *dest;
    size_t        *destLen;     // Changed to size_t
    size_t         sourceLen;   // Changed to size_t
    size_t         sourceSize;  // Changed to size_t
    size_t         destSize;    // Changed to size_t
    TINF_TREE      ltree;
    TINF_TREE      dtree;
};

// Per-context tinflate tables (replaces the old static globals)
struct TinfTables {
    TINF_TREE sltree, sdtree;
    u8        length_bits[30];
    u16       length_base[30];
    u8        dist_bits[30];
    u16       dist_base[30];
};

static const u8 clcidx[] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static void tinf_build_bits_base(u8 *bits, u16 *base, int delta, int first) {
    int i, sum;
    for (i = 0; i < delta; ++i) bits[i] = 0;
    for (i = 0; i < 30 - delta; ++i) bits[i+delta] = i/delta;
    for (sum = first, i = 0; i < 30; ++i) { base[i] = sum; sum += 1 << bits[i]; }
}

static void tinf_build_fixed_trees(TINF_TREE *lt, TINF_TREE *dt) {
    int i;
    for (i = 0; i < 7; ++i) lt->table[i] = 0;
    lt->table[7] = 24; lt->table[8] = 152; lt->table[9] = 112;
    for (i = 0; i < 24; ++i)  lt->trans[i]           = 256+i;
    for (i = 0; i < 144; ++i) lt->trans[24+i]         = i;
    for (i = 0; i < 8; ++i)   lt->trans[24+144+i]     = 280+i;
    for (i = 0; i < 112; ++i) lt->trans[24+144+8+i]   = 144+i;
    for (i = 0; i < 5; ++i)  dt->table[i] = 0;
    dt->table[5] = 32;
    for (i = 0; i < 32; ++i) dt->trans[i] = i;
}

static void tinf_build_tree(TINF_TREE *t, const u8 *lengths, unsigned num) {
    unsigned short offs[16];
    unsigned i, sum;
    for (i = 0; i < 16; ++i) t->table[i] = 0;
    for (i = 0; i < num; ++i) t->table[lengths[i]]++;
    t->table[0] = 0;
    for (sum = 0, i = 0; i < 16; ++i) { offs[i] = sum; sum += t->table[i]; }
    for (i = 0; i < num; ++i) if (lengths[i]) t->trans[offs[lengths[i]]++] = i;
}

static void tinf_init(TinfTables &tt) {
    tinf_build_fixed_trees(&tt.sltree, &tt.sdtree);
    tinf_build_bits_base(tt.length_bits, tt.length_base, 4, 3);
    tinf_build_bits_base(tt.dist_bits,   tt.dist_base,   2, 1);
    tt.length_bits[28] = 0; tt.length_base[28] = 258;
}

static int tinf_getbit(TINF_DATA *d) {
    if (!d->bitcount--) {
        if ((d->sourceLen+1) > d->sourceSize) return 0;
        d->tag = *d->source++; d->sourceLen++; d->bitcount = 7;
    }
    int bit = d->tag & 1; d->tag >>= 1; return bit;
}

static unsigned int tinf_read_bits(TINF_DATA *d, int num, int base) {
    unsigned val = 0;
    if (num) {
        unsigned limit = 1u << num, mask;
        for (mask = 1; mask < limit; mask <<= 1) if (tinf_getbit(d)) val += mask;
    }
    return val + base;
}

static int tinf_decode_symbol(TINF_DATA *d, TINF_TREE *t) {
    int sum = 0, cur = 0, len = 0;
    do { cur = 2*cur + tinf_getbit(d); ++len; sum += t->table[len]; cur -= t->table[len]; }
    while (cur >= 0);
    return t->trans[sum+cur];
}

static int tinf_decode_trees(TINF_DATA *d, TINF_TREE *lt, TINF_TREE *dt) {
    TINF_TREE code_tree;
    u8 lengths[288+32];
    unsigned hlit = tinf_read_bits(d,5,257);
    unsigned hdist= tinf_read_bits(d,5,1);
    unsigned hclen= tinf_read_bits(d,4,4);
    unsigned i, num, length;
    for (i = 0; i < 19; ++i) lengths[i] = 0;
    for (i = 0; i < hclen; ++i) { unsigned clen = tinf_read_bits(d,3,0); lengths[clcidx[i]] = clen; }
    tinf_build_tree(&code_tree, lengths, 19);
    for (num = 0; num < hlit+hdist; ) {
        int sym = tinf_decode_symbol(d, &code_tree);
        switch (sym) {
        case 16: {
            u8 prev = lengths[num-1];
            length = tinf_read_bits(d,2,3);
            if ((num+length)>(288+32)) return TINF_DATA_ERROR;
            for (; length; --length) lengths[num++] = prev;
            break;
        }
        case 17: {
            length = tinf_read_bits(d,3,3);
            if ((num+length)>(288+32)) return TINF_DATA_ERROR;
            for (; length; --length) lengths[num++] = 0;
            break;
        }
        case 18: {
            length = tinf_read_bits(d,7,11);
            if ((num+length)>(288+32)) return TINF_DATA_ERROR;
            for (; length; --length) lengths[num++] = 0;
            break;
        }
        default: if ((num+1)>(288+32)) return TINF_DATA_ERROR;
                 lengths[num++] = sym; break;
        }
    }
    tinf_build_tree(lt, lengths, hlit);
    tinf_build_tree(dt, lengths+hlit, hdist);
    return TINF_OK;
}

static int tinf_inflate_block_data(TINF_DATA *d, TINF_TREE *lt, TINF_TREE *dt,
                                    const TinfTables &tt) {
    while (1) {
        int sym = tinf_decode_symbol(d, lt);
        if (sym == 256) break;
        if (sym < 256) {
            if ((*d->destLen+1) > d->destSize) return TINF_DATA_ERROR;
            *d->dest++ = sym; *d->destLen += 1;
        } else {
            sym -= 257;
            int length = tinf_read_bits(d, tt.length_bits[sym], tt.length_base[sym]);
            int dist   = tinf_decode_symbol(d, dt);
            int offs   = tinf_read_bits(d, tt.dist_bits[dist], tt.dist_base[dist]);
            if ((*d->destLen+length) > d->destSize) return TINF_DATA_ERROR;
            for (int i = 0; i < length; ++i) d->dest[i] = d->dest[i-offs];
            d->dest += length; *d->destLen += length;
        }
    }
    return TINF_OK;
}

static int tinf_inflate_uncompressed_block(TINF_DATA *d) {
    if ((d->sourceLen+4) > d->sourceSize) return TINF_DATA_ERROR;
    unsigned length    = (unsigned)d->source[1]<<8 | d->source[0];
    unsigned invlength = (unsigned)d->source[3]<<8 | d->source[2];
    if (length != (~invlength & 0xffff)) return TINF_DATA_ERROR;
    d->source += 4; d->sourceLen += 4;
    if ((d->sourceLen+length) > d->sourceSize) return TINF_DATA_ERROR;
    if ((*d->destLen+length) > d->destSize)    return TINF_DATA_ERROR;
    for (unsigned i = length; i; --i) *d->dest++ = *d->source++;
    d->bitcount = 0; d->sourceLen += length; *d->destLen += length;
    return TINF_OK;
}

static int tinf_uncompress(TinfTables &tt,
                            void *dest, size_t *destLen, // Changed to size_t
                            const void *source, size_t sourceLen, // Changed to size_t
                            const unsigned swapped_btype[3]) {
    TINF_DATA d;
    d.source     = (const u8 *)source;
    d.bitcount   = 0;
    d.dest       = (u8 *)dest;
    d.destLen    = destLen;
    d.sourceLen  = 0;
    d.sourceSize = sourceLen;
    d.destSize   = *destLen;
    *destLen = 0;
    int bfinal;
    do {
        bfinal = tinf_getbit(&d);
        unsigned btype = tinf_read_bits(&d, 2, 0);
        int res;
        if      (btype == swapped_btype[0]) res = tinf_inflate_uncompressed_block(&d);
        else if (btype == swapped_btype[1]) res = tinf_inflate_block_data(&d, &tt.sltree, &tt.sdtree, tt);
        else if (btype == swapped_btype[2]) {
            if (tinf_decode_trees(&d, &d.ltree, &d.dtree) != TINF_OK) return TINF_DATA_ERROR;
            res = tinf_inflate_block_data(&d, &d.ltree, &d.dtree, tt);
        } else return TINF_DATA_ERROR;
        if (res != TINF_OK) return TINF_DATA_ERROR;
    } while (!bfinal);
    return TINF_OK;
}

// ═══════════════════════════════════════════════════════════════════════════
//  DAA obfuscation functions
// ═══════════════════════════════════════════════════════════════════════════

static void gburner_lame(u8 *data, int size, u8 crc8) {
    u8 d = size >> 2;
    for (int i = 0; i < size; i++) { data[i] -= crc8; data[i] ^= d; }
}

static void poweriso_lame(u8 *data, int size, u64 isosize) {
    isosize /= 0x800;
    u8 a = (isosize >> 8) & 0xff;
    u8 c = isosize & 0xff;
    for (int i = 0; i < size; i++) { data[i] -= c; c += a; }
}

static void poweriso_is_shit(u8 *chunk, int chunksize) {
    static const u8 shit[8] = {0,1,2,2,3,3,3,3};
    static const u8 shiz[8] = {1,1,1,0,1,0,0,0};
    u32 num, e10 = (u32)-1, e20 = 5, e28 = 0, bp = 0;
    u8  tmp;
    for (int i = 0; (i+5) <= chunksize; i++) {
        if ((chunk[i] & 0xfe) != 0xe8) continue;
        if ((i - e10) <= 3) bp = (bp << ((i-e10-1)&0xff)) & 7;
        else bp = 0;
        if (bp) {
            tmp = chunk[i - shit[bp] + 4];
            if (!shiz[bp] || !tmp || (tmp == 0xff)) {
                bp = ((bp&3)<<1)|1; e10 = i; continue;
            }
        }
        e10 = i;
        if ((chunk[i+4] != 0) && (chunk[i+4] != 0xff)) { bp = ((bp&3)<<1)|1; continue; }
        num = ((u32)chunk[i+4]<<24)|((u32)chunk[i+3]<<16)|((u32)chunk[i+2]<<8)|chunk[i+1];
        for (;;) {
            if (!e28) num -= i + e20;
            else      num += i + e20;
            if (!bp) break;
            tmp = num >> (24-(shit[bp]<<3));
            if (tmp && (tmp != 0xff)) break;
            num ^= ((1u << (32-(shit[bp]<<3)))-1);
        }
        chunk[++i] = num;
        chunk[++i] = num >> 8;
        chunk[++i] = num >> 16;
        chunk[++i] = (u8)(~(((num>>24)&1)-1));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Bit-reader for v1.10 index table
//  powerisuxn is now passed by reference (stored in DaaContext) instead of
//  being a static global, making concurrent calls fully independent.
// ═══════════════════════════════════════════════════════════════════════════

static const u8 powerisux[] = "\x0A\x35\x2D\x3F\x08\x33\x09\x15";

static unsigned daa2iso_read_bits(unsigned bits, const u8 *in, unsigned in_bits,
                                   unsigned lame, int lame_increase,
                                   int &powerisuxn) {
    unsigned seek_bits, rem, seek = 0, ret = 0;
    u32 mask = 0xffffffff;
    if (bits > 32) return 0;
    if (bits < 32) mask = (1u << bits) - 1;
    for (;;) {
        seek_bits = in_bits & 7;
        ret |= ((in[in_bits >> 3] >> seek_bits)) << seek;
        rem = 8 - seek_bits;
        if (rem >= bits) break;
        bits    -= rem;
        in_bits += rem;
        seek    += rem;
    }
    if (lame) {
        ret ^= ((powerisuxn ^ powerisux[powerisuxn & 7]) & 0xff) * 0x01010101u;
        if (lame_increase) powerisuxn++;
    }
    return ret & mask;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Case-insensitive suffix check
// ═══════════════════════════════════════════════════════════════════════════

static u8 *find_ext(u8 *fname, const char *ext) {
    int len    = strlen((char*)fname);
    int extlen = strlen(ext);
    u8 *ret    = fname + len - extlen;
    if (len >= extlen) {
        for (int i = 0; i < extlen; i++)
            if (tolower(ret[i]) != tolower((unsigned char)ext[i])) return nullptr;
        return ret;
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Library context
//  TinfTables and powerisuxn are per-instance — no shared mutable state.
// ═══════════════════════════════════════════════════════════════════════════

struct DaaContext {
    FILE       *fdi            = nullptr;
    FILE       *fdo            = nullptr;
    std::string outputPath;

    int         multi          = 0;
    int         multinum       = 0;
    char       *multi_filename = nullptr;

    int         endian         = 0;
    int         daagbi         = TYPE_DAA;
    unsigned    swapped_btype[3] = {0, 1, 2};

    u8         *in             = nullptr;
    u32         insz           = 0;
    u8         *out_buf        = nullptr;
    u32         outsz          = 0;

    CLzmaDec    lzma           = {};

    // Per-instance tinflate tables (was static globals — race condition fix)
    TinfTables  tinf           = {};

    // Per-instance bit-reader counter (was static global — race condition fix)
    int         powerisuxn     = 0;

    std::atomic<size_t> *completedBytes = nullptr;

    ~DaaContext() {
		if (fdi) { fclose(fdi); fdi = nullptr; }
		if (fdo) { fclose(fdo); fdo = nullptr; }
		if (in) { free(in); in = nullptr; }
		if (out_buf) { free(out_buf); out_buf = nullptr; }
		if (multi_filename) { free(multi_filename); multi_filename = nullptr; }

		// Safety check for parallel threads
		if (lzma.probs != nullptr) {
			LzmaDec_Free(&lzma, &g_Alloc);
			lzma.probs = nullptr; // Ensure this is NULL so the next thread doesn't try to free it
		}
	}
};

struct DaaError {
    const char *msg;
    explicit DaaError(const char *m) : msg(m) {}
};

static FILE *ctx_next_volume(DaaContext &ctx) {
    static const char *fmts[] = { "%03d.daa", "%02d.daa", ".d%02d" };
    if (!ctx.multi_filename)
        throw DaaError("multi_filename not initialised");

    char *toadd = ctx.multi_filename + strlen(ctx.multi_filename);
    int written = snprintf(toadd, 32, fmts[ctx.multi - 1], ctx.multinum);
	if (written < 0 || written >= 32)
		throw DaaError("volume filename overflow");

    FILE *fd = fopen(ctx.multi_filename, "rb");
    if (!fd) throw DaaError("cannot open next volume");

    daa_t daa;
    if (fread(&daa, 1, sizeof(daa), fd) != sizeof(daa)) {
        fclose(fd); throw DaaError("volume header read error");
    }
    swap_daa_if_be(&daa, ctx.endian);
    if (strncmp((char*)daa.sign,"DAA VOL",16) && strncmp((char*)daa.sign,"GBI VOL",16)) {
        fclose(fd); throw DaaError("wrong DAA VOL signature");
    }
    if (fseek(fd, daa.size_offset, SEEK_SET)) {
        fclose(fd); throw DaaError("fseek on volume");
    }
    ctx.multinum++;
    return fd;
}

static void ctx_read(DaaContext &ctx, void *data, unsigned size) {
    if (size == 0) return;
    unsigned len = (unsigned)fread(data, 1, size, ctx.fdi);
    if (len == size) return;

    if (!ctx.multi) throw DaaError("incomplete input file");

    fclose(ctx.fdi);
    ctx.fdi = ctx_next_volume(ctx);
    ctx_read(ctx, (u8*)data + len, size - len);
}

static void ctx_alloc(u8 **data, unsigned wantsize, unsigned *currsize) {
    unsigned actual_wantsize = wantsize + 16;
    if (actual_wantsize <= *currsize && *data) return;

    u8 *tmp = (u8*)realloc(*data, actual_wantsize);
    if (!tmp) throw DaaError("out of memory");

    memset(tmp + wantsize, 0, 16);
    *data = tmp;
    *currsize = actual_wantsize;
}

static bool daa_fail(DaaContext &ctx) {
    if (ctx.fdo) { fclose(ctx.fdo); ctx.fdo = nullptr; }
    if (!ctx.outputPath.empty()) fs::remove(ctx.outputPath);
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Validates the DAA file signature and version information.
 *
 * @param daa Reference to the header structure read from the file.
 * @param ctx Reference to the active DAA context.
 * @throws DaaError If the signature is unknown, a multi-volume file is selected, or the version is unsupported.
 */
static void validateDaaHeader(const daa_t &daa, DaaContext &ctx) {
    if (!strncmp((char*)daa.sign, "DAA", 16) || !strncmp((char*)daa.sign, "\xb8\xbd\xb6", 3))
        ctx.daagbi = TYPE_DAA;
    else if (!strncmp((char*)daa.sign, "GBI", 16))
        ctx.daagbi = TYPE_GBI;
    else {
        if (!strncmp((char*)daa.sign, "DAA VOL", 16) || !strncmp((char*)daa.sign, "GBI VOL", 16))
            throw DaaError("must choose the first DAA file, not a volume");
        throw DaaError("unknown DAA signature");
    }

    if ((daa.version != 0x100 && daa.version != 0x110) || daa.b1 != 1)
        throw DaaError("unsupported DAA version");
}

/**
 * @brief Configures the swapped bit type lookup array based on version parameters.
 *
 * @param ctx Reference to the active DAA context.
 * @param ver110_btype The compression bit type identifier from version 1.10 metadata.
 */
static void setupBtype(DaaContext &ctx, int ver110_btype) {
    switch (ver110_btype) {
        case 0: ctx.swapped_btype[0]=0; ctx.swapped_btype[1]=1; ctx.swapped_btype[2]=2; break;
        case 1: ctx.swapped_btype[0]=1; ctx.swapped_btype[1]=2; ctx.swapped_btype[2]=0; break;
        case 2: ctx.swapped_btype[0]=0; ctx.swapped_btype[1]=2; ctx.swapped_btype[2]=1; break;
        case 3: ctx.swapped_btype[0]=1; ctx.swapped_btype[1]=0; ctx.swapped_btype[2]=2; break;
    }
}

struct VersionLayout {
    u32 daas = 0;          // Total number of data chunks
    u32 daas_mem = 0;      // Memory size of the index table
    u32 bitsize = 0;       // Bit size configuration
    u32 bittype = 0;       // Bit type configuration
    u32 daa_dataz = 0;     // Compressed index table size if applicable
    int dolame = 0;        // Flag indicating if LAME filtering is active
    int dolzma = 0;        // Flag indicating if LZMA properties are active
    int dolamebits = 0;    // Flag indicating LZMA bit adjustments
    int lzma_filter = 0;   // LZMA filter flags
};

/**
 * @brief Sets up version-specific structures, headers, memory buffers, and allocation properties.
 *
 * @param ctx Reference to the active DAA context.
 * @param daa Reference to the header structure.
 * @param out Output layout and filter configuration populated from the header.
 * @return void* Pointer to the allocated memory buffer for index data.
 * @throws DaaError If memory allocation fails or LZMA property allocation fails.
 */
static void* setupVersionData(DaaContext &ctx, daa_t &daa, VersionLayout &out) {
    int ver110_btype = 0;

    if (daa.version == 0x100) {
        out.daas_mem = daa.data_offset - daa.size_offset;
        daa_data_t *daa_data = (daa_data_t*)malloc(out.daas_mem + 16);
        if (!daa_data) throw DaaError("out of memory (daa_data)");
        out.daas = out.daas_mem / 3;
        setupBtype(ctx, ver110_btype);
        return daa_data;
    } else {
        u32 ver110_y = daa.chunksize;
        daa.data_offset &= 0xffffff;
        daa.chunksize = (daa.chunksize & 0xfff) << 14;

        out.bittype = daa.hdata[5] & 7;
        out.bitsize = daa.hdata[5] >> 3;
        if (out.bitsize) out.bitsize += 10;
        if (!out.bitsize) {
            u32 len = daa.chunksize;
            for (out.bitsize = 0; len > (unsigned)out.bittype; out.bitsize++, len >>= 1);
        }
        out.daas_mem = daa.data_offset - daa.size_offset;

        u32 bits_per_entry = (u32)(out.bittype + out.bitsize);
        if (bits_per_entry == 0) throw DaaError("invalid header bits");
        out.daas = (u32)(((u64)out.daas_mem << 3) / bits_per_entry);

        if (ver110_y & 0x4000) {
            out.daas_mem += 0x10000;
            out.daa_dataz = *(u32*)(daa.hdata + 1);
            swap32_if_be(&out.daa_dataz, ctx.endian);
        }

        daa_data_t *daa_data = (daa_data_t*)malloc(out.daas_mem + 16);
        if (!daa_data) throw DaaError("out of memory (daa_data v110)");
        memset(daa_data, 0, out.daas_mem + 16);

        out.dolamebits = (ver110_y & 0x20000) ? 1 : 0;
        out.dolame     = (ver110_y & 0x8000000) ? 1 : 0;
        out.dolzma     = (ver110_y & 0x100000) ? 1 : 0;

        ver110_btype = (ver110_y >> 0x17) & 3;
        if (ctx.daagbi == TYPE_GBI) ver110_btype ^= 1;

        if (out.dolzma) {
            out.lzma_filter = daa.hdata[6];
            if (LzmaDec_Allocate(&ctx.lzma, daa.hdata + 7, LZMA_PROPS_SIZE, &g_Alloc) != SZ_OK)
                throw DaaError("LZMA property allocation failed");
        }

        setupBtype(ctx, ver110_btype);
        return daa_data;
    }
}

/**
 * @brief Parses and validates metadata records prior to the primary data offset.
 *
 * @param ctx Reference to the active DAA context.
 * @param daa Reference to the header structure.
 * @throws DaaError If a password-protected DAA format is encountered or file seeking fails.
 */
static void processPreDataRecords(DaaContext &ctx, const daa_t &daa) {
    while ((u64)ftell(ctx.fdi) < daa.size_offset) {
        u32 rec_type, rec_len;
        ctx_read(ctx, &rec_type, 4); swap32_if_be(&rec_type, ctx.endian);
        ctx_read(ctx, &rec_len,  4); swap32_if_be(&rec_len,  ctx.endian);
        if (rec_type == 1) ctx.multi = 1;
        else if (rec_type == 3) throw DaaError("password-protected DAA not supported");
        if (fseek(ctx.fdi, rec_len - 8, SEEK_CUR)) throw DaaError("fseek on pre-data record");
    }
}

/**
 * @brief Detects multi-part DAA files and configures corresponding file paths/naming handles.
 *
 * @param ctx Reference to the active DAA context.
 * @param inputFile Path string of the primary input file.
 * @param daa Reference to the header structure.
 */
static void setupMultiPartFiles(DaaContext &ctx, const std::string &inputFile, const daa_t &daa) {
    fseek(ctx.fdi, 0, SEEK_END);
    u64 tot = (u64)ftell(ctx.fdi);
    if (ctx.multi || (tot != daa.daasize)) {
        u8 *p;
        const char *fi = inputFile.c_str();
        if      (find_ext((u8*)fi,"001.daa")) { ctx.multi=1; ctx.multinum=2; p=(u8*)fi+strlen(fi)-7; }
        else if (find_ext((u8*)fi,"01.daa"))  { ctx.multi=2; ctx.multinum=2; p=(u8*)fi+strlen(fi)-6; }
        else {
            ctx.multi=3; ctx.multinum=0;
            p=(u8*)strrchr(fi,'.');
            if (!p) p=(u8*)fi+strlen(fi);
        }
        size_t plen = (u8*)p - (u8*)fi;
        ctx.multi_filename = (char*)malloc(plen + 32);
        memcpy(ctx.multi_filename, fi, plen);
        ctx.multi_filename[plen] = '\0';
    }
}

/**
 * @brief Reads and uncompresses the image chunk index table from the file input stream.
 *
 * @param ctx Reference to the active DAA context.
 * @param daa_data Pointer to the data buffer to populate.
 * @param daas_mem Allocated memory size for index data.
 * @param daa_dataz Compressed size of the index data, if compressed.
 * @param daas Reference to total chunks count to be updated.
 * @param bittype Bit type parameter used for calculating entry layouts.
 * @param bitsize Bit size parameter used for calculating entry layouts.
 * @throws DaaError If index table decompression fails.
 */
static void readAndDecompressIndexTable(DaaContext &ctx, void *daa_data, u32 daas_mem, u32 daa_dataz, u32 &daas, u32 bittype, u32 bitsize) {
    if (daa_dataz) {
        ctx_alloc(&ctx.in, daa_dataz, &ctx.insz);
        ctx_read(ctx, ctx.in, daa_dataz);
        size_t destLen = daas_mem;
        if (tinf_uncompress(ctx.tinf, (u8*)daa_data, &destLen, ctx.in, daa_dataz, ctx.swapped_btype) != TINF_OK)
            throw DaaError("failed to decompress index table");
        u32 bits_per_entry = (u32)(bittype + bitsize);
        daas = (u32)(((u64)destLen << 3) / bits_per_entry);
    } else {
        ctx_read(ctx, daa_data, daas_mem);
    }
}

/**
 * @brief Applies vendor-specific scrambling or scrambling filters (e.g., GBI/PowerISO Lame modifications).
 *
 * @param ctx Reference to the active DAA context.
 * @param daa Reference to the header structure.
 * @param daa_data Pointer to the index data buffer.
 * @param daas_mem Size of the index data memory buffer.
 * @param dolame Flag specifying whether PowerISO Lame transformation is requested.
 */
static void applyLameFilters(DaaContext &ctx, const daa_t &daa, void *daa_data, u32 daas_mem, int dolame) {
    if (ctx.daagbi == TYPE_GBI) gburner_lame((u8*)daa_data, daas_mem, daa.crc & 0xff);
    if (dolame) poweriso_lame((u8*)daa_data, daas_mem, daa.isosize);
}

struct ChunkDecodeConfig {
    u32 version;
    u32 chunksize;
    u32 bitsize;
    u32 bittype;
    int dolamebits;
};

/**
 * @brief Decodes the length and compression type for a single chunk index entry.
 *
 * @details Reads either the legacy v1.00 three-byte layout or advances through
 *          the v1.10 bit-packed index stream using @p bitpos. Any decoded
 *          length at or above @p cfg.chunksize marks the entry as uncompressed
 *          (@c -1).
 *
 * @param[in]     daa_data   Pointer to the decoded chunk index table.
 * @param[in]     cfg        Format version, chunk size, and bit-reader settings.
 * @param[in]     chunk_idx  Zero-based index of the chunk being decoded.
 * @param[in,out] bitpos     Current bit offset in @p daa_data for v1.10 streams;
 *                           incremented by @p cfg.bitsize + @p cfg.bittype.
 * @param[in,out] powerisuxn Running bit-reader counter (lives in DaaContext).
 *                           Must be passed by reference so increments persist
 *                           across chunks when @p cfg.dolamebits is set.
 * @param[out]    out_len    Decoded chunk payload length in bytes.
 * @param[out]    out_ztype  Resolved chunk mode (@c -1 uncompressed, @c 0 LZMA,
 *                           @c 1 Inflate).
 */
static void decodeChunkEntry(const void *daa_data, const ChunkDecodeConfig &cfg,
                             u32 chunk_idx, int &bitpos, int &powerisuxn,
                             u32 &out_len, int &out_ztype)
{
    if (cfg.version == 0x100) {
        const auto *pdata = static_cast<const daa_data_t *>(daa_data);
        out_len = (static_cast<u32>(pdata[chunk_idx].n1) << 16) |
                  pdata[chunk_idx].n2 |
                  (static_cast<u32>(pdata[chunk_idx].n3) << 8);
        out_ztype = (out_len >= cfg.chunksize) ? -1 : 1;
        return;
    }

    const auto *raw_bits = static_cast<const u8 *>(daa_data);
    out_len = daa2iso_read_bits(cfg.bitsize, raw_bits, bitpos,
                                cfg.dolamebits, 0, powerisuxn);
    bitpos += static_cast<int>(cfg.bitsize);
    out_len += LZMA_PROPS_SIZE;

    out_ztype = static_cast<int>(
        daa2iso_read_bits(cfg.bittype, raw_bits, bitpos,
                          cfg.dolamebits, 1, powerisuxn));
    bitpos += static_cast<int>(cfg.bittype);

    if (out_len >= cfg.chunksize) {
        out_ztype = -1;
    }
}

/**
 * @brief Decompresses or copies a single input chunk into @c ctx.out_buf.
 *
 * @param[in,out] ctx         Active DAA decoding context and buffers.
 * @param[in]     chunksize   Configured uncompressed chunk size in bytes.
 * @param[in]     in_len      Byte length of compressed or raw data in @c ctx.in.
 * @param[in]     ztype       Chunk mode (@c -1 raw copy, @c 0 LZMA, @c 1 Inflate).
 * @param[in]     lzma_filter Non-zero to apply @c poweriso_is_shit after LZMA decode.
 *
 * @return Number of decompressed bytes written to @c ctx.out_buf.
 *
 * @throws DaaError If @p ztype is unrecognized or decompression fails.
 */
static u32 decompressChunk(DaaContext &ctx, u32 chunksize, u32 in_len,
                           int ztype, int lzma_filter)
{
    if (ztype == -1) {
        const u32 outlen = (in_len < chunksize) ? in_len : chunksize;
        memcpy(ctx.out_buf, ctx.in, outlen);
        return outlen;
    }

    if (ztype == 0) {
        LzmaDec_Init(&ctx.lzma);
        const u32 outlen = lzma_decode_full(
            &ctx.lzma, ctx.in, in_len, ctx.out_buf, ctx.outsz);
        if (outlen == 0) {
            throw DaaError("LZMA decompression failed");
        }
        if (lzma_filter) {
            poweriso_is_shit(ctx.out_buf, static_cast<int>(outlen));
        }
        return outlen;
    }

    if (ztype == 1) {
        size_t destLen = ctx.outsz;
        if (tinf_uncompress(ctx.tinf, ctx.out_buf, &destLen, ctx.in, in_len,
                            ctx.swapped_btype) != TINF_OK) {
            throw DaaError("INFLATE decompression failed");
        }
        return static_cast<u32>(destLen);
    }

    throw DaaError("unknown compression type");
}

/**
 * @brief Validates chunk output size, writes it to the output stream, and updates totals.
 *
 * @details Clamps trailing bytes on the final chunk so total output does not
 *          exceed @p isosize, and requires exact @p chunksize output on all
 *          earlier chunks.
 *
 * @param[in,out] ctx         Active DAA context with output stream and progress counter.
 * @param[in]     outlen      Decompressed byte count available in @c ctx.out_buf.
 * @param[in]     is_last     @c true if processing the final chunk in the archive.
 * @param[in]     chunksize   Expected uncompressed chunk size for non-final chunks.
 * @param[in]     isosize     Expected total uncompressed ISO size in bytes.
 * @param[in,out] total_out   Running total of bytes written so far; incremented by
 *                            the actual byte count written.
 *
 * @throws DaaError If a non-final chunk does not match @p chunksize or writing fails.
 */
static void writeValidatedChunk(DaaContext &ctx, u32 outlen, bool is_last,
                               u32 chunksize, u64 isosize, u64 &total_out)
{
    if (is_last) {
        if ((total_out + outlen) > isosize) {
            outlen = static_cast<u32>(isosize - total_out);
        }
    } else if (outlen != chunksize) {
        throw DaaError("chunk size mismatch");
    }

    if (fwrite(ctx.out_buf, 1, outlen, ctx.fdo) != outlen) {
        throw DaaError("write failed");
    }

    total_out += outlen;
    if (ctx.completedBytes) {
        ctx.completedBytes->fetch_add(outlen, std::memory_order_relaxed);
    }
}

/**
 * @brief Iterates over all disk chunks, performs decompression (LZMA, Inflate, or
 *        uncompressed), and writes the resulting data to the output stream.
 *
 * @details Processes each entry in the chunk index table using a
 *          @c ChunkDecodeConfig built from @p daa and the v1.10 bit parameters.
 *          Each chunk is decompressed according to its type and written to
 *          @p ctx.fdo. Progress is reported through @p ctx.completedBytes when
 *          non-null.
 *
 *          Cancellation is signalled by returning @c false rather than throwing,
 *          so that the caller can perform a silent bail-out without emitting an
 *          error banner to stderr. The index buffer @p daa_data is freed before
 *          returning @c false on cancellation; ownership is therefore transferred
 *          to this function on that path.
 *
 * @param ctx          Reference to the active DAA context (holds file handles,
 *                     output buffer, LZMA decoder, progress counter, etc.).
 * @param daa          Reference to the parsed DAA header structure.
 * @param daa_data     Pointer to the decoded chunk index table. Freed internally
 *                     if the operation is cancelled.
 * @param daas         Total number of chunks to process.
 * @param bitsize      Bit-width of the length field in the v1.10 index layout.
 * @param bittype      Bit-width of the compression-type field in the v1.10
 *                     index layout.
 * @param dolamebits   Flag selecting the alternate bit-extraction offset used by
 *                     certain PowerISO-produced archives.
 * @param lzma_filter  Flag indicating that the vendor-specific post-processing
 *                     filter (@c poweriso_is_shit) must be applied to
 *                     LZMA-decompressed output.
 *
 * @return @c true if every chunk was processed and the total output size matched
 *         @c daa.isosize.
 * @return @c false if the global cancellation flag was observed before the loop
 *         completed; @p daa_data has been freed and no error message was emitted.
 *
 * @throws DaaError If a chunk length exceeds the hard limit, an unknown
 *         compression type is encountered, decompression fails, a non-final
 *         chunk does not match the expected chunk size, a write fails, or the
 *         final output size does not match @c daa.isosize.
 */
static bool processAllChunks(DaaContext &ctx, const daa_t &daa, void *daa_data,
                             u32 daas, u32 bitsize, u32 bittype,
                             int dolamebits, int lzma_filter)
{
    ctx_alloc(&ctx.out_buf, daa.chunksize, &ctx.outsz);

    const ChunkDecodeConfig decode_cfg {
        daa.version,
        daa.chunksize,
        bitsize,
        bittype,
        dolamebits,
    };

    u64 tot = 0;
    const u32 last_chunk = daas - 1;
    int bitpos = 0;

    for (u32 i = 0; i < daas; i++) {
        if (GlobalState::g_operationCancelled.load()) {
            free(daa_data);
            return false;  // silent cancel, no exception
        }

        u32 len = 0;
        int ztype = 1;
        decodeChunkEntry(daa_data, decode_cfg, i, bitpos, ctx.powerisuxn, len, ztype);

        if (len > 0x1000000) {
            throw DaaError("excessive chunk length");
        }

        ctx_alloc(&ctx.in, len, &ctx.insz);
        ctx_read(ctx, ctx.in, len);

        const u32 outlen = decompressChunk(ctx, daa.chunksize, len, ztype, lzma_filter);
        writeValidatedChunk(ctx, outlen, i == last_chunk, daa.chunksize,
                            daa.isosize, tot);
    }

    if (tot != daa.isosize) {
        throw DaaError("output size mismatch");
    }
    return true;
}

/**
 * @brief Converts a DAA/GBI archive image file into a raw ISO image.
 *
 * @details Opens @p inputFile for reading and @p outputFile for writing,
 *          validates the DAA/GBI header and version, decodes the chunk index
 *          table (decompressing it if the v1.10 header indicates a compressed
 *          index), applies any vendor-specific index scrambling filters, and
 *          then decompresses every chunk to produce the reconstructed ISO image.
 *
 *          Cancellation is handled silently: if the global cancellation flag is
 *          observed, the function returns @c false via @c daa_fail without
 *          writing an error banner to stderr, preserving the terminal state of
 *          any progress renderer driven by @p completedBytes.
 *
 * @param inputFile       Path to the source DAA (or GBI) file. Multi-part
 *                        archives must be opened via their first volume.
 * @param outputFile      Path to the destination ISO file. Truncated if it
 *                        already exists.
 * @param completedBytes  Optional pointer to an atomic counter that is
 *                        incremented by the number of output bytes produced
 *                        after each chunk. May be @c nullptr if progress
 *                        reporting is not required.
 *
 * @return @c true if the conversion completed successfully and the output size
 *         matched @c daa.isosize.
 * @return @c false if the input could not be opened, the output could not be
 *         created, the operation was cancelled, or a recoverable error was
 *         reported through the @c DaaError handler.
 *
 * @throws Nothing. All @c DaaError exceptions are caught internally and
 *         handled via @c daa_fail before returning @c false.
 */
bool convertDaaToIso(const std::string &inputFile,
                     const std::string &outputFile,
                     std::atomic<size_t> *completedBytes)
{
    if (GlobalState::g_operationCancelled.load()) return false;

    DaaContext ctx;
    ctx.outputPath     = outputFile;
    ctx.completedBytes = completedBytes;

    ctx.powerisuxn = 0;
    tinf_init(ctx.tinf);

    { int e = 1; ctx.endian = (*(char*)&e) ? 0 : 1; }

    ctx.fdi = fopen(inputFile.c_str(), "rb");
    if (!ctx.fdi) return false;

    ctx.fdo = fopen(outputFile.c_str(), "wb");
    if (!ctx.fdo) return daa_fail(ctx);

    try {
        daa_t daa;
        ctx_read(ctx, &daa, sizeof(daa));
        swap_daa_if_be(&daa, ctx.endian);

        validateDaaHeader(daa, ctx);

        LzmaDec_Construct(&ctx.lzma);

        VersionLayout layout;
        void *daa_data = setupVersionData(ctx, daa, layout);

        processPreDataRecords(ctx, daa);
        setupMultiPartFiles(ctx, inputFile, daa);

        if (fseek(ctx.fdi, daa.size_offset, SEEK_SET)) throw DaaError("fseek to size_offset");

        readAndDecompressIndexTable(ctx, daa_data, layout.daas_mem, layout.daa_dataz,
                                    layout.daas, layout.bittype, layout.bitsize);
        applyLameFilters(ctx, daa, daa_data, layout.daas_mem, layout.dolame);

        if (fseek(ctx.fdi, daa.data_offset, SEEK_SET)) throw DaaError("fseek to data_offset");

        // Cancellation is signalled by processAllChunks returning false (with
        // daa_data already freed). Mirror the original silent bail-out by
        // skipping the error banner and returning daa_fail directly.
        if (!processAllChunks(ctx, daa, daa_data, layout.daas, layout.bitsize,
                              layout.bittype, layout.dolamebits, layout.lzma_filter)) {
            return daa_fail(ctx);
        }

        free(daa_data);
        fclose(ctx.fdo); ctx.fdo = nullptr;
        fclose(ctx.fdi); ctx.fdi = nullptr;
        return true;

    } catch (const DaaError &e) {
        //fprintf(stderr, "\nError: %s\n", e.msg);
        return daa_fail(ctx);
    }
}
