/*
 * aac_decoder.c - AAC-LC ADTS decoder for tOS
 * Long blocks, mono/stereo, ADTS format. No SBR/PS/LTP.
 * Output: 8-bit unsigned mono PCM at AUDIO_OUT_RATE (22050 Hz).
 */
#include "aac_decoder.h"
#include "audio.h"
#include "string.h"
#include "memory.h"

/* ---- Bit reader ---------------------------------------------------------- */
typedef struct { const uint8_t *buf; uint32_t len; uint32_t pos; uint32_t acc; int avail; } bs_t;
static void bs_init(bs_t *b, const uint8_t *p, uint32_t n)
{ b->buf=p; b->len=n; b->pos=0; b->acc=0; b->avail=0; }
static int bs_get1(bs_t *b) {
    if (!b->avail) {
        if (b->pos >= b->len) return 0;
        b->acc = b->buf[b->pos++]; b->avail = 8;
    }
    return (int)((b->acc >> --b->avail) & 1);
}
static uint32_t bs_getn(bs_t *b, int n)
{ uint32_t v=0; int i; for(i=0;i<n;i++) v=(v<<1)|(uint32_t)bs_get1(b); return v; }
static int bs_bits_left(bs_t *b)
{ return (int)(b->len - b->pos)*8 + b->avail; }

/* ---- Huffman: canonical code lengths → decode table --------------------- */
typedef struct { uint32_t code; uint8_t len; int16_t sym; } hcw_t;

static int build_huff(const uint8_t *lens, int n, hcw_t *out) {
    int cnt=0, l, i;
    int max_l=0;
    for(i=0;i<n;i++) if(lens[i]>max_l) max_l=lens[i];
    for(l=1;l<=max_l;l++) for(i=0;i<n;i++) if(lens[i]==(uint8_t)l)
        { out[cnt].len=(uint8_t)l; out[cnt].sym=(int16_t)i; cnt++; }
    uint32_t code=0; uint8_t pl=0;
    for(i=0;i<cnt;i++) {
        if(out[i].len!=pl) { code<<=out[i].len-pl; pl=out[i].len; }
        out[i].code=code++;
    }
    return cnt;
}
static int huff_dec(bs_t *b, const hcw_t *t, int n) {
    uint32_t acc=0; int l=0, i;
    for(;;){ acc=(acc<<1)|(uint32_t)bs_get1(b); l++;
        for(i=0;i<n;i++) if(t[i].len==(uint8_t)l && t[i].code==acc) return t[i].sym;
        if(l>20) return -1; }
}

/* ---- Huffman code-length tables (ISO 14496-3:2009 Tables 4.A.2-4.A.12) -- */
/* Scale factor (121 symbols, delta offset=60) */
static const uint8_t sf_l[121]={
    18,18,18,18,18,18,19,19,19,19,19,19,19,19,19,19,
    19,18,18,18,18,18,18,17,17,17,17,17,16,16,16,16,
    16,15,15,14,14,14,14,13,13,12,12,11,11,10, 9, 8,
     8, 7, 6, 5, 4, 3, 2, 1, 2, 3, 4, 5, 6, 7, 8, 8,
     9,10,11,11,12,12,13,13,14,14,14,14,15,15,16,16,
    16,16,16,17,17,17,17,17,18,18,18,18,18,18,19,19,
    19,19,19,19,19,19,19,19,19,18,18,18,18,18,18,18,
    18,18,18,18,18,18
};
/* CB1: 4-D {0,1}  (16 syms, sign bits separate) */
static const uint8_t cb1_l[16]={1,5,5,7,5,7,7,9,5,7,7,9,7,9,9,11};
/* CB2: 4-D {0,1} */
static const uint8_t cb2_l[16]={3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7};
/* CB3: 4-D {0,1,2} (81 syms) */
static const uint8_t cb3_l[81]={
    1, 5, 5, 7, 5, 7, 7,10, 5, 7, 7,10, 7,10,10,11, 5, 7, 7,10,
    7,10,10,11, 7,10,10,11,10,11,11,12, 5, 7, 7,10, 7,10,10,11,
    7,10,10,11,10,11,11,12, 7,10,10,11,10,11,11,12,10,11,11,12,
   11,12,12,13, 5, 7, 7,10, 7,10,10,11, 7,10,10,11,10,11,11,12,7
};
/* CB4: 4-D {0,1,2} */
static const uint8_t cb4_l[81]={
    4, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6,
    5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8, 4, 5, 5, 6, 5, 6, 6, 7,
    5, 6, 6, 7, 6, 7, 7, 8, 5, 6, 6, 7, 6, 7, 7, 8, 6, 7, 7, 8,
    7, 8, 8, 9, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8,4
};
/* CB5: 2-D signed {-4..4} (81 syms, idx=(a+4)*9+(b+4)) */
static const uint8_t cb5_l[81]={
   13,12,11,10, 9, 8, 9,10,11,12,11,10, 9, 8, 7, 8, 9,10,
   11,10, 9, 8, 7, 6, 7, 8, 9,10, 9, 8, 7, 6, 5, 6, 7, 8,
    9, 8, 7, 6, 5, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 4, 5, 6,
    9, 8, 7, 6, 5, 4, 5, 6, 7,10, 9, 8, 7, 6, 5, 6, 7, 8,
   11,10, 9, 8, 7, 6, 7, 8, 9
};
/* CB6: 2-D signed {-4..4} */
static const uint8_t cb6_l[81]={
   11,10, 9, 8, 7, 7, 7, 8, 9,10, 9, 8, 7, 6, 6, 6, 7, 8,
    9, 8, 7, 6, 5, 5, 5, 6, 7, 8, 7, 6, 5, 4, 4, 4, 5, 6,
    7, 6, 5, 4, 3, 3, 3, 4, 5, 7, 6, 5, 4, 3, 2, 3, 4, 5,
    7, 6, 5, 4, 3, 3, 3, 4, 5, 8, 7, 6, 5, 4, 4, 4, 5, 6,
    9, 8, 7, 6, 5, 5, 5, 6, 7
};
/* CB7: 2-D unsigned {0..7} (64 syms) */
static const uint8_t cb7_l[64]={
    1, 4, 5, 6, 7, 8, 9,10, 4, 5, 6, 6, 7, 8, 8, 9,
    5, 6, 6, 7, 7, 8, 8, 9, 6, 6, 7, 7, 7, 8, 8, 9,
    7, 7, 7, 7, 8, 8, 9,10, 8, 8, 8, 8, 8, 9, 9,10,
    9, 8, 8, 9, 9, 9,10,10,10, 9, 9, 9,10,10,10,11
};
/* CB8: 2-D unsigned {0..7} */
static const uint8_t cb8_l[64]={
    2, 3, 4, 5, 6, 7, 8, 9, 3, 3, 4, 5, 6, 7, 7, 8,
    4, 4, 4, 5, 6, 6, 7, 8, 5, 5, 5, 5, 6, 6, 7, 8,
    6, 6, 6, 6, 6, 7, 7, 8, 7, 7, 6, 7, 7, 7, 8, 8,
    8, 7, 7, 7, 7, 8, 8, 9, 9, 8, 8, 8, 8, 8, 9, 9
};
/* CB9: 2-D unsigned {0..12} (169 syms) */
static const uint8_t cb9_l[169]={
    1, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,
    4, 5, 6, 6, 7, 8, 8, 9,10,11,12,12,13,
    5, 6, 6, 7, 7, 8, 8, 9, 9,10,11,12,13,
    6, 6, 7, 7, 8, 8, 8, 9, 9,10,11,11,12,
    7, 7, 7, 8, 8, 8, 9, 9,10,10,11,11,12,
    8, 8, 8, 8, 8, 9, 9,10,10,11,11,12,13,
    9, 8, 8, 8, 9, 9, 9,10,10,11,11,12,12,
   10, 9, 9, 9, 9, 9,10,10,11,11,11,12,13,
   11,10, 9, 9,10,10,10,11,11,11,12,12,13,
   12,11,10,10,10,10,11,11,11,12,12,13,13,
   13,12,11,11,11,11,11,11,12,12,13,13,14,
   14,13,12,11,11,12,12,12,12,13,13,13,14,
   15,14,13,12,12,13,12,13,13,13,14,14,14
};
/* CB10: 2-D unsigned {0..12} */
static const uint8_t cb10_l[169]={
    2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    3, 4, 4, 5, 6, 7, 7, 8, 9,10,11,12,13,
    4, 4, 5, 5, 6, 7, 7, 8, 8, 9,10,11,12,
    5, 5, 5, 6, 6, 7, 7, 8, 8, 9,10,10,11,
    6, 6, 6, 6, 7, 7, 7, 8, 8, 9, 9,10,11,
    7, 7, 7, 7, 7, 7, 8, 8, 9, 9,10,10,11,
    8, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9,10,11,
    9, 8, 8, 8, 8, 8, 8, 9, 9, 9,10,10,11,
   10, 9, 8, 8, 8, 8, 9, 9, 9,10,10,11,11,
   11,10, 9, 8, 9, 9, 9, 9,10,10,10,11,12,
   12,11,10, 9, 9, 9, 9,10,10,10,11,11,12,
   13,12,11,10,10,10,10,10,11,11,11,12,12,
   14,13,12,11,11,11,11,11,11,12,12,12,13
};
/* CB11: 2-D unsigned {0..16}+esc (289 syms) */
static const uint8_t cb11_l[289]={
    2, 4, 6, 7, 8, 9,10,11,11,12,12,13,13,14,14,14,14,
    4, 4, 6, 7, 8, 8, 9,10,11,11,12,12,13,13,14,14,15,
    6, 6, 6, 7, 8, 8, 9, 9,10,10,11,12,12,13,13,14,14,
    7, 7, 7, 7, 8, 8, 9, 9,10,10,11,11,12,12,13,13,14,
    8, 8, 8, 8, 8, 8, 9, 9,10,10,11,11,11,12,13,13,13,
    9, 8, 8, 8, 8, 9, 9, 9,10,10,10,11,12,12,13,13,14,
   10, 9, 9, 9, 9, 9, 9,10,10,10,11,11,12,12,12,13,13,
   11,10, 9, 9, 9, 9,10,10,10,11,11,12,12,12,13,13,14,
   11,11,10, 9,10,10,10,10,10,11,11,11,12,13,13,14,14,
   12,11,10,10,10,10,10,11,11,11,12,12,12,13,13,13,14,
   12,12,11,10,10,10,11,11,11,12,12,12,13,13,13,14,14,
   13,12,11,11,11,11,11,11,12,12,12,13,13,13,14,14,14,
   13,13,12,11,11,11,12,12,12,12,13,13,13,13,14,14,15,
   14,13,12,11,12,12,12,12,13,13,13,13,13,14,14,15,15,
   14,14,13,12,12,12,12,13,13,13,13,14,14,14,14,15,15,
   14,14,13,12,13,13,13,13,13,13,14,14,14,15,15,15,15,
   15,14,14,13,13,13,13,13,14,14,14,14,15,15,15,15,15
};

/* ---- Decode tables (built at runtime) ------------------------------------ */
static hcw_t sf_tab[121], cb1_tab[16], cb2_tab[16];
static hcw_t cb3_tab[81],  cb4_tab[81];
static hcw_t cb5_tab[81],  cb6_tab[81];
static hcw_t cb7_tab[64],  cb8_tab[64];
static hcw_t cb9_tab[169], cb10_tab[169], cb11_tab[289];
static int   sf_n, cb1_n, cb2_n, cb3_n, cb4_n, cb5_n, cb6_n;
static int   cb7_n, cb8_n, cb9_n, cb10_n, cb11_n;
static int   g_built = 0;

static void build_tables(void) {
    if (g_built) return;
    g_built=1;
    sf_n  =build_huff(sf_l,  121,sf_tab);
    cb1_n =build_huff(cb1_l,  16,cb1_tab); cb2_n=build_huff(cb2_l,16,cb2_tab);
    cb3_n =build_huff(cb3_l,  81,cb3_tab); cb4_n=build_huff(cb4_l,81,cb4_tab);
    cb5_n =build_huff(cb5_l,  81,cb5_tab); cb6_n=build_huff(cb6_l,81,cb6_tab);
    cb7_n =build_huff(cb7_l,  64,cb7_tab); cb8_n=build_huff(cb8_l,64,cb8_tab);
    cb9_n =build_huff(cb9_l, 169,cb9_tab); cb10_n=build_huff(cb10_l,169,cb10_tab);
    cb11_n=build_huff(cb11_l,289,cb11_tab);
}

/* ---- Inverse quantisation table: |q|^(4/3) in Q10 for q=0..255 ---------- */
static int32_t iq256[256];
static int     iq_done=0;

/* Integer x^(4/3) via cbrt Newton-Raphson */
static int32_t pow43_q10(int x) {
    if (x<=0) return 0;
    /* cbrt(x) via Newton: y_{n+1} = (2y + x/y^2)/3 */
    int32_t y=1, xx=(int32_t)x;
    int i;
    while (y*y*y < xx) y++;   /* y = ceil(cbrt(x)) */
    for (i=0;i<8;i++) {       /* refine with Newton in fixed-point */
        int32_t y2=y*y;
        if (!y2) break;
        int32_t yn=(2*y + xx/y2)/3;
        if (yn==y) break;
        y=yn; if(y<1)y=1;
    }
    /* y ≈ cbrt(x); result = x * cbrt(x) * 1024 = x * y * 1024 / (correction) */
    /* scale: return x^(4/3)*1024 = x*cbrt(x)*1024 */
    /* y is integer cbrt, so x*y*1024/y ... no: x^(4/3)=x*cbrt(x) */
    /* cbrt_q10 = y * 1024 would be integer*1024, but cbrt(x) ≈ y (integer floor) */
    /* Better: cbrt_q10 = y*1024 + frac correction */
    /* frac: cbrt(x) - y ≈ (x - y^3) / (3*y^2) */
    int32_t y3=y*y*y;
    int32_t frac=0;
    if (y>0) frac = (xx - y3)*1024 / (3*y*y);  /* Q10 fractional correction */
    int32_t cbrt_q10 = y*1024 + frac;
    int64_t r = (int64_t)xx * cbrt_q10;
    if (r > 0x7FFFFFFF) r=0x7FFFFFFF;
    return (int32_t)r;
}

static void build_iq(void) {
    if (iq_done) return;
    iq_done=1;
    int i; iq256[0]=0;
    for(i=1;i<256;i++) iq256[i]=pow43_q10(i);
}

/* Lookup: dequantise |q| → Q10 value */
static int32_t iq_lookup(int q) {
    if (q<0) q=-q;
    if (q<256) return iq256[q];
    /* For large q: approximate using q^(4/3) ≈ 256^(4/3) * (q/256)^(4/3)
       256^(4/3) = 256*256^(1/3) = 256*6.35 ≈ 1625 (Q10 = 1663K) */
    /* Use linear extrapolation from q=255 */
    int32_t v = iq256[255];
    int32_t delta = v - iq256[254];
    int32_t r = v + (int32_t)(q-255) * delta;
    if (r < 0) r=0;
    return r;
}

/* ---- 2048-entry cosine table for IMDCT ---------------------------------- */
/* cos_tab[j] = round(cos(j*pi/2048) * 16384)  j=0..4095 */
static int16_t cos_tab[4096];
static int     cos_done=0;

/* Integer cos via Chebyshev (same approach as mp3_decoder) */
static int16_t icos14(int num, int den) {
    /* cos(num*pi/den) * 16384 */
    int64_t pi_q30 = (int64_t)3373259426LL;
    int64_t a = pi_q30 * num / den;
    int64_t a2 = (a*a)>>30;
    int64_t a4 = (a2*a2)>>30;
    int64_t a6 = (a4*a2)>>30;
    int64_t c = (int64_t)1073741824LL - a2/2 + a4/24 - a6/720;
    return (int16_t)(c >> 16);   /* Q30 → Q14 */
}

static void build_cos_tab(void) {
    if (cos_done) return;
    cos_done=1;
    int j;
    for (j=0; j<=2048; j++) cos_tab[j] = icos14(j, 2048);
    for (j=2049; j<4096; j++) cos_tab[j] = cos_tab[4096-j];
}

/* ---- IMDCT-1024 + windowing + overlap-add ------------------------------- */
/* Sine window: w[n] = sin(pi/2048 * (n+0.5)) = sin((2n+1)*pi/4096) */
static void imdct1024(const int32_t *X, int32_t *out2048, int32_t *prev1024)
{
    int n, k;
    /* Compute 2048 output samples.
       y[n] = sum_{k=0}^{1023} X[k] * cos(pi/2048*(2n+1025)*(2k+1))
       Using precomputed cos_tab: index = ((2n+1025)*(2k+1)) % 4096 */
    for (n=0; n<2048; n++) {
        int64_t acc=0;
        uint32_t base = (uint32_t)(2*n+1025);
        for (k=0; k<1024; k++) {
            uint32_t idx = (base * (uint32_t)(2*k+1)) & 0xFFF;  /* mod 4096 */
            acc += (int64_t)X[k] * cos_tab[idx];
        }
        /* acc is in Q(iq_scale + 14); scale down to Q15 PCM range */
        out2048[n] = (int32_t)(acc >> 14);
    }

    /* Apply sine window and overlap-add */
    for (n=0; n<1024; n++) {
        /* Window factor: sin((2n+1)*pi/4096) ≈ cos_tab[(2n+1)] (sin(x)=cos(pi/2-x)) */
        /* sin((2n+1)*pi/4096) = cos((4096/4 - (2n+1))*pi/4096) = cos((1023-2n)*pi/4096)?
           No: sin(x)=cos(pi/2 - x). pi/2 in our table = index 1024.
           sin((2n+1)*pi/4096) = cos(1024 - (2n+1)) = cos_tab[(1023-2n)] */
        /* But simpler: compute sine directly from cos table:
           sin(t*pi/4096) = cos((2048-t... no, sin(t*pi/4096)=cos(pi/2-t*pi/4096)=cos((2048-2t)*pi/4096)?
           Actually sin(t) = cos(pi/2 - t).  pi/2 corresponds to index 1024 in our table.
           sin(j*pi/4096) = cos_tab[1024 - j]  for j in [0,1024]   (since cos(pi/2-x)=sin(x))
           But cos_tab index must be in [0,4095]. */
        int wn = 2*n+1;  /* window argument index: sin(wn*pi/4096) */
        /* sin(wn*pi/4096) = cos((2048-wn)*pi/4096) when wn < 2048? No.
           sin(wn*pi/4096) = cos(pi/2 - wn*pi/4096) = cos((2048-wn)*pi/4096) */
        int wi = 2048 - wn;  /* wi in [2047..1] for n in [0..1023] */
        if (wi < 0) wi = -wi;
        int32_t w = (int32_t)cos_tab[wi & 0xFFF];
        /* first half: output = windowed(out2048[n]) + overlap */
        int32_t s0 = (int32_t)(((int64_t)out2048[n] * w) >> 14);
        /* window for prev overlap: sin((2*(n+1024)+1)*pi/4096) = sin((2n+2049)*pi/4096) */
        int wn2 = 2*n + 2049;
        int wi2 = 2048 - wn2; if(wi2<0)wi2=-wi2;
        int32_t w2 = (int32_t)cos_tab[wi2 & 0xFFF];
        int32_t s1 = (int32_t)(((int64_t)out2048[n+1024] * w2) >> 14);
        /* Overlap-add: current output = windowed first half + stored prev */
        out2048[n] = s0 + prev1024[n];
        /* Store second half for next frame */
        prev1024[n] = s1;
    }
}

/* ---- SFB offset tables (44100 and 48000 Hz long windows) ---------------- */
static const uint16_t sfb44[53]={
    0,4,8,12,16,20,24,28,32,36,40,48,56,64,72,80,88,96,108,120,
    132,144,160,176,196,216,240,264,292,320,352,384,416,448,480,512,
    544,576,608,640,672,704,736,768,800,832,864,896,928,1024,0,0,0
};
static const uint16_t sfb48[53] __attribute__((unused))={
    0,4,8,12,16,20,24,28,32,36,40,48,56,64,72,80,88,96,108,120,
    132,144,160,176,196,216,240,264,292,320,352,384,416,448,480,512,
    544,576,608,640,672,704,736,768,800,832,864,896,928,960,1024,0,0
};

/* ---- Individual channel decoding ---------------------------------------- */
/* Decode one channel's spectral data from bitstream */
static int decode_ics(bs_t *b, int32_t *spec, int global_gain, int *sf_out, int *num_sfb)
{
    /* individual_channel_stream syntax */
    bs_getn(b,1);              /* ics_reserved_bit */
    int win_seq  = (int)bs_getn(b,2);
    int win_shape= (int)bs_getn(b,1);
    (void)win_shape;
    if (win_seq == 2) { /* EIGHT_SHORT_SEQUENCE -- unsupported */
        return -1;
    }
    int max_sfb = (int)bs_getn(b,6);
    *num_sfb = max_sfb;
    bs_getn(b,1); /* predictor_data_present */

    /* Scalefactor band offsets */
    /* (use 44100 as default) */
    const uint16_t *sfb = sfb44;  /* caller adjusts if needed */

    /* section_data */
    int sfb_cb[AAC_MAX_SFB];
    int sfb_top[AAC_MAX_SFB];
    int cb;
    int sect_esc = 15; /* SECT_ESC_VAL */
    int sect_bits = 5; /* for long window */

    int sfb_i = 0;
    while (sfb_i < max_sfb) {
        cb = (int)bs_getn(b,4);
        int sect_len = 0;
        for (;;) {
            int incr = (int)bs_getn(b, sect_bits);
            sect_len += incr;
            if (incr < sect_esc) break;
        }
        int end = sfb_i + sect_len;
        if (end > AAC_MAX_SFB) end = AAC_MAX_SFB;
        while (sfb_i < end) sfb_cb[sfb_i++] = cb;
    }
    for (; sfb_i < AAC_MAX_SFB; sfb_i++) sfb_cb[sfb_i] = 0;
    for (sfb_i=0; sfb_i<max_sfb; sfb_i++) sfb_top[sfb_i] = sfb[sfb_i+1];

    /* scale_factor_data */
    int sf_val[AAC_MAX_SFB];
    int prev = global_gain;
    sf_val[0] = prev;
    for (sfb_i=1; sfb_i<max_sfb; sfb_i++) {
        if (sfb_cb[sfb_i] != 0) {
            int delta = huff_dec(b, sf_tab, sf_n) - 60;
            prev += delta;
        }
        sf_val[sfb_i] = prev;
    }
    if (sf_out) {
        int i; for(i=0;i<max_sfb;i++) sf_out[i]=sf_val[i];
    }

    /* spectral_data */
    int n;
    for (n=0; n<1024; n++) spec[n]=0;

    for (sfb_i=0; sfb_i<max_sfb; sfb_i++) {
        int cb_i = sfb_cb[sfb_i];
        int start = (int)sfb[sfb_i];
        int end2  = sfb_top[sfb_i];
        if (end2 > 1024) end2=1024;
        int i;

        /* Power of 2 scale: 2^((sf_val-100)/4) * 1/16 to stay in range */
        /* We'll apply scaling after quantize: sf_shift = (sf_val-100)/4 */
        /* For now keep in spec[] as dequantised Q10 * 2^(global_gain/4) */
        /* Actual: spec[i] = sign(q)*|q|^(4/3) * 2^((gain-100)/4) */
        /* Simplified: apply as bit-shift after iq_lookup */
        /* sf_exp = sf_val[sfb_i] - 100; shift = sf_exp / 4 */
        int sf_exp = sf_val[sfb_i] - 100;  /* can be negative */

        if (cb_i == 0) {
            /* ZERO_HCB: all zeros */
            for (i=start;i<end2;i++) spec[i]=0;
        } else if (cb_i >= 1 && cb_i <= 4) {
            /* 4-dimensional codebooks */
            hcw_t *tab = (cb_i<=2) ? (cb_i==1 ? cb1_tab : cb2_tab) : (cb_i==3?cb3_tab:cb4_tab);
            int    tn  = (cb_i<=2) ? (cb_i==1 ? cb1_n  : cb2_n)   : (cb_i==3?cb3_n:cb4_n);
            int max_val= (cb_i<=2) ? 1 : 2;
            for (i=start; i+3<end2; i+=4) {
                int sym = huff_dec(b, tab, tn);
                int vals[4];
                if (max_val==1) {
                    vals[0]=(sym>>3)&1; vals[1]=(sym>>2)&1;
                    vals[2]=(sym>>1)&1; vals[3]=sym&1;
                } else {
                    vals[0]=sym/27; sym%=27;
                    vals[1]=sym/9;  sym%=9;
                    vals[2]=sym/3;  vals[3]=sym%3;
                }
                int j;
                for (j=0;j<4;j++) {
                    if (vals[j]) {
                        int s = bs_get1(b) ? -1:1;
                        int32_t q = iq_lookup(vals[j]);
                        /* apply sf scale */
                        int32_t v = (sf_exp >= 0) ? (q << (sf_exp/4)) : (q >> (-sf_exp/4));
                        spec[i+j] = s * v;
                    }
                }
            }
        } else if (cb_i >= 5 && cb_i <= 6) {
            /* signed pair */
            hcw_t *tab = (cb_i==5) ? cb5_tab : cb6_tab;
            int    tn  = (cb_i==5) ? cb5_n   : cb6_n;
            for (i=start; i+1<end2; i+=2) {
                int sym = huff_dec(b, tab, tn);
                int a = sym/9 - 4, bv = sym%9 - 4;
                int32_t qa = iq_lookup(a), qb = iq_lookup(bv);
                int sa = a<0?-1:1, sb = bv<0?-1:1;
                int32_t va = (sf_exp>=0)?(qa<<(sf_exp/4)):(qa>>(-sf_exp/4));
                int32_t vb = (sf_exp>=0)?(qb<<(sf_exp/4)):(qb>>(-sf_exp/4));
                spec[i]   = sa * va;
                spec[i+1] = sb * vb;
            }
        } else if (cb_i >= 7 && cb_i <= 10) {
            hcw_t *tab; int tn, max_v;
            if (cb_i==7){tab=cb7_tab;tn=cb7_n;max_v=7;}
            else if(cb_i==8){tab=cb8_tab;tn=cb8_n;max_v=7;}
            else if(cb_i==9){tab=cb9_tab;tn=cb9_n;max_v=12;}
            else {tab=cb10_tab;tn=cb10_n;max_v=12;}
            int dim = (cb_i<=8)?8:13;
            for (i=start; i+1<end2; i+=2) {
                int sym = huff_dec(b, tab, tn);
                int a = sym/dim, bv = sym%dim;
                (void)max_v;
                int sa=1,sb=1;
                if (a) sa = bs_get1(b)?-1:1;
                if (bv) sb = bs_get1(b)?-1:1;
                int32_t qa=iq_lookup(a), qb=iq_lookup(bv);
                int32_t va=(sf_exp>=0)?(qa<<(sf_exp/4)):(qa>>(-sf_exp/4));
                int32_t vb=(sf_exp>=0)?(qb<<(sf_exp/4)):(qb>>(-sf_exp/4));
                spec[i]   = sa * va;
                spec[i+1] = sb * vb;
            }
        } else if (cb_i == 11) {
            /* escape coding */
            for (i=start; i+1<end2; i+=2) {
                int sym = huff_dec(b, cb11_tab, cb11_n);
                int a = sym/17, bv = sym%17;
                /* escape: if a==16, read extra */
                if (a==16) { int e=(int)bs_getn(b,4)+4; a=16+(int)bs_getn(b,e); }
                if (bv==16){ int e=(int)bs_getn(b,4)+4; bv=16+(int)bs_getn(b,e);}
                int sa=1,sb=1;
                if (a)  sa=bs_get1(b)?-1:1;
                if (bv) sb=bs_get1(b)?-1:1;
                int32_t qa=iq_lookup(a), qb=iq_lookup(bv);
                int32_t va=(sf_exp>=0)?(qa<<(sf_exp/4)):(qa>>(-sf_exp/4));
                int32_t vb=(sf_exp>=0)?(qb<<(sf_exp/4)):(qb>>(-sf_exp/4));
                spec[i]   = sa * va;
                spec[i+1] = sb * vb;
            }
        }
    }
    return 0;
}

/* ---- Decode one ADTS frame → 1024 mono PCM samples (int16) --------------- */
static int decode_adts_frame(aac_ctx_t *ctx, bs_t *b, int16_t *pcm_out)
{
    /* Read raw_data_block:
       Typical: one SCE (id_syn_ele 0b000) = single_channel_element */
    int i;
    for (;;) {
        int id = (int)bs_getn(b,3);
        if (id == 7) break; /* ID_END */
        int tag = (int)bs_getn(b,4);
        (void)tag;

        if (id == 0) {
            /* SCE: single_channel_element */
            int global_gain = (int)bs_getn(b,8);
            int32_t spec[1024];
            int num_sfb=0;
            if (decode_ics(b, spec, global_gain, NULL, &num_sfb) < 0) return -1;
            /* IMDCT */
            int32_t time2048[2048];
            imdct1024(spec, time2048, ctx->overlap[0]);
            for (i=0;i<1024;i++) {
                int32_t v = time2048[i] >> 6;
                if(v>32767)v=32767;
                if(v<-32768)v=-32768;
                pcm_out[i] = (int16_t)v;
            }
            return 1024;
        } else if (id == 1) {
            /* CPE: channel_pair_element */
            int ms_mask_present = (int)bs_getn(b,2);
            int ms_used[AAC_MAX_SFB]; int sfb;
            if (ms_mask_present == 1)
                for(sfb=0;sfb<AAC_MAX_SFB;sfb++) ms_used[sfb]=(int)bs_getn(b,1);
            else { for(sfb=0;sfb<AAC_MAX_SFB;sfb++) ms_used[sfb]=(ms_mask_present==3)?1:0; }
            int global_gain0=(int)bs_getn(b,8);
            int32_t spec0[1024];
            int num0=0;
            decode_ics(b,spec0,global_gain0,NULL,&num0);
            int global_gain1=(int)bs_getn(b,8);
            int32_t spec1[1024];
            int num1=0;
            decode_ics(b,spec1,global_gain1,NULL,&num1);
            /* M/S stereo */
            if (ms_mask_present) {
                for(sfb=0;sfb<num0&&sfb<num1;sfb++) {
                    if (!ms_used[sfb]) continue;
                    int s0=(int)sfb44[sfb], e0=(int)sfb44[sfb+1];
                    int j;
                    for(j=s0;j<e0&&j<1024;j++) {
                        int32_t m=spec0[j], s=spec1[j];
                        spec0[j]=m+s; spec1[j]=m-s;
                    }
                }
            }
            /* IMDCT both channels, mix to mono */
            int32_t t0[2048], t1[2048];
            imdct1024(spec0,t0,ctx->overlap[0]);
            imdct1024(spec1,t1,ctx->overlap[1]);
            for(i=0;i<1024;i++) {
                int32_t v = ((t0[i]+t1[i])>>1) >> 6;
                if(v>32767)v=32767;
                if(v<-32768)v=-32768;
                pcm_out[i]=(int16_t)v;
            }
            return 1024;
        } else if (id == 3) {
            /* CCE/LFE/DSE: skip */
            int cnt=(int)bs_getn(b,8);
            if (cnt==255) cnt+=(int)bs_getn(b,8);
            bs_getn(b, cnt*8);
        } else if (id == 5) {
            /* ID_PCE: skip */
            bs_getn(b,4); /* element_instance_tag */
            /* rest of PCE is complex, just break */
            break;
        } else {
            /* Unknown element: bail */
            break;
        }
        if (bs_bits_left(b) < 3) break;
    }
    return 0;
}

/* ======================================================================
   PUBLIC API
   ====================================================================== */

static const uint32_t sr_tbl[13]={96000,88200,64000,48000,44100,32000,
                                   24000,22050,16000,12000,11025,8000,7350};

int aac_open(aac_ctx_t *ctx, const uint8_t *data, uint32_t size)
{
    if (!data || size < 8) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->data = data;
    ctx->size = size;

    build_tables();
    build_iq();
    build_cos_tab();

    /* Scan for ADTS sync word 0xFFF / 0xFFE */
    uint32_t i;
    for (i=0; i+4<=size; i++) {
        if (data[i]==0xFF && (data[i+1]&0xF0)==0xF0) {
            uint8_t sridx = (data[i+2]>>2)&0xF;
            uint8_t ch    = ((data[i+2]&1)<<2)|((data[i+3]>>6)&3);
            if (sridx > 12) continue;
            ctx->sample_rate = sr_tbl[sridx];
            ctx->channels    = ch ? (ch<=2?ch:2) : 2;
            ctx->pos         = i;
            ctx->valid       = 1;
            /* Estimate total samples: size / avg_frame_size * 1024 */
            ctx->total_samples = (size / 400) * 1024;
            return 0;
        }
    }
    return -1;
}

/* Fill pcm[] with next decoded frame (1024 samples) */
static int aac_decode_next(aac_ctx_t *ctx)
{
    const uint8_t *d = ctx->data;
    uint32_t sz = ctx->size;
    uint32_t pos = ctx->pos;

    /* Find next ADTS sync */
    while (pos + 7 <= sz) {
        if (d[pos]==0xFF && (d[pos+1]&0xF0)==0xF0) break;
        pos++;
    }
    if (pos + 7 > sz) { ctx->valid=0; return 0; }

    /* Parse ADTS header */
    /* uint8_t id         = (d[pos+1]>>3)&1; */
    uint8_t prot       = d[pos+1]&1;          /* 1=no CRC, 0=CRC */
    uint16_t frame_len = ((uint16_t)(d[pos+3]&3)<<11)|((uint16_t)d[pos+4]<<3)|(d[pos+5]>>5);

    if (frame_len < 8 || pos + frame_len > sz) { pos++; ctx->pos=pos; return 0; }

    int hdr_bytes = prot ? 7 : 9;
    bs_t b;
    bs_init(&b, d + pos + hdr_bytes, frame_len - hdr_bytes);

    int n = decode_adts_frame(ctx, &b, ctx->pcm);
    ctx->pos = pos + frame_len;
    ctx->pcm_fill = (n > 0) ? (uint32_t)n : 0;
    ctx->pcm_pos  = 0;
    ctx->cur_sample += ctx->pcm_fill;
    return n > 0 ? 1 : 0;
}

uint32_t aac_read(aac_ctx_t *ctx, uint8_t *out, uint32_t out_len)
{
    if (!ctx->valid || !out_len) return 0;
    uint32_t written = 0;
    static uint32_t rs_frac = 0;

    while (written < out_len) {
        /* Refill PCM buffer if empty */
        if (ctx->pcm_pos >= ctx->pcm_fill) {
            if (!aac_decode_next(ctx)) break;
            if (!ctx->pcm_fill) break;
        }
        int32_t samp = (int32_t)ctx->pcm[ctx->pcm_pos];
        int32_t u8 = (samp >> 8) + 128;
        if (u8 < 0) u8=0;
        if (u8>255) u8=255;
        out[written++] = (uint8_t)u8;

        rs_frac += ctx->sample_rate;
        while (rs_frac >= AUDIO_OUT_RATE) {
            rs_frac -= AUDIO_OUT_RATE;
            ctx->pcm_pos++;
            if (ctx->pcm_pos >= ctx->pcm_fill) break;
        }
    }
    return written;
}

uint32_t aac_duration_sec(const aac_ctx_t *ctx)
{
    if (!ctx->valid || !ctx->sample_rate) return 0;
    return ctx->total_samples / ctx->sample_rate;
}

uint32_t aac_pos_sec(const aac_ctx_t *ctx)
{
    if (!ctx->valid || !ctx->sample_rate) return 0;
    return ctx->cur_sample / ctx->sample_rate;
}
