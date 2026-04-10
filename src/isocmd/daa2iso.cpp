/*
 * daa2iso.cpp  –  Convert PowerISO DAA / gBurner GBI disk images to ISO
 *
 * Original reverse-engineering and algorithms by Luigi Auriemma (aluigi.org)
 * C++ Linux port + library API (no Windows deps, no encryption/password) – 2026
 *
 * License: GPL-2.0 (same as the original work)
 *
 * Build as command-line tool:
 *   g++ -O2 -std=c++17 -o daa2iso daa2iso.cpp
 *
 * Use as library: include this file or link against it.
 *   bool convertDaaToIso(const std::string& inputFile,
 *                        const std::string& outputFile,
 *                        std::atomic<size_t>* completedBytes);
 */

// ─── large-file support ────────────────────────────────────────────────────
#define _LARGE_FILES
#define __USE_LARGEFILE64
#define __USE_FILE_OFFSET64
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cerrno>
#include <atomic>
#include <string>
#include <filesystem>

// Use 64-bit file I/O on Linux
#define off_t   off64_t
#define fopen   fopen64
#define fseek   fseeko64
#define ftell   ftello64

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════
//  Type aliases
// ═══════════════════════════════════════════════════════════════════════════
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// ═══════════════════════════════════════════════════════════════════════════
//  LZMA decoder (Igor Pavlov / 7-zip SDK, minimal subset)
// ═══════════════════════════════════════════════════════════════════════════

#define LZMA_PROPS_SIZE 5
#define SZ_OK           0
#define SZ_ERROR_DATA   1

typedef size_t SizeT;
typedef int    SRes;

typedef enum { LZMA_FINISH_ANY, LZMA_FINISH_END } ELzmaFinishMode;
typedef enum { LZMA_STATUS_NOT_SPECIFIED, LZMA_STATUS_FINISHED_WITH_MARK,
               LZMA_STATUS_NOT_FINISHED, LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK } ELzmaStatus;

typedef void *(*ISzAlloc_Alloc)(void *, size_t);
typedef void  (*ISzAlloc_Free) (void *, void *);
struct ISzAlloc { ISzAlloc_Alloc Alloc; ISzAlloc_Free Free; };
static void *SzAlloc(void *, size_t sz) { return malloc(sz);  }
static void  SzFree (void *, void *p)   { free(p);            }
static ISzAlloc g_Alloc = { SzAlloc, SzFree };

// Bit-reader helper
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

#define LZMA_DIC_MIN (1 << 12)

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

#define NORMALIZE_CHECK if (p->range < (1 << 24)) { if (p->buf == bufLimit) return SZ_ERROR_DATA; p->range <<= 8; p->code = (p->code << 8) | (*p->buf++); }
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

#define kNumStates      12
#define kNumLenToBitModelTotal   10
#define kMatchMinLen    2
#define kMatchSpecLenStart 274

static u32 lzma_decode_full(CLzmaDec *p, const u8 *in, u32 insz, u8 *out, u32 outsz) {
    p->dic        = out;
    p->dicBufSize = outsz;
    p->dicPos     = 0;
    p->buf  = in;
    p->code = 0;
    p->range = 0xFFFFFFFF;
    for (int i = 0; i < 5; i++) p->code = (p->code << 8) | (*p->buf++);
    const u8 *bufEnd = in + insz;

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
    (void)bufEnd;
}

// ═══════════════════════════════════════════════════════════════════════════
//  tinflate – Luigi Auriemma's modified tiny inflate (btype table swapped)
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

static TINF_TREE sltree, sdtree;
static u8        length_bits[30];
static u16       length_base[30];
static u8        dist_bits[30];
static u16       dist_base[30];

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
        case 16: { u8 prev = lengths[num-1]; length = tinf_read_bits(d,2,3);
                   if ((num+length)>(288+32)) return TINF_DATA_ERROR;
                   for (; length; --length) lengths[num++] = prev; break; }
        case 17: { length = tinf_read_bits(d,3,3);
                   if ((num+length)>(288+32)) return TINF_DATA_ERROR;
                   for (; length; --length) lengths[num++] = 0; break; }
        case 18: { length = tinf_read_bits(d,7,11);
                   if ((num+length)>(288+32)) return TINF_DATA_ERROR;
                   for (; length; --length) lengths[num++] = 0; break; }
        default: if ((num+1)>(288+32)) return TINF_DATA_ERROR;
                 lengths[num++] = sym; break;
        }
    }
    tinf_build_tree(lt, lengths, hlit);
    tinf_build_tree(dt, lengths+hlit, hdist);
    return TINF_OK;
}

static int tinf_inflate_block_data(TINF_DATA *d, TINF_TREE *lt, TINF_TREE *dt) {
    while (1) {
        int sym = tinf_decode_symbol(d, lt);
        if (sym == 256) break;
        if (sym < 256) {
            if ((*d->destLen+1) > d->destSize) return TINF_DATA_ERROR;
            *d->dest++ = sym; *d->destLen += 1;
        } else {
            sym -= 257;
            int length = tinf_read_bits(d, length_bits[sym], length_base[sym]);
            int dist   = tinf_decode_symbol(d, dt);
            int offs   = tinf_read_bits(d, dist_bits[dist], dist_base[dist]);
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

static void tinf_init() {
    tinf_build_fixed_trees(&sltree, &sdtree);
    tinf_build_bits_base(length_bits, length_base, 4, 3);
    tinf_build_bits_base(dist_bits,   dist_base,   2, 1);
    length_bits[28] = 0; length_base[28] = 258;
}

static int tinf_uncompress(void *dest, unsigned *destLen,
                            const void *source, unsigned sourceLen,
                            const unsigned swapped_btype[3]) {
    TINF_DATA d;
    d.source    = (const u8 *)source;
    d.bitcount  = 0;
    d.dest      = (u8 *)dest;
    d.destLen   = destLen;
    d.sourceLen = 0;
    d.sourceSize = sourceLen;
    d.destSize   = *destLen;
    *destLen = 0;
    int bfinal;
    do {
        bfinal = tinf_getbit(&d);
        unsigned btype = tinf_read_bits(&d, 2, 0);
        int res;
        if      (btype == swapped_btype[0]) res = tinf_inflate_uncompressed_block(&d);
        else if (btype == swapped_btype[1]) res = tinf_inflate_block_data(&d, &sltree, &sdtree);
        else if (btype == swapped_btype[2]) {
            if (tinf_decode_trees(&d, &d.ltree, &d.dtree) != TINF_OK) return TINF_DATA_ERROR;
            res = tinf_inflate_block_data(&d, &d.ltree, &d.dtree);
        } else return TINF_DATA_ERROR;
        if (res != TINF_OK) return TINF_DATA_ERROR;
    } while (!bfinal);
    return TINF_OK;
}

// ═══════════════════════════════════════════════════════════════════════════
//  DAA structures
// ═══════════════════════════════════════════════════════════════════════════

#pragma pack(4)
struct daa_t {
    u8  sign[16];
    u32 size_offset;
    u32 version;
    u32 data_offset;
    u32 b1;
    u32 b0;
    u32 chunksize;
    u64 isosize;
    u64 daasize;
    u8  hdata[16];
    u32 crc;
};
#pragma pack(1)
struct daa_data_t { u8 n1, n2, n3; };
#pragma pack()

enum { TYPE_DAA, TYPE_GBI, TYPE_NONE };

// ═══════════════════════════════════════════════════════════════════════════
//  Endian helpers (parameterised)
// ═══════════════════════════════════════════════════════════════════════════

static inline void swap32_if_be(u32 *n, int endian) {
    if (!endian) return;
    u32 t = *n;
    *n = ((t&0xff000000)>>24)|((t&0x00ff0000)>>8)|((t&0x0000ff00)<<8)|((t&0x000000ff)<<24);
}
static inline void swap64_if_be(u64 *n, int endian) {
    if (!endian) return;
    u64 t = *n;
    *n = ((u64)(t&0xff00000000000000ULL)>>56) | ((u64)(t&0x00ff000000000000ULL)>>40)
       | ((u64)(t&0x0000ff0000000000ULL)>>24) | ((u64)(t&0x000000ff00000000ULL)>> 8)
       | ((u64)(t&0x00000000ff000000ULL)<< 8) | ((u64)(t&0x0000000000ff0000ULL)<<24)
       | ((u64)(t&0x000000000000ff00ULL)<<40) | ((u64)(t&0x00000000000000ffULL)<<56);
}
static inline void swap_daa_if_be(daa_t *d, int endian) {
    if (!endian) return;
    swap32_if_be(&d->size_offset, 1);
    swap32_if_be(&d->version, 1);
    swap32_if_be(&d->data_offset, 1);
    swap32_if_be(&d->b1, 1);
    swap32_if_be(&d->b0, 1);
    swap32_if_be(&d->chunksize, 1);
    swap64_if_be(&d->isosize, 1);
    swap64_if_be(&d->daasize, 1);
    swap32_if_be(&d->crc, 1);
}

// ═══════════════════════════════════════════════════════════════════════════
//  CRC-32
// ═══════════════════════════════════════════════════════════════════════════

static u32 crc32_calc(const u8 *data, int size) {
    static const u32 T[] = {
        0x00000000,0x77073096,0xee0e612c,0x990951ba,0x076dc419,0x706af48f,
        0xe963a535,0x9e6495a3,0x0edb8832,0x79dcb8a4,0xe0d5e91e,0x97d2d988,
        0x09b64c2b,0x7eb17cbd,0xe7b82d07,0x90bf1d91,0x1db71064,0x6ab020f2,
        0xf3b97148,0x84be41de,0x1adad47d,0x6ddde4eb,0xf4d4b551,0x83d385c7,
        0x136c9856,0x646ba8c0,0xfd62f97a,0x8a65c9ec,0x14015c4f,0x63066cd9,
        0xfa0f3d63,0x8d080df5,0x3b6e20c8,0x4c69105e,0xd56041e4,0xa2677172,
        0x3c03e4d1,0x4b04d447,0xd20d85fd,0xa50ab56b,0x35b5a8fa,0x42b2986c,
        0xdbbbc9d6,0xacbcf940,0x32d86ce3,0x45df5c75,0xdcd60dcf,0xabd13d59,
        0x26d930ac,0x51de003a,0xc8d75180,0xbfd06116,0x21b4f4b5,0x56b3c423,
        0xcfba9599,0xb8bda50f,0x2802b89e,0x5f058808,0xc60cd9b2,0xb10be924,
        0x2f6f7c87,0x58684c11,0xc1611dab,0xb6662d3d,0x76dc4190,0x01db7106,
        0x98d220bc,0xefd5102a,0x71b18589,0x06b6b51f,0x9fbfe4a5,0xe8b8d433,
        0x7807c9a2,0x0f00f934,0x9609a88e,0xe10e9818,0x7f6a0dbb,0x086d3d2d,
        0x91646c97,0xe6635c01,0x6b6b51f4,0x1c6c6162,0x856530d8,0xf262004e,
        0x6c0695ed,0x1b01a57b,0x8208f4c1,0xf50fc457,0x65b0d9c6,0x12b7e950,
        0x8bbeb8ea,0xfcb9887c,0x62dd1ddf,0x15da2d49,0x8cd37cf3,0xfbd44c65,
        0x4db26158,0x3ab551ce,0xa3bc0074,0xd4bb30e2,0x4adfa541,0x3dd895d7,
        0xa4d1c46d,0xd3d6f4fb,0x4369e96a,0x346ed9fc,0xad678846,0xda60b8d0,
        0x44042d73,0x33031de5,0xaa0a4c5f,0xdd0d7cc9,0x5005713c,0x270241aa,
        0xbe0b1010,0xc90c2086,0x5768b525,0x206f85b3,0xb966d409,0xce61e49f,
        0x5edef90e,0x29d9c998,0xb0d09822,0xc7d7a8b4,0x59b33d17,0x2eb40d81,
        0xb7bd5c3b,0xc0ba6cad,0xedb88320,0x9abfb3b6,0x03b6e20c,0x74b1d29a,
        0xead54739,0x9dd277af,0x04db2615,0x73dc1683,0xe3630b12,0x94643b84,
        0x0d6d6a3e,0x7a6a5aa8,0xe40ecf0b,0x9309ff9d,0x0a00ae27,0x7d079eb1,
        0xf00f9344,0x8708a3d2,0x1e01f268,0x6906c2fe,0xf762575d,0x806567cb,
        0x196c3671,0x6e6b06e7,0xfed41b76,0x89d32be0,0x10da7a5a,0x67dd4acc,
        0xf9b9df6f,0x8ebeeff9,0x17b7be43,0x60b08ed5,0xd6d6a3e8,0xa1d1937e,
        0x38d8c2c4,0x4fdff252,0xd1bb67f1,0xa6bc5767,0x3fb506dd,0x48b2364b,
        0xd80d2bda,0xaf0a1b4c,0x36034af6,0x41047a60,0xdf60efc3,0xa867df55,
        0x316e8eef,0x4669be79,0xcb61b38c,0xbc66831a,0x256fd2a0,0x5268e236,
        0xcc0c7795,0xbb0b4703,0x220216b9,0x5505262f,0xc5ba3bbe,0xb2bd0b28,
        0x2bb45a92,0x5cb36a04,0xc2d7ffa7,0xb5d0cf31,0x2cd99e8b,0x5bdeae1d,
        0x9b64c2b0,0xec63f226,0x756aa39c,0x026d930a,0x9c0906a9,0xeb0e363f,
        0x72076785,0x05005713,0x95bf4a82,0xe2b87a14,0x7bb12bae,0x0cb61b38,
        0x92d28e9b,0xe5d5be0d,0x7cdcefb7,0x0bdbdf21,0x86d3d2d4,0xf1d4e242,
        0x68ddb3f8,0x1fda836e,0x81be16cd,0xf6b9265b,0x6fb077e1,0x18b74777,
        0x88085ae6,0xff0f6a70,0x66063bca,0x11010b5c,0x8f659eff,0xf862ae69,
        0x616bffd3,0x166ccf45,0xa00ae278,0xd70dd2ee,0x4e048354,0x3903b3c2,
        0xa7672661,0xd06016f7,0x4969474d,0x3e6e77db,0xaed16a4a,0xd9d65adc,
        0x40df0b66,0x37d83bf0,0xa9bcae53,0xdebb9ec5,0x47b2cf7f,0x30b5ffe9,
        0xbdbdf21c,0xcabac28a,0x53b39330,0x24b4a3a6,0xbad03605,0xcdd70693,
        0x54de5729,0x23d967bf,0xb3667a2e,0xc4614ab8,0x5d681b02,0x2a6f2b94,
        0xb40bbe37,0xc30c8ea1,0x5a05df1b,0x2d02ef8d
    };
    u32 crc = 0xffffffff;
    for (const u8 *p = data, *end = data+size; p < end; p++)
        crc = T[*p ^ (crc & 0xff)] ^ (crc >> 8);
    return ~crc;
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
//  Bit-reader for v1.10 index table (static powerisuxn, reset per conversion)
// ═══════════════════════════════════════════════════════════════════════════

static unsigned daa2iso_read_bits(unsigned bits, const u8 *in, unsigned in_bits,
                                   unsigned lame, int lame_increase) {
    static const u8 powerisux[] = "\x0A\x35\x2D\x3F\x08\x33\x09\x15";
    static int powerisuxn = 0;
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

// Helper to reset the bit-reader's internal counter before a new conversion.
static void reset_bit_reader() {
    static int powerisuxn = 0;
    powerisuxn = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Case‑insensitive suffix check
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
//  Library context and helpers
// ═══════════════════════════════════════════════════════════════════════════

struct DaaContext {
    FILE       *fdi          = nullptr;
    FILE       *fdo          = nullptr;
    std::string outputPath;

    int         multi        = 0;
    int         multinum     = 0;
    char       *multi_filename = nullptr;

    int         endian       = 0;
    int         daagbi       = TYPE_DAA;
    unsigned    swapped_btype[3] = {0, 1, 2};

    u8         *in           = nullptr;
    u32         insz         = 0;
    u8         *out_buf      = nullptr;
    u32         outsz        = 0;

    CLzmaDec    lzma         = {};

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
    if (fread(&daa, 1, sizeof(daa), fd) != sizeof(daa)) { fclose(fd); throw DaaError("volume header read error"); }
    swap_daa_if_be(&daa, ctx.endian);
    if (strncmp((char*)daa.sign,"DAA VOL",16) && strncmp((char*)daa.sign,"GBI VOL",16)) {
        fclose(fd); throw DaaError("wrong DAA VOL signature");
    }
    if (fseek(fd, daa.size_offset, SEEK_SET)) { fclose(fd); throw DaaError("fseek on volume"); }
    ctx.multinum++;
    return fd;
}

static void ctx_read(DaaContext &ctx, void *data, unsigned size) {
    unsigned len = (unsigned)fread(data, 1, size, ctx.fdi);
    if (len == size) return;
    if (!ctx.multi) throw DaaError("incomplete input file");
    fclose(ctx.fdi);
    ctx.fdi = ctx_next_volume(ctx);
    ctx_read(ctx, (u8*)data + len, size - len);
}

static void ctx_alloc(u8 **data, unsigned wantsize, unsigned *currsize) {
    if (wantsize <= *currsize) return;
    *data = (u8*)realloc(*data, wantsize);
    if (!*data) throw DaaError("out of memory");
    *currsize = wantsize;
}

static bool daa_fail(DaaContext &ctx) {
    if (ctx.fdo) { fclose(ctx.fdo); ctx.fdo = nullptr; }
    if (!ctx.outputPath.empty()) fs::remove(ctx.outputPath);
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Global cancellation flag (set by external code)
// ═══════════════════════════════════════════════════════════════════════════

std::atomic<bool> g_operationCancelled = false;

// ═══════════════════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════════════════

bool convertDaaToIso(const std::string &inputFile,
                     const std::string &outputFile,
                     std::atomic<size_t> *completedBytes)
{
    if (g_operationCancelled.load()) return false;

    // Reset global bit-reader state for this conversion
    reset_bit_reader();
    tinf_init();  // idempotent, but ensures tables are built

    DaaContext ctx;
    ctx.outputPath    = outputFile;
    ctx.completedBytes = completedBytes;
    if (completedBytes) *completedBytes = 0;

    // Host endianness detection (0 = little-endian, no swap needed)
    { int e = 1; ctx.endian = (*(char*)&e) ? 0 : 1; }

    ctx.fdi = fopen(inputFile.c_str(), "rb");
    if (!ctx.fdi) return false;

    ctx.fdo = fopen(outputFile.c_str(), "wb");
    if (!ctx.fdo) return daa_fail(ctx);

    try {
        daa_t daa;
        ctx_read(ctx, &daa, sizeof(daa));
        u32 daacrc = crc32_calc((u8*)&daa, sizeof(daa) - 4);
        swap_daa_if_be(&daa, ctx.endian);

        if (!strncmp((char*)daa.sign,"DAA",16) || !strncmp((char*)daa.sign,"\xb8\xbd\xb6",3))
            ctx.daagbi = TYPE_DAA;
        else if (!strncmp((char*)daa.sign,"GBI",16))
            ctx.daagbi = TYPE_GBI;
        else {
            if (!strncmp((char*)daa.sign,"DAA VOL",16) ||
                !strncmp((char*)daa.sign,"GBI VOL",16))
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
        u32 ver110_x=0, ver110_y=0;

        if (daa.version == 0x100) {
            daas_mem = daa.data_offset - daa.size_offset;
            daa_data = (daa_data_t*)malloc(daas_mem);
            if (!daa_data) throw DaaError("out of memory (daa_data)");
            daas = daas_mem / 3;
        } else {
            ver110_x = daa.data_offset;
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
            daas     = (daas_mem << 3) / (unsigned)(bittype + bitsize);
            daas_mem = (((unsigned)(bitsize + bittype) * daas) + 7) >> 3;

            if (ver110_y & 0x4000) {
                daas_mem += 0x10000;
                daa_dataz = *(u32*)(daa.hdata + 1);
                swap32_if_be(&daa_dataz, ctx.endian);
            }
            daa_data = (daa_data_t*)malloc(daas_mem);
            if (!daa_data) throw DaaError("out of memory (daa_data v110)");

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

        // Pre-data records
        while ((u64)ftell(ctx.fdi) < daa.size_offset) {
            u32 rec_type, rec_len;
            ctx_read(ctx, &rec_type, 4); swap32_if_be(&rec_type, ctx.endian);
            ctx_read(ctx, &rec_len,  4); swap32_if_be(&rec_len,  ctx.endian);
            if (rec_type == 1) ctx.multi = 1;
            else if (rec_type == 3) throw DaaError("password-protected DAA not supported");
            if (fseek(ctx.fdi, rec_len - 8, SEEK_CUR)) throw DaaError("fseek on pre-data record");
        }

        // Multi-volume detection
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

        // Read index table
        if (fseek(ctx.fdi, daa.size_offset, SEEK_SET)) throw DaaError("fseek to size_offset");

        if (daa_dataz) {
            ctx_alloc(&ctx.in, daa_dataz, &ctx.insz);
            ctx_read(ctx, ctx.in, daa_dataz);
            unsigned destLen = daas_mem;
            if (tinf_uncompress(daa_data, &destLen, ctx.in, daa_dataz, ctx.swapped_btype) != TINF_OK)
                throw DaaError("failed to decompress index table");
            daas     = (destLen << 3) / (unsigned)(bittype + bitsize);
            daas_mem = (((unsigned)(bitsize + bittype) * daas) + 7) >> 3;
        } else {
            ctx_read(ctx, daa_data, daas_mem);
        }

        if (ctx.daagbi == TYPE_GBI) gburner_lame((u8*)daa_data, daas_mem, daa.crc & 0xff);
        if (dolame)                  poweriso_lame((u8*)daa_data, daas_mem, daa.isosize);

        if (fseek(ctx.fdi, daa.data_offset, SEEK_SET)) throw DaaError("fseek to data_offset");

        // Main decompression loop
        ctx_alloc(&ctx.out_buf, daa.chunksize, &ctx.outsz);
        u64  tot       = 0;
        u32  last_chunk = daas - 1;

        for (u32 i = 0; i < daas; i++) {
            if (g_operationCancelled.load()) {
                free(daa_data);
                return daa_fail(ctx);
            }

            u32 len;
            if (daa.version == 0x100) {
                len   = ((u32)daa_data[i].n1<<16) | daa_data[i].n2 | ((u32)daa_data[i].n3<<8);
                ztype = (len >= daa.chunksize) ? -1 : 1;
            } else {
                len   = daa2iso_read_bits(bitsize,(u8*)daa_data,bitpos,dolamebits,0);
                bitpos += bitsize;
                len  += LZMA_PROPS_SIZE;
                ztype = (int)daa2iso_read_bits(bittype,(u8*)daa_data,bitpos,dolamebits,1);
                bitpos += bittype;
                if (len >= daa.chunksize) ztype = -1;
            }

            ctx_alloc(&ctx.in, len, &ctx.insz);
            ctx_read(ctx, ctx.in, len);

            u32 outlen;
            switch (ztype) {
                case -1:
                    outlen = daa.chunksize;
                    memcpy(ctx.out_buf, ctx.in, len);
                    break;
                case 0:
                    LzmaDec_Init(&ctx.lzma);
                    outlen = lzma_decode_full(&ctx.lzma, ctx.in, len, ctx.out_buf, ctx.outsz);
                    if (outlen == 0) throw DaaError("LZMA decompression failed");
                    if (lzma_filter) poweriso_is_shit(ctx.out_buf, (int)outlen);
                    break;
                case 1: {
                    unsigned destLen = ctx.outsz;
                    if (tinf_uncompress(ctx.out_buf, &destLen, ctx.in, len, ctx.swapped_btype) != TINF_OK)
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
                    throw DaaError("chunk size mismatch after decompression");
            }

            if (fwrite(ctx.out_buf, 1, outlen, ctx.fdo) != outlen)
                throw DaaError("write failed – check disk space");

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

// ═══════════════════════════════════════════════════════════════════════════
//  Command-line main (if compiled standalone)
// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[]) {
    printf("\nDAA2ISO (Linux C++ port) – library & command-line tool\n"
           "Original algorithms by Luigi Auriemma (aluigi.org)\n\n");

    if (argc < 3) {
        printf("Usage: %s <input.daa> <output.iso>\n\n", argv[0]);
        return 0;
    }

    const std::string input  = argv[1];
    const std::string output = argv[2];

    // Check if output exists and ask for overwrite (same as original)
    FILE *test = fopen(output.c_str(), "rb");
    if (test) {
        fclose(test);
        char ans[8];
        printf("- output file exists, overwrite? (y/N) ");
        fflush(stdout);
        if (!fgets(ans, sizeof(ans), stdin)) ans[0] = 'n';
        if (ans[0] != 'y' && ans[0] != 'Y') {
            printf("aborted.\n");
            return 0;
        }
    }

    printf("- converting %s → %s\n", input.c_str(), output.c_str());
    bool ok = convertDaaToIso(input, output, nullptr);
    if (ok)
        printf("- finished successfully\n");
    else
        printf("- conversion failed\n");

    return ok ? 0 : 1;
}
