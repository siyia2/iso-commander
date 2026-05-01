// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * daa2iso.cpp  –  Convert PowerISO DAA / gBurner GBI disk images to ISO
 *
 * Original reverse-engineering and algorithms by Luigi Auriemma (aluigi.org)
 * C++ Linux port + library API (no Windows deps, no encryption/password) – 2026
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

// C++ Standard Library Headers
#include <filesystem>

// Project Headers
#include "../daa2iso.h"
#include "../state.h"

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
    if (p->probs) alloc->Free(alloc, p->probs);
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

static u32 lzma_decode_full(CLzmaDec *p, const u8 *in, u32 insz, u8 *out, u32 outsz) {
    p->dic        = out;
    p->dicBufSize = outsz;
    p->dicPos     = 0;
    p->buf  = in;
    p->code = 0;
    p->range = 0xFFFFFFFF;
    for (int i = 0; i < 5; i++) p->code = (p->code << 8) | (*p->buf++);
    (void)insz;

    unsigned state  = 0;
    u32 rep0 = 1, rep1 = 1, rep2 = 1, rep3 = 1;
    int len = 0;
    CLzmaProb *probs = p->probs;

    for (;;) {
        u32 posState = (u32)(p->dicPos) & ((1 << p->pb) - 1);
        CLzmaProb *prob = probs + IsMatch + (state << kNumPosBitsMax) + posState;
        u32 ttt = *prob;
        u32 bound = (p->range >> kNumBitModelTotalBits) * ttt;

        if (p->code < bound) {
            p->range = bound;
            ttt += (kBitModelTotal - ttt) >> kNumMoveBits;
            *prob = (CLzmaProb)ttt;
            NORMALIZE;

            prob = probs + Literal;
            if (p->processedPos != 0 || p->checkDicSize != 0) {
                u8 prevByte = (p->dicPos == 0) ? 0 : p->dic[p->dicPos - 1];
                prob += (LZMA_LIT_SIZE * (((p->processedPos & ((1 << p->lp) - 1)) << p->lc)
                        + (prevByte >> (8 - p->lc))));
            }

            u32 symbol = 1;
            if (state >= 7) {
                u32 dicPos2 = (p->dicPos >= rep0) ? (p->dicPos - rep0) : (p->dicPos + p->dicBufSize - rep0);
                u8 matchByte = p->dic[dicPos2];
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
            }
            while (symbol < 0x100) { GET_BIT(prob + symbol, symbol); }

            if (p->dicPos >= p->dicBufSize) return 0;
            p->dic[p->dicPos++] = (u8)symbol;
            p->processedPos++;
            state = (state < 4) ? 0 : (state < 10) ? state - 3 : state - 6;
            continue;
        }

        p->range -= bound; p->code -= bound;
        ttt -= ttt >> kNumMoveBits; *prob = (CLzmaProb)ttt;
        NORMALIZE;

        prob = probs + IsRep + state;
        ttt = *prob;
        bound = (p->range >> kNumBitModelTotalBits) * ttt;
        if (p->code < bound) {
            p->range = bound;
            ttt += (kBitModelTotal - ttt) >> kNumMoveBits; *prob = (CLzmaProb)ttt;
            NORMALIZE;

            rep3 = rep2; rep2 = rep1; rep1 = rep0;

            prob = probs + LenCoder;
            {
                CLzmaProb *pc = prob;
                u32 sym;
                ttt = *pc; bound = (p->range >> kNumBitModelTotalBits) * ttt;
                if (p->code < bound) {
                    p->range = bound; ttt += (kBitModelTotal-ttt)>>kNumMoveBits; *pc=(CLzmaProb)ttt; NORMALIZE;
                    pc = prob + 1 + (posState << kLenNumLowBits);
                    TREE_DECODE(pc, kLenNumLowSymbols, sym);
                    len = sym;
                } else {
                    p->range -= bound; p->code -= bound; ttt -= ttt>>kNumMoveBits; *pc=(CLzmaProb)ttt; NORMALIZE;
                    pc = prob + 1 + (kNumPosStatesMax << kLenNumLowBits);
                    ttt = *pc; bound = (p->range>>kNumBitModelTotalBits)*ttt;
                    if (p->code < bound) {
                        p->range = bound; ttt+=(kBitModelTotal-ttt)>>kNumMoveBits; *pc=(CLzmaProb)ttt; NORMALIZE;
                        pc = prob + 1 + (kNumPosStatesMax << kLenNumLowBits) + 1 + (posState << kLenNumMidBits);
                        TREE_DECODE(pc, kLenNumMidSymbols, sym);
                        len = kLenNumLowSymbols + sym;
                    } else {
                        p->range -= bound; p->code -= bound; ttt-=ttt>>kNumMoveBits; *pc=(CLzmaProb)ttt; NORMALIZE;
                        pc = prob + 1 + (kNumPosStatesMax<<kLenNumLowBits) + 1 + (kNumPosStatesMax<<kLenNumMidBits);
                        TREE_DECODE(pc, kLenNumHighSymbols, sym);
                        len = kLenNumLowSymbols + kLenNumMidSymbols + sym;
                    }
                }
            }

            state = (state < 7) ? 7 : 10;

            {
                u32 posSlot;
                int numDirectBits;
                u32 distance;
                unsigned lenState = (unsigned)(len < kNumLenToBitModelTotal-1 ? len : kNumLenToBitModelTotal-1);
                prob = probs + PosSlot + (lenState << 6);
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
                            u32 bd = (p->range>>kNumBitModelTotalBits)*tt2;
                            if (p->code < bd) {
                                p->range = bd; tt2+=(kBitModelTotal-tt2)>>kNumMoveBits; *(prob+(distance>>1))=(CLzmaProb)tt2; NORMALIZE;
                                distance |= 0;
                            } else {
                                p->range -= bd; p->code -= bd; tt2-=tt2>>kNumMoveBits; *(prob+(distance>>1))=(CLzmaProb)tt2; NORMALIZE;
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
                            if (p->code >= p->range) { p->code -= p->range; directBitsResult |= (1 << i); }
                            NORMALIZE;
                        }
                        distance = ((distance << numDirectBits) | directBitsResult) << 4;
                        prob = probs + Align;
                        u32 sym2 = 1;
                        for (int i = 0; i < 4; i++) { TREE_GET_BIT(prob, sym2); }
                        distance |= sym2 & 0xF;
                    }
                }
                rep0 = distance + 1;
                if (rep0 == 0) return (u32)p->dicPos;
            }
        } else {
            p->range -= bound; p->code -= bound;
            ttt -= ttt>>kNumMoveBits; *prob=(CLzmaProb)ttt;
            NORMALIZE;

            prob = probs + IsRepG0 + state;
            ttt = *prob; bound = (p->range>>kNumBitModelTotalBits)*ttt;
            if (p->code < bound) {
                p->range = bound; ttt+=(kBitModelTotal-ttt)>>kNumMoveBits; *prob=(CLzmaProb)ttt; NORMALIZE;
                prob = probs + IsRep0Long + (state << kNumPosBitsMax) + posState;
                ttt = *prob; bound = (p->range>>kNumBitModelTotalBits)*ttt;
                if (p->code < bound) {
                    p->range = bound; ttt+=(kBitModelTotal-ttt)>>kNumMoveBits; *prob=(CLzmaProb)ttt; NORMALIZE;
                    if (p->dicPos == 0 && p->checkDicSize == 0) return 0;
                    u32 srcPos = (p->dicPos >= rep0) ? (p->dicPos - rep0) : (p->dicPos + p->dicBufSize - rep0);
                    if (p->dicPos >= p->dicBufSize) return 0;
                    p->dic[p->dicPos++] = p->dic[srcPos];
                    p->processedPos++;
                    state = (state < 7) ? 9 : 11;
                    continue;
                } else {
                    p->range -= bound; p->code -= bound; ttt-=ttt>>kNumMoveBits; *prob=(CLzmaProb)ttt; NORMALIZE;
                }
            } else {
                u32 distance;
                p->range -= bound; p->code -= bound; ttt-=ttt>>kNumMoveBits; *prob=(CLzmaProb)ttt; NORMALIZE;
                prob = probs + IsRepG1 + state;
                ttt = *prob; bound = (p->range>>kNumBitModelTotalBits)*ttt;
                if (p->code < bound) {
                    p->range = bound; ttt+=(kBitModelTotal-ttt)>>kNumMoveBits; *prob=(CLzmaProb)ttt; NORMALIZE;
                    distance = rep1;
                } else {
                    p->range -= bound; p->code -= bound; ttt-=ttt>>kNumMoveBits; *prob=(CLzmaProb)ttt; NORMALIZE;
                    prob = probs + IsRepG2 + state;
                    ttt = *prob; bound = (p->range>>kNumBitModelTotalBits)*ttt;
                    if (p->code < bound) {
                        p->range = bound; ttt+=(kBitModelTotal-ttt)>>kNumMoveBits; *prob=(CLzmaProb)ttt; NORMALIZE;
                        distance = rep2;
                    } else {
                        p->range -= bound; p->code -= bound; ttt-=ttt>>kNumMoveBits; *prob=(CLzmaProb)ttt; NORMALIZE;
                        distance = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = distance;
            }

            prob = probs + RepLenCoder;
            {
                CLzmaProb *pc = prob;
                u32 sym;
                ttt = *pc; bound = (p->range>>kNumBitModelTotalBits)*ttt;
                if (p->code < bound) {
                    p->range = bound; ttt+=(kBitModelTotal-ttt)>>kNumMoveBits; *pc=(CLzmaProb)ttt; NORMALIZE;
                    pc = prob + 1 + (posState << kLenNumLowBits);
                    TREE_DECODE(pc, kLenNumLowSymbols, sym);
                    len = sym;
                } else {
                    p->range -= bound; p->code -= bound; ttt-=ttt>>kNumMoveBits; *pc=(CLzmaProb)ttt; NORMALIZE;
                    pc = prob + 1 + (kNumPosStatesMax << kLenNumLowBits);
                    ttt = *pc; bound = (p->range>>kNumBitModelTotalBits)*ttt;
                    if (p->code < bound) {
                        p->range = bound; ttt+=(kBitModelTotal-ttt)>>kNumMoveBits; *pc=(CLzmaProb)ttt; NORMALIZE;
                        pc = prob + 1 + (kNumPosStatesMax<<kLenNumLowBits) + 1 + (posState<<kLenNumMidBits);
                        TREE_DECODE(pc, kLenNumMidSymbols, sym);
                        len = kLenNumLowSymbols + sym;
                    } else {
                        p->range -= bound; p->code -= bound; ttt-=ttt>>kNumMoveBits; *pc=(CLzmaProb)ttt; NORMALIZE;
                        pc = prob + 1 + (kNumPosStatesMax<<kLenNumLowBits) + 1 + (kNumPosStatesMax<<kLenNumMidBits);
                        TREE_DECODE(pc, kLenNumHighSymbols, sym);
                        len = kLenNumLowSymbols + kLenNumMidSymbols + sym;
                    }
                }
            }
            state = (state < 7) ? 8 : 11;
        }

        len += kMatchMinLen;

        if (rep0 > p->dicPos + p->checkDicSize) return 0;
        while (len--) {
            if (p->dicPos >= p->dicBufSize) return 0;
            u32 srcPos = (p->dicPos >= rep0) ? (p->dicPos - rep0)
                                              : (p->dicPos + p->dicBufSize - rep0);
            p->dic[p->dicPos++] = p->dic[srcPos];
            p->processedPos++;
        }
    }
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
    unsigned int  *destLen;
    unsigned int   sourceLen;
    unsigned int   sourceSize;
    unsigned int   destSize;
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
                            void *dest, unsigned *destLen,
                            const void *source, unsigned sourceLen,
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
        if (fdi)            fclose(fdi);
        if (fdo)            fclose(fdo);
        if (in)             free(in);
        if (out_buf)        free(out_buf);
        if (multi_filename) free(multi_filename);
        if (lzma.probs)     LzmaDec_Free(&lzma, &g_Alloc);
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
    sprintf(toadd, fmts[ctx.multi - 1], ctx.multinum);

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

bool convertDaaToIso(const std::string &inputFile,
                     const std::string &outputFile,
                     std::atomic<size_t> *completedBytes)
{
    if (GlobalState::g_operationCancelled.load()) return false;

    DaaContext ctx;
    ctx.outputPath     = outputFile;
    ctx.completedBytes = completedBytes;

    // Initialise per-instance state (replaces reset_bit_reader / tinf_init globals)
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

        if (!strncmp((char*)daa.sign,"DAA",16) || !strncmp((char*)daa.sign,"\xb8\xbd\xb6",3))
            ctx.daagbi = TYPE_DAA;
        else if (!strncmp((char*)daa.sign,"GBI",16))
            ctx.daagbi = TYPE_GBI;
        else {
            if (!strncmp((char*)daa.sign,"DAA VOL",16) || !strncmp((char*)daa.sign,"GBI VOL",16))
                throw DaaError("must choose the first DAA file, not a volume");
            throw DaaError("unknown DAA signature");
        }

        if ((daa.version != 0x100 && daa.version != 0x110) || daa.b1 != 1)
            throw DaaError("unsupported DAA version");

        LzmaDec_Construct(&ctx.lzma);

        daa_data_t *daa_data = nullptr;
        u32 daas = 0, daas_mem = 0, daa_dataz = 0;
        int ztype=1, bitpos=0, bitsize=0, bittype=0;
        int dolame=0, dolzma=0, dolamebits=0, lzma_filter=0, ver110_btype=0;
        u32 ver110_y = 0;

        if (daa.version == 0x100) {
            daas_mem = daa.data_offset - daa.size_offset;
            daa_data = (daa_data_t*)malloc(daas_mem + 16);
            if (!daa_data) throw DaaError("out of memory (daa_data)");
            daas = daas_mem / 3;
        } else {
            ver110_y = daa.chunksize;
            daa.data_offset &= 0xffffff;
            daa.chunksize    = (daa.chunksize & 0xfff) << 14;

            bittype = daa.hdata[5] & 7;
            bitsize = daa.hdata[5] >> 3;
            if (bitsize) bitsize += 10;
            if (!bitsize) {
                u32 len = daa.chunksize;
                for (bitsize=0; len>(unsigned)bittype; bitsize++, len>>=1);
            }
            daas_mem = daa.data_offset - daa.size_offset;

            u32 bits_per_entry = (u32)(bittype + bitsize);
            if (bits_per_entry == 0) throw DaaError("invalid header bits");
            daas = (u32)(((u64)daas_mem << 3) / bits_per_entry);

            if (ver110_y & 0x4000) {
                daas_mem += 0x10000;
                daa_dataz = *(u32*)(daa.hdata + 1);
                swap32_if_be(&daa_dataz, ctx.endian);
            }

            daa_data = (daa_data_t*)malloc(daas_mem + 16);
            if (!daa_data) throw DaaError("out of memory (daa_data v110)");
            memset(daa_data, 0, daas_mem + 16);

            dolamebits   = (ver110_y & 0x20000)   ? 1 : 0;
            dolame       = (ver110_y & 0x8000000)  ? 1 : 0;
            dolzma       = (ver110_y & 0x100000)   ? 1 : 0;

            ver110_btype = (ver110_y >> 0x17) & 3;
            if (ctx.daagbi == TYPE_GBI) ver110_btype ^= 1;

            if (dolzma) {
                lzma_filter = daa.hdata[6];
                if (LzmaDec_Allocate(&ctx.lzma, daa.hdata + 7, LZMA_PROPS_SIZE, &g_Alloc) != SZ_OK)
                    throw DaaError("LZMA property allocation failed");
            }
        }

        switch (ver110_btype) {
            case 0: ctx.swapped_btype[0]=0; ctx.swapped_btype[1]=1; ctx.swapped_btype[2]=2; break;
            case 1: ctx.swapped_btype[0]=1; ctx.swapped_btype[1]=2; ctx.swapped_btype[2]=0; break;
            case 2: ctx.swapped_btype[0]=0; ctx.swapped_btype[1]=2; ctx.swapped_btype[2]=1; break;
            case 3: ctx.swapped_btype[0]=1; ctx.swapped_btype[1]=0; ctx.swapped_btype[2]=2; break;
        }

        while ((u64)ftell(ctx.fdi) < daa.size_offset) {
            u32 rec_type, rec_len;
            ctx_read(ctx, &rec_type, 4); swap32_if_be(&rec_type, ctx.endian);
            ctx_read(ctx, &rec_len,  4); swap32_if_be(&rec_len,  ctx.endian);
            if (rec_type == 1) ctx.multi = 1;
            else if (rec_type == 3) throw DaaError("password-protected DAA not supported");
            if (fseek(ctx.fdi, rec_len - 8, SEEK_CUR)) throw DaaError("fseek on pre-data record");
        }

        {
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
                ctx.multi_filename = (char*)malloc(plen + 16);
                memcpy(ctx.multi_filename, fi, plen);
                ctx.multi_filename[plen] = '\0';
            }
        }

        if (fseek(ctx.fdi, daa.size_offset, SEEK_SET)) throw DaaError("fseek to size_offset");

        if (daa_dataz) {
            ctx_alloc(&ctx.in, daa_dataz, &ctx.insz);
            ctx_read(ctx, ctx.in, daa_dataz);
            unsigned destLen = daas_mem;
            if (tinf_uncompress(ctx.tinf, (u8*)daa_data, &destLen, ctx.in, daa_dataz, ctx.swapped_btype) != TINF_OK)
                throw DaaError("failed to decompress index table");
            u32 bits_per_entry = (u32)(bittype + bitsize);
            daas = (u32)(((u64)destLen << 3) / bits_per_entry);
        } else {
            ctx_read(ctx, daa_data, daas_mem);
        }

        if (ctx.daagbi == TYPE_GBI) gburner_lame((u8*)daa_data, daas_mem, daa.crc & 0xff);
        if (dolame)                  poweriso_lame((u8*)daa_data, daas_mem, daa.isosize);

        if (fseek(ctx.fdi, daa.data_offset, SEEK_SET)) throw DaaError("fseek to data_offset");

        ctx_alloc(&ctx.out_buf, daa.chunksize, &ctx.outsz);
        u64  tot        = 0;
        u32  last_chunk = daas - 1;

        for (u32 i = 0; i < daas; i++) {
            if (GlobalState::g_operationCancelled.load()) {
                free(daa_data);
                return daa_fail(ctx);
            }

            u32 len;
            if (daa.version == 0x100) {
                len   = ((u32)daa_data[i].n1<<16) | daa_data[i].n2 | ((u32)daa_data[i].n3<<8);
                ztype = (len >= daa.chunksize) ? -1 : 1;
            } else {
                len   = daa2iso_read_bits(bitsize,(u8*)daa_data,bitpos,dolamebits,0,ctx.powerisuxn);
                bitpos += bitsize;
                len  += LZMA_PROPS_SIZE;
                ztype = (int)daa2iso_read_bits(bittype,(u8*)daa_data,bitpos,dolamebits,1,ctx.powerisuxn);
                bitpos += bittype;
                if (len >= daa.chunksize) ztype = -1;
            }

            if (len > 0x1000000) throw DaaError("excessive chunk length");

            ctx_alloc(&ctx.in, len, &ctx.insz);
            ctx_read(ctx, ctx.in, len);

            u32 outlen;
            switch (ztype) {
                case -1:
                    outlen = (len < daa.chunksize) ? len : daa.chunksize;
                    memcpy(ctx.out_buf, ctx.in, outlen);
                    break;
                case 0:
                    LzmaDec_Init(&ctx.lzma);
                    outlen = lzma_decode_full(&ctx.lzma, ctx.in, len, ctx.out_buf, ctx.outsz);
                    if (outlen == 0) throw DaaError("LZMA decompression failed");
                    if (lzma_filter) poweriso_is_shit(ctx.out_buf, (int)outlen);
                    break;
                case 1: {
                    unsigned destLen = ctx.outsz;
                    if (tinf_uncompress(ctx.tinf, ctx.out_buf, &destLen, ctx.in, len, ctx.swapped_btype) != TINF_OK)
                        throw DaaError("INFLATE decompression failed");
                    outlen = destLen;
                    break;
                }
                default:
                    throw DaaError("unknown compression type");
            }

            if (i == last_chunk) {
                if ((tot + outlen) > daa.isosize)
                    outlen = (u32)(daa.isosize - tot);
            } else {
                if (outlen != daa.chunksize)
                    throw DaaError("chunk size mismatch");
            }

            if (fwrite(ctx.out_buf, 1, outlen, ctx.fdo) != outlen)
                throw DaaError("write failed");

            tot += outlen;
            if (completedBytes)
                completedBytes->fetch_add(outlen, std::memory_order_relaxed);
        }

        if (tot != daa.isosize) throw DaaError("output size mismatch");

        free(daa_data);
        fclose(ctx.fdo); ctx.fdo = nullptr;
        fclose(ctx.fdi); ctx.fdi = nullptr;
        return true;

    } catch (const DaaError &e) {
        fprintf(stderr, "\nError: %s\n", e.msg);
        return daa_fail(ctx);
    }
}
