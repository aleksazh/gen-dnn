/*******************************************************************************
* Copyright 2017 NEC Laboratories America
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/
/** \file
 * sx vectorization ref_conv3.cpp */
#if ! defined(_SX)
// TODO ok for gcc, but others ???
#define restrict __restrict__
#endif

#include "conv/conv.hpp"
#include "idiv.hpp"

namespace conv {

// BWD + dilate is not fast for these loops (and mkl-dnn doesn't allow it yet)
extern void refconv_2_bwd_d(const prb_t *p, dnn_mem_t &diff_src_m,
        dnn_mem_t &wei_m, dnn_mem_t &diff_dst_m);
extern void refconv_4_bwd_d(const prb_t *p, dnn_mem_t &diff_src_m,
        dnn_mem_t &wei_m, dnn_mem_t &diff_dst_m);
extern void sxconv_2_bwd_d(const prb_t *p, dnn_mem_t &diff_src_m,
    dnn_mem_t &wei_m, dnn_mem_t &diff_dst_m);

static void chk( bool cond, char const* msg, char const* file, int const lineno ){
    if (!cond){ printf("@@@ error: %s : [%s:%d]\n", msg, file, lineno); exit(1); }
}
static bool trivial( int const verb, bool const cond, char const* msg,
                     char const* file, int const lineno ){
    if (verb > verbose && cond){
        printf("@@@ trivial: %s : [%s:%d]\n", msg, file, lineno); fflush(stdout);
    }

    return cond;
}
//#define MUST( COND )
#define MUST( COND )    chk(    (COND), #COND, __PRETTY_FUNCTION__, __LINE__)
#define PRINTF(...)     do{ printf(__VA_ARGS__); fflush(stdout);}while(0)
#define TRIVIAL( COND ) COND
//#define TRIVIAL( COND ) trivial(1, (COND), #COND, __PRETTY_FUNCTION__, __LINE__)

#if defined(NDEBUG)
#define DPRINTF(...)
#define DMUST(...)
#else
#define DPRINTF(...)  do{printf(__VA_ARGS__); fflush(stdout);}while(0)
#define DMUST(...)   MUST(__VA_ARGS__)
#endif

/** greatest common denominator, a,b > 0 */
static int gcd(int a, int b)
{
    for (;;)
    {
        if (a == 0) return b;
        b %= a;
        if (b == 0) return a;
        a %= b;
    }
}

/** lowest common multiple, a,b > 0 */
static int lcm(int a, int b)
{
    int temp = gcd(a, b);

    return temp ? (a / temp * b) : 0;
}

/** Solve for integers j, k and g=gcd(a,b) such that ja + ky = g,
 * where a,b are integer constants.
 * This is a linear equation in integers, and is solved
 * via the extended Euclid algorithm.
 */
static void inline extendedEuclid( int& k, int a, int& j, int b, int& g)
{
  int x = 1, y = 0;
  int xLast = 0, yLast = 1;
  int q, r, m, n;
  while (a != 0) 
  {
    q = b / a;
    r = b % a;
    m = xLast - q * x;
    n = yLast - q * y;
    xLast = x; 
    yLast = y;
    x = m; 
    y = n;
    b = a; 
    a = r;
  }
  g = b;
  k = xLast;
  j = yLast;
}

/** hoist `A+iB in range [C,D)` condition out of a loop for i in [imin,imax].
 * When 
 * Original:
 * \code
 * for(i=imin; i<imax; ++i){       // original loop
 *   int const ApiB = a + i*b;      // linear fn, ( b>=0 ? )
 *   if( ApiB < c || ApiB > d ) continue;
 *   // Loop Body
 * }
 * \endcode
 * Transformed:
 * \code
 * int const ibeg, iend;
 * hoist_ApiB_in( ibeg, iend, imin,imax, a,b, c,d );
 * for(i=ibeg; i<iend; ++i){       // original loop
 *   int const ApiB = a + i*b;
 *   // GONE: if( ApiB < c || ApiB > d ) continue;
 *   // Loop Body
 * }
 * \endcode
 * \pre \c b > 0, (c, d?)
 */
static inline void hoist_ApiB_in( int& beg, int& end,
        const int imin, const int imax,
        const int a, const int b, const int c, const int d)
{
    DMUST( b > 0 );
    // int i*B < A    iff    i < (A    )/B
    // int i*B > A    iff    i > (A+B-1)/A
    // A+iB >= c ... iB >= c-A  ... i >= (c-A + B-1 )/B
#if 1
    beg = div_floor( c-a+b-1, b );
#else
    beg = c-a + b-1;
    if( beg >= 0 ){
        beg /= b;
    } else {
        int const fmul=(-beg + b)/b;
        RT_ASSERT( beg + fmul*b >= 0 );
        beg = (beg + fmul*b) / b - fmul;
    }
#endif
    //print(0, "i in [%d,%d), lin(a,b)=%d+i*%d in [c,d]=[%d,%d), beg=%d? f+c-a+b-1=%d\n",
    //        imin,imax, a,b, c,d, beg, f+c-a+b-1);
    DMUST( a + (beg-1)*b < c );
    DMUST( a + (beg  )*b >= c );
    if( beg < imin ) beg = imin;

    // A+iB < d ... iB < d-A    ... i < (d-A) / B
#if 1
    end = div_floor( d-a+b-1, b );
#else
    end = d-a +b-1;
    if( end >= 0 ){
        end /= b;
    } else {
        int const fmul=(-end + b)/b;
        RT_ASSERT( end + fmul*b >= 0 );
        end = (end + fmul*b) / b - fmul;
    }
#endif
    //print(0, "i in [%d,%d), lin(a,b)=%d+i*%d in [c,d]=[%d,%d), end=%d? f+d-a=%d\n",
    //        imin,imax, a,b, c,d, end, f+d-a);
    DMUST( a + (end-1)*b < d );
    DMUST( a + (end  )*b >= d );
    if( end > imax ) end = imax;
}

/** Integer \c i so A-iB is <em>just below</em> \c target.
 * \pre \c B>0 (unchecked). */
static inline int AmiB_lt_target( const int a, const int b, const int target)
{
    int ibelow = a-target + b;
    // XXX use idiv.hpp here too
    if( ibelow >= 0 ){
        ibelow /= b;
        //ibelow = div_floor( ibelow, b );
    } else {
        int const fmul=(-ibelow + b)/b;
        //RT_ASSERT( ibelow + fmul*b >= 0 );
        //RT_ASSERT( fmul == div_floor( -ibelow, b )+1 );
        ibelow = (ibelow + fmul                    *b) / b - fmul; // orig
        //ibelow = (ibelow + (div_floor(-ibelow,b)+1)*b) / b - (div_floor(-ibelow,b)+1);
        //ibelow = (ibelow + div_floor(-ibelow,b)* b) / b - div_floor(-ibelow,b);
        //ibelow = div_floor( ibelow, b );
        //ibelow = (ibelow + div_floor(-ibelow,b)* b) / b - div_floor(-ibelow,b);
    }
    DMUST( a - (ibelow-1)*b >= target );
    DMUST( a - (ibelow  )*b <  target );
    return ibelow;
}
/** Get range if \c i so A-iB is in [c,d), and then further
 * restrict to range [imin,imax).  Note that \c -B means as \c i
 * increases, we move from \c d-1 downwards to \c c. */
static inline void hoist_AmiB_in( int& beg, int& end,
        const int imin, const int imax,
        const int a, const int b, const int c, const int d)
{
    DMUST( b > 0 );
    beg = AmiB_lt_target( a, b, d );
    //RT_ASSERT( beg == div_floor( a-d+b, b) );
    //beg = div_floor( a-d+b, b); // possibly slower ?? do I have a cmov here? no!
    //beg = div_floor( a-d, b) + 1; // possibly slower ?? do I have a cmov here? no!
    if( beg < imin ) beg = imin;

    end = AmiB_lt_target( a, b, c );
    //RT_ASSERT( end == div_floor( a-c+b, b) );
    //end = div_floor( a-c+b, b);
    //end = div_floor( a-c, b) + 1;
    if( end > imax ) end = imax;
}

/** shows a new hoisting method.
 *
 * Hoisting conditionals refers to replacing conditionals with
 * formulas for loop limits.  A simple example can be found in
 * \ref hoist_ApiB_in, which uses the following examples:
 *
 * Original:
 * \code
 * for(i=imin; i<imax; ++i){       // original loop
 *   int const ApiB = a + i*b;      // linear fn, ( b>=0 ? )
 *   if( ApiB < c || ApiB > d ) continue;
 *   // Loop Body
 * }
 * \endcode
 *
 * Transformed:
 * \code
 * int const ibeg, iend;
 * hoist_ApiB_in( ibeg, iend, imin,imax, a,b, c,d );
 * for(i=ibeg; i<iend; ++i){       // original loop
 *   int const ApiB = a + i*b;
 *   // GONE: if( ApiB < c || ApiB > d ) continue;
 *   // Loop Body
 * }
 * \endcode
 *
 * For \c kh, for examples we replaced and simplified the \em hoist routine
 * in steps:
 *
 * \ref ref_conv2.cpp
 * \code
 *  for (int kh = 0; kh < p->kh; ++kh) {
 *      const int ih = oh * p->sh - p->ph + kh * (p->dh + 1);
 *      if (ih < 0 || ih >= p->ih) continue;
 *      // etc
 * \endcode
 * With hoisting, \ref refconv3_fwd
 * \code   
 * int kh_beg, kh_end;
 * hoist_ApiB_in( kh_beg, kh_end,
 *                0, p->kh                          // i  in  [0, p->kh)
 *               (oh * p->sh - p->ph), (p->dh + 1), // ih=A+iB
 *               0, p->ih);                         // ih in [0, p->ih)
 * //if (kh_beg >= kh_end) continue;
 * for (int kh = kh_beg; kh < kh_end; ++kh) {
 *     const int ih = oh * p->sh - p->ph + kh * (p->dh + 1);
 *     // etc
 * \endcode
 * which simplifies to
 * \code
 * kh_beg = div_floor( 0     - (oh * SH - PH) + p->dh, (p->dh+1) );
 * kh_end = div_floor( IH - (oh * SH - PH) + p->dh, (p->dh+1) );
 * if( kh_beg < 0     ) kh_beg = 0;
 * if( kh_end > p->kh ) kh_end = p->kh;
 * //if (kh_beg >= kh_end) continue;
 * for (int kh = kh_beg; kh < kh_end; ++kh) {
 *     const int ih = oh * p->sh - p->ph + kh * (p->dh + 1);
 *     // etc
 * \endcode
 *
 * A key feature for the mathematics of such formulas is that integer
 * rounding should go toward negative infinity.  Unfortunately, C99
 * and C++11 round toward zero.  So div_floor, which has been optimized
 * for x86, may have conditionals that don't behave so well for other
 * chips.
 *
 * So here we derive a way to do all calculations with positive integers,
 * avoiding negatives.  Such a calculation is now correct when evaluated
 * with unsigned integers, and also allows normal division to be used.
 *
 * - First, solve for \c kh, assuming div_floor rounding.
 *   - ih = oh * p->sh - p->ph + kh * (p->dh + 1)
 *   - \f$ih = oh*SH - PH + kh*DH\f$
 *   - \f$kh*DH = ih + PH - oh*SH\f$
 *   - \f$kh = div\_floor( ih+PH-oh*SH+DH-1, DH )\f$, which we'll loosely call
 *   - \f$kh(ih,oh) = (ih+DH-1+PH-oh*SH) / DH\f$ (true for positive numerator & denominator)
 *
 * - \f$kh_{beg}\f$, the lowest \c kh value, is associated with the lowest possible \c oh.
 *   - We avoid testing for values of zero, since division values of zero can
 *     result by rounding negative integers upward
 * - Consider \f$kh(ih,oh) >= 1\f$
 *   - \f$ (0 + DH-1+PH-oh*SH) / DH >= 1\f$  (for +ve numerator & denominator)
 *   - \f$ DH-1+PH-oh*SH >= DH\f$
 *   - \f$ PH-1 - oh*SH >= 0\f$
 *   - \f$ oh*SH <= PH-1 \f$
 *   - \f$ oh*SH < PH \f$
 * - Therefore if \f$oh*SH < PH\f$, we use the formula \f$kh_{beg}=(DH-1+[PH-oh*SH]) / DH\f$
 *   - Notice that both numerator and denominator are both strictly positive.
 *   - So this formula is correct for signed/unsigned integers.
 * - Otherwise, \f$kh_{beg} = 0\f$, the lowest possible value.
 * - We'll avoid \f$kh_{beg} > KH\f$, by testing \f$kh_{beg} < kh_{end}\f$
 *
 *
 * - Now Consider \f$kh_{end} >= KH\f$, where KH is the highest valid value for \f$kh_{end}\f$
 *   - The largest \f$kh\f$ occurs when \c ih has it's largest possible value, \c IH.
 * - Let's first check for \f$kh(ih,oh) >= KH\f$
 *   - \f$ (IH + DH-1 + PH - oh*SH) / DH >= KH \f$
 *   - \f$ IH + DH - 1 + PH - oh*SH  >= KH*DH \f$
 *   - \f$ KH*DH + oh*SH + 1 <= IH+PH+DH \f$, now RHS and LHS are positive
 *   - \f$ KH*DH + oh*SH < IH+PH+DH \f$
 *   - When the above condition holds, we can set \f$kh_{end} = KH\f$ (maximal value)
 * - Otherwise we can also check for \f$kh(ih,oh) >= 1\f$, so that we can safely use
 *   division with positive integers.
 *   - Replacing 'KH' with '1' in above...  \f$ 1*DH + oh*SH < IH+PH+DH \f$
 *   - So when \f$oh*SH < IH+PH\f$, \f$kh_{end} =  (DH-1 + [IH+PH - oh*SH]) / DH \f$
 *     will be \f$ > 0\f$
 *     - Otherwise we can set \f$kh_{end} = 0\f$
 *
 * So the \em long-hand postive-integer solutions for \c kh_beg and \c kh_end are:
 *
 * \code
 * if( oh*SH < PH )
 *   kh_beg = (p->dh + (PH - oh * SH)) / DH;
 * else
 *   kh_beg = 0;
 * \endcode
 * and
 * \code
 * if (oh*SH + KH*DH < IH + PH + DH)
 *   kh_end = KH;
 * else if (oh*SH >= IH+PH)
 *   kh_end = 0;
 * else
 *   kh_end = ([IH+PH - oh*SH] + DH-1) / DH;
 * \endcode
 *
 * \ref sxconv_4_fwd shows how this can be done, and results in big speedups for sxc++,
 * whose compiler can vectorize the few remaining simple conditionals quite well (apparently).
 *
 * Other SX optimizations include using unit-stride temporaries, since complex expressions
 * with multiple strided vectors are sometimes not vectorized very well.
 */
void sxconv_4_fwd(const prb_t *p, dnn_mem_t &src_m,
        dnn_mem_t &wei_m, dnn_mem_t &bia_m, dnn_mem_t &dst_m) {
  // V 8 is default
#define V 9
#if V==8 // A1, A3 : 74, 108x
  const ssize_t G = p->g;
  const ssize_t MB = p->mb;
  const ssize_t IC = p->ic;
  const ssize_t IH = p->ih;
  const ssize_t IW = p->iw;
  const ssize_t OC = p->oc;
  const ssize_t OH = p->oh;
  const ssize_t OW = p->ow;
  const ssize_t KH = p->kh;
  const ssize_t KW = p->kw;
  const ssize_t PH = p->ph;
  const ssize_t PW = p->pw;
  const ssize_t SH = p->sh;
  const ssize_t SW = p->sw;
  const ssize_t DH = p->dh + 1;
  const ssize_t DW = p->dw + 1;

  const ssize_t ICOG = IC/G;
  const ssize_t OCOG = OC/G;
  const ssize_t KH_KW = KH * KW;
  const ssize_t ICOG_KH_KW = ICOG * KH_KW;
  const ssize_t OH_OW = OH * OW;
  const ssize_t OCOG_ICOG_KH_KW = OCOG * ICOG_KH_KW;
  const ssize_t OCOG_ICOG_KH = OCOG * ICOG * KH;
  const ssize_t IH_IW = IH * IW;
  const ssize_t SH_IW = SH * IW;
  const ssize_t DH_IW = DH * IW;

  MUST( p->kh > 0 && KW > 0 );
  MUST( p->dh >= 0 && p->dw >= 0 );
  MUST( SH >= 0 && SW >= 0 );
  float const * restrict const psrc = (float*)src_m;
  float const * restrict const pwei = (float*)wei_m;
  float const * restrict const pbia = (float*)bia_m;
  float       * restrict const pdst = (float*)dst_m;
  OMP(parallel)//;
  {
    ssize_t khkw_begend[4] alignas(4*sizeof(ssize_t));
    ssize_t kh_beg=0, kh_end=0;
    ssize_t kw_beg=0, kw_end=0;
#ifdef __ve
    VREG(khkw_begend)
#else
        VREG(khkw_begend)
#endif
    ssize_t khkw_muls[4] alignas(4*sizeof(ssize_t)) = {1, KH, (1<<16), (1<<16)*KW};
#ifdef __ve
    VREG(khkw_muls)
#else
        VREG(khkw_muls)
#endif
    //ssize_t kh_beg_prv=0, kh_end_prv=0, kw_beg_prv=0, kw_end_prv=0, w0_prv=0;
    ssize_t khash, w0, s0, s00;
    ssize_t khash_prv = ~0;
    //ssize_t khash_prv2 = (KH+KW)*4; // impossibly high hash
    bool kok[KH][KW] alignas(128);
#ifdef __ve
    VREG(kok)
#else
        VREG(kok)
#endif
    float src[ICOG*KH*KW] alignas(128);
#ifdef __ve
    VREG(src)
#else
    ALLOC_ON_VREG(src)
#endif
    float tmp[OCOG] alignas(128);
//RETAIN(tmp)
#ifdef __ve
    VREG(tmp)
        VREG(OCOG)
#else
    ALLOC_ON_VREG(tmp,OCOG) // roughly double the speed
#endif
    OMP(for collapse(4))//;
  for (ssize_t g = 0; g < G; ++g) {
    for (ssize_t mb = 0; mb < MB; ++mb) {
      for (ssize_t oh = 0; oh < OH; ++oh) {
        for (ssize_t ow = 0; ow < OW; ++ow) {
          // ---- 1 omp thread ----
          if ((p->dir & FLAG_BIA) == 0){
            for (ssize_t oc = 0; oc < OCOG; ++oc)
              tmp[oc] = 0.f;
          }else{
            ssize_t bia_off0 = (ssize_t)g * OCOG + 0;
            for (ssize_t oc = 0; oc < OCOG; ++oc)
              tmp[oc] = pbia[bia_off0 + oc];
          }
          // this alt to div_floor avoids negatives.
          // ... so it is correct for unsigned types AND normal div.
          kh_beg=0, kh_end=0;
          if (oh*SH < PH) kh_beg = (PH - oh*SH + (DH - 1)) / DH;
          kh_end = 0;
          if (oh*SH < IH+PH) kh_end = (IH+PH - oh*SH + (DH - 1)) / DH;
          if (kh_end >= KH) kh_end = KH;

          kw_beg=0, kw_end=0;
          if (ow*SW < PW) kw_beg = (PW - ow*SW + (DW - 1)) / DW;
          if (ow*SW < IW+PW) kw_end = (IW+PW - ow*SW + (DW - 1)) / DW;
          if (kw_end >= KW) kw_end = KW;

          bool const khw_ok = ( kw_beg < kw_end && kh_beg < kh_end );
          if (khw_ok)
          {
            //unsigned khash = ((kw_beg+kh_beg) | ((kw_end+kh_end) - (KH+KW)) | khash_prv);
            // --> FAIL. ?better hash needed?
            khkw_begend[0] = kh_beg;
            khkw_begend[1] = kh_end;
            khkw_begend[2] = kw_beg;
            khkw_begend[3] = kw_end;
            khash = 0;
            for (size_t i=0; i<4; ++i)
              khash += khkw_begend[i] * khkw_muls[i];
            if (khash != khash_prv){
              khash_prv = khash;
              ShortLoop() for (ssize_t kh = 0; kh < KH; ++kh) {
                ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
                  kok[kh][kw] = (kh>=kh_beg && kw>=kw_beg) && (kh<kh_end && kw<kw_end);
                }
              }
            }
            const ssize_t w0 = (((g * OCOG + 0 ) * ICOG + 0) * KH + 0) * KW + 0; // oc,ic,kh,kw=0
            const ssize_t s0 = ((mb * IC + g * ICOG + 0 ) * IH + (oh*SH-PH+0*DH)) * IW + (ow*SW-PW+0*DW); //ic,kh,kw=0
            // slower for (ssize_t ic = 0, ickhkw=0; ic < ICOG; ++ickhkw, ++ic)
            for (ssize_t ic = 0; ic < ICOG; ++ic)
            {
              ShortLoop() for (ssize_t kh = 0; kh < KH; ++kh) {
                ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
                  src[ic*KH*KW + kh*KW + kw] = (kok[kh][kw]? psrc[s0 + ic*IH*IW + kh*DH*IW + kw*DW]: 0.f);
                }
              }
            }

            // this may fail if wei access also needs to be protected.
            for (ssize_t oc = 0; oc < OCOG; ++oc) {
              for (ssize_t ickhkw = 0; ickhkw < ICOG_KH_KW; ++ickhkw) {
                tmp[oc] += src[ickhkw] * pwei[w0 + ickhkw + oc * ICOG*KH*KW];
              }
            }
          }
          //for (size_t oc = 0; oc < OCOG; ++oc) pdst[dst_off0 + oc * OH_OW] = tmp[oc];
          if (p->merge == RELU) {
            for (size_t oc = 0; oc < OCOG; ++oc)
              tmp[oc] = (tmp[oc] < 0.f? 0.f: tmp[oc]);
          }
          const ssize_t dst_off0 = (((ssize_t)mb * OC + g * OCOG + 0) * OH + oh) * OW + ow;
          for (size_t oc = 0; oc < OCOG; ++oc)
            pdst[dst_off0 + oc * OH_OW] = tmp[oc];
        }
      }
    }
  }
#elif V==9 // A1, A3 : 74, 108x
  const ssize_t G = p->g;
  const ssize_t MB = p->mb;
  const ssize_t IC = p->ic;
  const ssize_t IH = p->ih;
  const ssize_t IW = p->iw;
  const ssize_t OC = p->oc;
  const ssize_t OH = p->oh;
  const ssize_t OW = p->ow;
  const ssize_t KH = p->kh;
  const ssize_t KW = p->kw;
  const ssize_t PH = p->ph;
  const ssize_t PW = p->pw;
  const ssize_t SH = p->sh;
  const ssize_t SW = p->sw;
  const ssize_t DH = p->dh + 1;
  const ssize_t DW = p->dw + 1;

  const ssize_t ICOG = IC/G;
  const ssize_t OCOG = OC/G;
  const ssize_t KH_KW = KH * KW;
  const ssize_t ICOG_KH_KW = ICOG * KH_KW;
  const ssize_t OH_OW = OH * OW;
  const ssize_t OCOG_ICOG_KH_KW = OCOG * ICOG_KH_KW;
  const ssize_t OCOG_ICOG_KH = OCOG * ICOG * KH;
  const ssize_t IH_IW = IH * IW;
  const ssize_t SH_IW = SH * IW;
  const ssize_t DH_IW = DH * IW;

  MUST( p->kh > 0 && KW > 0 );
  MUST( p->dh >= 0 && p->dw >= 0 );
  MUST( SH >= 0 && SW >= 0 );
  float const * restrict const psrc = (float*)src_m;
  float const * restrict const pwei = (float*)wei_m;
  float const * restrict const pbia = (float*)bia_m;
  float       * restrict const pdst = (float*)dst_m;
  const int khkw_end = (int)KH_KW;
  float khkw_zeros[khkw_end];
  for (int khkw=0; khkw < khkw_end; ++khkw){
      khkw_zeros[khkw] = 0.f;
  }
  float const * restrict const pkhkw_zeros = &khkw_zeros[0];
  OMP(parallel)//;
  {
    ssize_t khkw_begend[4];
    ssize_t kh_beg=0, kh_end=0;
    ssize_t kw_beg=0, kw_end=0;
#ifdef __ve
    VREG(khkw_begend)
#else
        VREG(khkw_begend)
#endif
    ssize_t khkw_muls[4] = {1, KH, (1<<16), (1<<16)*KW};
#ifdef __ve
    VREG(khkw_muls)
#else
        VREG(khkw_muls)
#endif
    //ssize_t kh_beg_prv=0, kh_end_prv=0, kw_beg_prv=0, kw_end_prv=0, w0_prv=0;
    ssize_t khash, w0, s0, s00;
    ssize_t khash_prv = ~0;
    //ssize_t khash_prv2 = (KH+KW)*4; // impossibly high hash
    //bool kok[KH][KW];
    int kok[KH_KW] alignas(128);
#ifdef __ve
    VREG(kok)
#else
        VREG(kok)
#endif
    float src[ICOG*KH*KW] alignas(128);
#ifdef __ve
    VREG(src)
#else
    ALLOC_ON_VREG(src)
#endif
    float tmp[OCOG] alignas(128);
    float seq[KH_KW] alignas(128);
    for(int i=0; i<KH_KW; ++i) seq[KH_KW] = (float)i;
//RETAIN(tmp)
#ifdef __ve
    VREG(tmp)
        VREG(OCOG)
#else
    ALLOC_ON_VREG(tmp,OCOG) // roughly double the speed
#endif

    OMP(for collapse(4))//;
  for (ssize_t g = 0; g < G; ++g) {
    for (ssize_t mb = 0; mb < MB; ++mb) {
      for (ssize_t oh = 0; oh < OH; ++oh) {
        for (ssize_t ow = 0; ow < OW; ++ow) {
          // ---- 1 omp thread ----
          if ((p->dir & FLAG_BIA) == 0){
            for (ssize_t oc = 0; oc < OCOG; ++oc)
              tmp[oc] = 0.f;
          }else{
            ssize_t bia_off0 = (ssize_t)g * OCOG + 0;
            for (ssize_t oc = 0; oc < OCOG; ++oc)
              tmp[oc] = pbia[bia_off0 + oc];
          }
          // this alt to div_floor avoids negatives.
          // ... so it is correct for unsigned types AND normal div.
          kh_beg=0, kh_end=0;
          if (oh*SH < PH) kh_beg = (PH - oh*SH + (DH - 1)) / DH;
          kh_end = 0;
          if (oh*SH < IH+PH) kh_end = (IH+PH - oh*SH + (DH - 1)) / DH;
          if (kh_end >= KH) kh_end = KH;

          kw_beg=0, kw_end=0;
          if (ow*SW < PW) kw_beg = (PW - ow*SW + (DW - 1)) / DW;
          if (ow*SW < IW+PW) kw_end = (IW+PW - ow*SW + (DW - 1)) / DW;
          if (kw_end >= KW) kw_end = KW;

          bool const khw_ok = ( kw_beg < kw_end && kh_beg < kh_end );
          if (khw_ok)
          {
            //unsigned khash = ((kw_beg+kh_beg) | ((kw_end+kh_end) - (KH+KW)) | khash_prv);
            // --> FAIL. ?better hash needed?
            khkw_begend[0] = kh_beg;
            khkw_begend[1] = kh_end;
            khkw_begend[2] = kw_beg;
            khkw_begend[3] = kw_end;
            khash = 0;
            for (size_t i=0; i<4; ++i)
              khash += khkw_begend[i] * khkw_muls[i];
            if (khash != khash_prv){
              khash_prv = khash;
#if 1
              int khkw_end = (int)KH_KW;
              //OMP(simd)//;
              IVDEP() for (int khkw = 0; khkw < khkw_end; ++khkw) { // this does not simd-ize very well.
                const int kh = khkw / p->kw;
                const int kw = khkw % p->kw;
                kok[khkw] = ((kh>=kh_beg && kw>=kw_beg) && (kh<kh_end && kw<kw_end)? 0xffFFffFF: 0);
                //kok[khkw] = ((kh>=kh_beg && kw>=kw_beg) && (kh<kh_end && kw<kw_end)? 0xffFFffFF: 0);
                //kok[khkw] = (kh>=kh_beg ? ~0: 0)
                //    &       (kw>=kw_beg ? ~0: 0)
                //    &       (kh<kh_end  ? ~0: 0)
                //    &       (kw<kw_end  ? ~0: 0)
                //    ;
              }
#else
              int khkw_end = (int)KH_KW;
              const int khh = (int)KH;
              const int kww = (int)KW;
              for (int khkw = 0; khkw < khkw_end; ++khkw) kok[khkw] = 0;
              bool alignas(128) khok[khh]; for(int i=0; i<khh; ++i) khok[i] = (i>=kh_beg? ~0: 0) & (i<kh_end? ~0: 0);
              bool alignas(128) kwok[kww]; for(int i=0; i<kww; ++i) kwok[i] = (i>=kw_beg? ~0: 0) & (i<kw_end? ~0: 0);
              for (ssize_t kh = 0; kh < khh; ++kh) {
                for (ssize_t kw = 0; kw < kww; ++kw) {
                  kok[kh*kww+kw] = khok[kh];
                }
                for (ssize_t kw = 0; kw < kww; ++kw) {
                  kok[kh*kww+kw] &= kwok[kw];
                }
              }
#endif
            }
            const ssize_t w0 = (((g * OCOG + 0 ) * ICOG + 0) * KH + 0) * KW + 0; // oc,ic,kh,kw=0
            const ssize_t s0 = ((mb * IC + g * ICOG + 0 ) * IH + (oh*SH-PH+0*DH)) * IW + (ow*SW-PW+0*DW); //ic,kh,kw=0
            // slower for (ssize_t ic = 0, ickhkw=0; ic < ICOG; ++ickhkw, ++ic)
            for (ssize_t ic = 0; ic < ICOG; ++ic)
            {
#if 0
              for (int khkw = 0; khkw < khkw_end; ++khkw) {
                //const int kh = khkw / p->kw;
                //const int kw = khkw % p->kw;
                //src[ic*KH*KW + khkw] = (kok[khkw]? psrc[s0 + ic*IH*IW + kh*DH*IW + kw*DW]: 0.f);
                //src[ic*KH*KW + khkw] = (kok[khkw]? psrc[s0 + ic*IH*IW + kh*DH*IW + kw*DW]: 0.f);
                src[ic*KH*KW + khkw] = ((kok[khkw] & (1<<31))? psrc[s0 + ic*IH_IW + (khkw/p->kw)*DH_IW + (khkw%p->kw)*DW]: 0.f);
              }
#elif 1
              float idx[khkw_end] alignas(128);
              //for (int khkw = 0; khkw < khkw_end; ++khkw) {
              const int khh = KH;
              const int kww = KW;
              for (int kh=0; kh<khh; ++kh){
                for (int kw=0; kw<kww; ++kw){
                  // using integer calc no longer results in vgather !
                  //idx[kh*kww+kw] = s0 + ic*IH_IW + kh*DH_IW + kw*DW;
                  idx[kh*kww+kw] = (float)s0 + (float)ic*IH*IW + (float)kh*DH*IW + (float)kw*DW;
                  //src[ic*khh*kww + kh*kww+kw] = 0.f;
                  //if (kok[kh*kww+kw]) src[ic*khh*kww + kh*kww+kw] += psrc[(int)(idx[kh*kww+kw])];
                  src[ic*khh*kww + kh*kww+kw] = ((kok[kh*kww+kw])? psrc[(int)(idx[kh*kww+kw])]: 0.f);
                  //src[ic*khh*kww + kh*kww+kw] = ((kok[kh*kww+kw])? psrc[(int)(idx[kh*kww+kw])]: kok[kh*kww+kw]);
                }
              }
#else
              float idx[khkw_end] alignas(128);
              const int khh = KH;
              const int kww = KW;
              for (int kh=0; kh<khh; ++kh){
                for (int kw=0; kw<kww; ++kw){
                  idx[kh*kww+kw] = (float)s0 + (float)ic*IH*IW + (float)kh*DH*IW + (float)kw*DW;
                }
              }
              for (int khkw=0; khkw < khkw_end; ++khkw){
                // AHAAAAAA  this is the magic to generate vmaskmovps and vgatherdps ... but it is no faster :)
                src[ic*khkw_end + khkw] = ((kok[khkw] & (1<<31))? psrc[(int)(idx[khkw])]: 0.f);
                //
                //src[ic*khh*kww + khkw] = 0.f;
                //if (kok[khkw]) src[ic*khh*kww + khkw] += psrc[(int)(idx[khkw])];
                //  not nice
                //if (kok[khkw]) src[ic*khh*kww + khkw] = psrc[(int)(idx[khkw])];
                //else           src[ic*khh*kww + khkw] = 0.f;
                //src[ic*khh*kww + khkw] = (kok[khkw]? psrc[(int)(idx[khkw])]: pkhkw_zeros[khkw]);
              }
#endif
            }

            // this may fail if wei access also needs to be protected.
            //float const * restrict pwei_oc = pwei + w0;
            //for (ssize_t oc = 0; oc < OCOG; pwei_oc+=ICOG_KH_KW, ++oc)
            for (ssize_t oc = 0; oc < OCOG; ++oc) {
#if 1
              OMP(simd)//;
              for (ssize_t ickhkw = 0; ickhkw < ICOG_KH_KW; ++ickhkw) {
                tmp[oc] += src[ickhkw] * pwei[w0 + ickhkw + oc * ICOG_KH_KW];
                //tmp[oc] += src[ickhkw] * pwei_oc[ickhkw];
              }
#else
              float x = 0.f;
              for (ssize_t ickhkw = 0; ickhkw < ICOG_KH_KW-16+1; ickhkw+=16) {
                OMP(simd)//;
                for (ssize_t i = ickhkw; i < ickhkw+16; ++i) {
                  x += src[i] * pwei_oc[i];
                }
                for (ssize_t i = ickhkw; i < ICOG_KH_KW; ++i) {
                  x += src[i] * pwei_oc[i];
                }
                tmp[oc] = x;
              }
#endif
            }
          }
          //for (size_t oc = 0; oc < OCOG; ++oc) pdst[dst_off0 + oc * OH_OW] = tmp[oc];
          if (p->merge == RELU) {
            for (size_t oc = 0; oc < OCOG; ++oc)
              tmp[oc] = (tmp[oc] < 0.f? 0.f: tmp[oc]);
          }
          const ssize_t dst_off0 = (((ssize_t)mb * OC + g * OCOG + 0) * OH + oh) * OW + ow;
          for (size_t oc = 0; oc < OCOG; ++oc)
            pdst[dst_off0 + oc * OH_OW] = tmp[oc];
        }
      }
    }
  }
#else
#error "please select one"
#endif
  }
}

/** hoisting for generic dilations is complicated by modulo conditions. */
static void sxconv_4_bwd_d_generic(const prb_t *p, dnn_mem_t &diff_src_m,
        dnn_mem_t &wei_m, dnn_mem_t &diff_dst_m)
{
#if defined(__ve) // compiler bug! XXX TODO temporarily disabled
  refconv_2_bwd_d(p, diff_src_m, wei_m, diff_dst_m);
#elif 0 // shorten, do same for kw,ow loop. tweaks to kh calc
  int const KH = p->kh;
  int const OH = OH;
  int const DH = p->dh + 1;
  int const SH = SH;
  int const PH = PH;
  const int gcd_h = gcd( SH, DH );
  const int lcm_h = SH * DH/gcd_h; //lcm( SH, DH ) = SH*DH / gcd(SH,DH)
  const int khh = lcm_h / DH;
  DMUST( khh == SH / gcd(SH,DH) );
  const int jhh = lcm_h / SH;
  int ha, hb, hg;
  extendedEuclid( ha, DH, hb, SH, hg);
  //print(0," extendedEuclid: %d * [DH=%d] + %d * [SH=%d] = %d[gcd(DH,SH)]\n", ha,DH, hb,SH, hg);
  DMUST( hg == gcd_h );

  int const KW = p->kh;
  int const DW = p->dw + 1;
  int const SW = SW;
  int const OW = OW;
  int const PW = PW;
  const int gcd_w = gcd( SW, DW );
  //const int lcm_w = lcm( SW, DW );
  const int lcm_w = SW * DW/gcd_w;
  const int kww = lcm_w / DW;
  const int jww = lcm_w / SW;
  int wa, wb, wg;
  extendedEuclid( wa, DW, wb, SW, wg);
  DMUST( wg == gcd_w );
  OMP(parallel for collapse(5))//;
  for (int g = 0; g < G; ++g) {
    for (int mb = 0; mb < MB; ++mb) {
      for (int ic = 0; ic < IC/G; ++ic) {
        for (int ih = 0; ih < IH; ++ih) {
          // calc kh_beg, kh_end here? 4.28x
          for (int iw = 0; iw < IW; ++iw) {
            size_t src_off = src_off_f(p, mb, g, ic, ih, iw);
            float &ds = ((float*)diff_src_m)[src_off];
            ds = 0;

            int kh_end = (ih + PH) / DH + 1;
            if (kh_end > KH) kh_end = KH;
            int kh_beg = kh_end;
            if( (ih+PH) % gcd_h == 0 ){ // Do solutions exist?
              // 1. Find one soln (any)
              int const mul = (ih+PH) / gcd_h;
              kh_beg = ha*mul;
              int j = hb*mul;
              // 2. Adjust to lowest kh_beg>=0
#if 0 // ok
              int m = (kh_beg<0? (-kh_beg+ khh -1) / khh
                  : kh_beg >= khh? - (kh_beg / khh)
                  : 0);
              RT_ASSERT( kh_beg + m*khh == rem_floor( kh_beg, khh ) ); // adjust kh_beg in khh-steps
              RT_ASSERT( m == (rem_floor(kh_beg,khh) - kh_beg) / khh ); // y
              RT_ASSERT( m == - div_floor(kh_beg,khh) ); // y
              if(m){
                  kh_beg += m * khh;
                  j -= m * jhh;
              }
#elif 0 // ok
              int k2 = rem_floor( kh_beg, khh );
              int m = (k2-kh_beg) / khh;
              kh_beg = k2;
              j -= m * jhh;
#elif 1 // ok
              int m = div_floor(kh_beg, khh);
              kh_beg -= m * khh;
              j      += m * jhh;
#endif
              // 3. Adjust j downwards st. j*SH < OH*SH (kh_beg can go up even more)
              if( j >= OH ){
                m = (j-OH) / jhh + 1;
                kh_beg += m * khh;
                //j      -= m * jhh; // not needed
              }
            }
            if( kh_beg >= kh_end ) continue;
#if 0
            int kw_beg, /*ow_beg,*/ kw_end;
            kw_end = (iw + PW) / (p->dw+1) + 1;
            if (kw_end > KW) kw_end = KW;
            kw_beg = div_floor( (iw + PW) - OW*SW, p->dw+1) + 1;
            if (kw_beg < 0    ) kw_beg = 0;
            { // jump kw_beg up to 1st non-skipped index
              int owxsw = iw+PW - kw_beg * (p->dw+1);
              if (owxsw % SW){
                do {
                  ++kw_beg;
                  owxsw = iw+PW - kw_beg * (p->dw+1);
                } while( owxsw % SW != 0 && kw_beg < kw_end );
                DMUST( kw_beg >= kw_end || (iw+PW - kw_beg * (p->dw+1)) % SW == 0 );
              }
            }
            DMUST( kw_beg >= 0 );
#else
            int kw_end = (iw + PW) / DW + 1;
            if (kw_end > KW) kw_end = KW;
            int kw_beg = kw_end;
            if( (iw+PW) % gcd_w == 0 ){ // Do solutions exist?
              int const mul = (iw+PW) / gcd_w;
              kw_beg = wa*mul;
              //int j = wb*mul;
              int m = div_floor(kw_beg, kww);
              kw_beg -= m * kww;
              int j = wb * mul + m * jww;
              if( j >= OW ){
                m = (j-OW) / jww + 1;
                kw_beg += m * kww;
                //j -= m * jww; // not needed
              }
            }
#endif
            if( kw_beg >= kw_end ) continue;

            for (int oc = 0; oc < OC/G; ++oc) {
              for (int kh = kh_beg, oh0=ih+PH - kh_beg*(p->dh+1) ;
                  kh < kh_end;
                  oh0 -= lcm_h, kh += khh)
              {
                for (int kw = kw_beg, ow0=iw+PW - kw_beg*(p->dw+1) ;
                    kw < kw_end;
                    ow0 -= lcm_w, kw += kww)
                {
                  size_t dst_off = dst_off_f(p, mb, g, oc, oh0/SH, ow0/SW);
                  size_t wei_off = wei_off_f(p, g, oc, ic, kh, kw);
                  ds += ((float*)diff_dst_m)[dst_off]
                    * ((float*)wei_m)[wei_off];
                }
              }
            }

          }
        }
      }
    }
  }
#elif 1 // clean up
  float       * restrict const pdiff_src = (float*)diff_src_m;
  float const * restrict const pwei = (float*)wei_m;
  //float const * restrict const pbia = (float*)bia_m;
  float const * restrict const pdiff_dst = (float*)diff_dst_m;
#define G  p->g
#define MB p->mb
#define IC p->ic
#define OC p->oc
#define KH p->kh
#define IH p->ih
#define OH p->oh
#define SH p->sh
#define PH p->ph
#define KW p->kw
#define IW p->iw
#define OW p->ow
#define SW p->sw
#define PW p->pw
  //int const KH = p->kh;
  //int const OH = OH;
  //int const DH = p->dh + 1;
  //int const SH = SH;
  //int const PH = PH;
  size_t const DH = p->dh + 1;
  const int gcd_h = gcd( SH, DH );
  const int lcm_h = SH * DH/gcd_h; //lcm( SH, DH ) = SH*DH / gcd(SH,DH)
  const int khh = lcm_h / DH;
  DMUST( khh == SH / gcd(SH,DH) );
  const int jhh = lcm_h / SH;
  int ha, hb, hg;
  extendedEuclid( ha, DH, hb, SH, hg);
  //print(0," extendedEuclid: %d * [DH=%d] + %d * [SH=%d] = %d[gcd(DH,SH)]\n", ha,DH, hb,SH, hg);
  DMUST( hg == gcd_h );

  //int const KW = p->kh;
  //int const SW = SW;
  //int const OW = OW;
  //int const PW = PW;
  size_t const DW = p->dw + 1;
  const int gcd_w = gcd( SW, DW );
  //const int lcm_w = lcm( SW, DW );
  const int lcm_w = SW * DW/gcd_w;
  const int kww = lcm_w / DW;
  const int jww = lcm_w / SW;
  int wa, wb, wg;
  extendedEuclid( wa, DW, wb, SW, wg);
  DMUST( wg == gcd_w );
  const size_t OCOG = OC / G;
  const size_t ICOG = IC / G;
  const size_t ICOG_KH_KW = ICOG * KH * KW;
  const size_t OH_OW = OH * OW;
  OMP(parallel for collapse(5))//;
  for (int g = 0; g < G; ++g) {
    for (int mb = 0; mb < MB; ++mb) {
      for (int ic = 0; ic < IC/G; ++ic) {
        for (int ih = 0; ih < IH; ++ih) {
          for (int iw = 0; iw < IW; ++iw) {
            size_t src_off = src_off_f(p, mb, g, ic, ih, iw);
            float &ds = pdiff_src[src_off];
            ds = 0;

            size_t kh_end = (ih + PH) / DH + 1;
            if (kh_end > KH) kh_end = KH;
            size_t kh_beg = kh_end;
            if( (ih+PH) % gcd_h == 0 ){ // Do solutions exist?
              int kh_ibeg = kh_end;
              int const mul = (ih+PH) / gcd_h;
              kh_ibeg = ha*mul;
              int m = div_floor(kh_ibeg, khh);
              kh_ibeg -= m * khh;
              int j = hb * mul + m * jhh; // var j --> 'm' again
              if (j >= OH) kh_ibeg += (j-OH)/jhh * khh + khh;
              kh_beg = kh_ibeg;
            }
            if( kh_beg >= kh_end ) continue;

            size_t kw_end = (iw + PW) / DW + 1;
            if (kw_end > KW) kw_end = KW;
            size_t kw_beg = kw_end;
            if( (iw+PW) % gcd_w == 0 ){ // Do solutions exist?
              int kw_ibeg;
              int const mul = (iw+PW) / gcd_w;
              kw_ibeg = wa * mul;
              int m = div_floor(kw_ibeg, kww);
              kw_ibeg -= m * kww;
              int j = wb * mul + m * jww;
              if (j >= OW) kw_ibeg += (j-OW)/jww * kww + kww;
              kw_beg = kw_ibeg;
            }
            if( kw_beg >= kw_end ) continue;
#if 0
            for (size_t kh = kh_beg, oh0=ih+PH - kh_beg*DH ;
                kh < kh_end;
                oh0 -= lcm_h, kh += khh)
            {
              for (size_t kw = kw_beg, ow0=iw+PW - kw_beg*(p->dw+1) ;
                  kw < kw_end;
                  ow0 -= lcm_w, kw += kww)
              {
                const size_t dst_off0 = (((size_t)mb * OC + g * OCOG + 0) * OH + oh0/SH) * OW + ow0/SH;
                const size_t wei_off0 = ((((size_t)g * OCOG + 0 ) * ICOG + ic) * KH + kh) * KW + kw;
                float d[OCOG], w[OCOG];
                for (size_t oc = 0; oc < OCOG; ++oc) {
                  //size_t dst_off = dst_off_f(p, mb, g, oc, oh0/SH, ow0/SW);
                  //size_t wei_off = wei_off_f(p, g, oc, ic, kh, kw);
                  //ds += pdiff_dst[dst_off] * pwei[wei_off];
                  d[oc] = pdiff_dst[dst_off0 + oc*OH_OW];
                  w[oc] = pwei     [wei_off0 + oc*ICOG_KH_KW];
                  ds += d[oc] * w[oc];
                }
              }
            }
#else
            float d[OCOG], w[OCOG];
            size_t oh0  = ih+PH - kh_beg*DH;
            size_t ow00 = iw+PW - kw_beg*DW;
            for (size_t kh = kh_beg; kh < kh_end; kh += khh)
            {
              size_t ow0 = ow00;
              for (size_t kw = kw_beg; kw < kw_end; kw += kww)
              {
                const size_t dst_off0 = (((size_t)mb * OC + g * OCOG + 0) * OH + oh0/SH) * OW + ow0/SH;
                const size_t wei_off0 = ((((size_t)g * OCOG + 0 ) * ICOG + ic) * KH + kh) * KW + kw;
                for (size_t oc = 0; oc < OCOG; ++oc) {
                  //size_t dst_off = dst_off_f(p, mb, g, oc, oh0/SH, ow0/SW);
                  //size_t wei_off = wei_off_f(p, g, oc, ic, kh, kw);
                  //ds += pdiff_dst[dst_off] * pwei[wei_off];
                  d[oc] = pdiff_dst[dst_off0 + oc*OH_OW];
                  w[oc] = pwei     [wei_off0 + oc*ICOG_KH_KW];
                  ds += d[oc] * w[oc];
                }
                ow0 -= lcm_w;
              }
              oh0 -= lcm_h;
            }
#endif

          }
        }
      }
    }
  }
#undef G
#undef MB
#undef IC
#undef OC
#undef KH
#undef IH
#undef OH
#undef SH
#undef PH
#undef KW
#undef IW
#undef OW
#undef SW
#undef PW
#elif 1 // no macros, use ssize_t
  float       * restrict const pdiff_src = (float*)diff_src_m;
  float const * restrict const pwei = (float*)wei_m;
  float const * restrict const pdiff_dst = (float*)diff_dst_m;
#define G  p->g
#define MB p->mb
#define IC p->ic
#define OC p->oc
#define KH p->kh
#define IH p->ih
#define OH p->oh
#define SH p->sh
#define PH p->ph
#define KW p->kw
#define IW p->iw
#define OW p->ow
#define SW p->sw
#define PW p->pw
  //int const KH = p->kh;
  //int const OH = OH;
  //int const DH = p->dh + 1;
  //int const SH = SH;
  //int const PH = PH;
  size_t const DH = p->dh + 1;
  const int gcd_h = gcd( SH, DH );
  const int lcm_h = SH * DH/gcd_h; //lcm( SH, DH ) = SH*DH / gcd(SH,DH)
  const int khh = lcm_h / DH;
  DMUST( khh == SH / gcd(SH,DH) );
  const int jhh = lcm_h / SH;
  int ha, hb, hg;
  extendedEuclid( ha, DH, hb, SH, hg);
  //print(0," extendedEuclid: %d * [DH=%d] + %d * [SH=%d] = %d[gcd(DH,SH)]\n", ha,DH, hb,SH, hg);
  DMUST( hg == gcd_h );

  //int const KW = p->kh;
  //int const SW = SW;
  //int const OW = OW;
  //int const PW = PW;
  size_t const DW = p->dw + 1;
  const int gcd_w = gcd( SW, DW );
  //const int lcm_w = lcm( SW, DW );
  const int lcm_w = SW * DW/gcd_w;
  const int kww = lcm_w / DW;
  const int jww = lcm_w / SW;
  int wa, wb, wg;
  extendedEuclid( wa, DW, wb, SW, wg);
  DMUST( wg == gcd_w );
  const size_t OCOG = OC / G;
  const size_t ICOG = IC / G;
  const size_t ICOG_KH_KW = ICOG * KH * KW;
  const size_t OH_OW = OH * OW;
  OMP(parallel for collapse(5))//;
  for (int g = 0; g < G; ++g) {
    for (int mb = 0; mb < MB; ++mb) {
      for (int ic = 0; ic < IC/G; ++ic) {
        for (int ih = 0; ih < IH; ++ih) {
          for (int iw = 0; iw < IW; ++iw) {
            size_t src_off = src_off_f(p, mb, g, ic, ih, iw);
            float &ds = pdiff_src[src_off];
            ds = 0;

            size_t kh_end = (ih + PH) / DH + 1;
            if (kh_end > KH) kh_end = KH;
            size_t kh_beg = kh_end;
            if( (ih+PH) % gcd_h == 0 ){ // Do solutions exist?
              int kh_ibeg = kh_end;
              int const mul = (ih+PH) / gcd_h;
              kh_ibeg = ha*mul;
              int m = div_floor(kh_ibeg, khh);
              kh_ibeg -= m * khh;
              int j = hb * mul + m * jhh; // var j --> 'm' again
              if (j >= OH) kh_ibeg += (j-OH)/jhh * khh + khh;
              kh_beg = kh_ibeg;
            }
            if( kh_beg >= kh_end ) continue;

            size_t kw_end = (iw + PW) / DW + 1;
            if (kw_end > KW) kw_end = KW;
            size_t kw_beg = kw_end;
            if( (iw+PW) % gcd_w == 0 ){ // Do solutions exist?
              int kw_ibeg;
              int const mul = (iw+PW) / gcd_w;
              kw_ibeg = wa * mul;
              int m = div_floor(kw_ibeg, kww);
              kw_ibeg -= m * kww;
              int j = wb * mul + m * jww;
              if (j >= OW) kw_ibeg += (j-OW)/jww * kww + kww;
              kw_beg = kw_ibeg;
            }
            if( kw_beg >= kw_end ) continue;

            for (size_t kh = kh_beg, oh0=ih+PH - kh_beg*DH ;
                kh < kh_end;
                oh0 -= lcm_h, kh += khh)
            {
              for (size_t kw = kw_beg, ow0=iw+PW - kw_beg*(p->dw+1) ;
                  kw < kw_end;
                  ow0 -= lcm_w, kw += kww)
              {
                const size_t dst_off0 = (((size_t)mb * OC + g * OCOG + 0) * OH + oh0/SH) * OW + ow0/SH;
                const size_t wei_off0 = ((((size_t)g * OCOG + 0 ) * ICOG + ic) * KH + kh) * KW + kw;
                float d[OCOG], w[OCOG];
                for (size_t oc = 0; oc < OCOG; ++oc) {
                  //size_t dst_off = dst_off_f(p, mb, g, oc, oh0/SH, ow0/SW);
                  //size_t wei_off = wei_off_f(p, g, oc, ic, kh, kw);
                  //ds += pdiff_dst[dst_off] * pwei[wei_off];
                  d[oc] = pdiff_dst[dst_off0 + oc*OH_OW];
                  w[oc] = pwei     [wei_off0 + oc*ICOG_KH_KW];
                  ds += d[oc] * w[oc];
                }
              }
            }

          }
        }
      }
    }
  }
#undef G
#undef MB
#undef IC
#undef OC
#undef KH
#undef IH
#undef OH
#undef SH
#undef PH
#undef KW
#undef IW
#undef OW
#undef SW
#undef PW
#else
#error "select one"
#endif
}

/** Special-case the non-dilation BWD_D code.
 * In this case, the postive integer conditional hoistings were
 * previously derived (ex. \ref ref_conv3.cpp).  They should agree
 * with \c sx_conv_3_fwd hoistings, specialized for DH=1 [DW=1],
 * but have been simplified a bit further.
 */
void sxconv_4_bwd_d(const prb_t *p, dnn_mem_t &diff_src_m,
    dnn_mem_t &wei_m, dnn_mem_t &diff_dst_m) {
  float       * restrict const pdiff_src = (float*)diff_src_m;
  float const * restrict const pwei = (float*)wei_m;
  //float const * restrict const pbia = (float*)bia_m;
  float const * restrict const pdiff_dst = (float*)diff_dst_m;
#if 1 // a new simplification of loop limits (same speed as above)
  // regr-dilate: 2.50x
  if (p->dh != 0 || p->dw != 0) { // A fast version here does not support dilation
    // FIXME can we call this even less?
    sxconv_4_bwd_d_generic(p, diff_src_m, wei_m, diff_dst_m);
    return;
  }
#define MAC 0
  // SX: MAC=0 ./regr.sh BWD_D 17.0x ---> MAC=1 10.55x; MAC=1+int 
#if MAC
#define G  p->g
#define MB p->mb
#define IC p->ic
#define OC p->oc
#define KH p->kh
#define IH p->ih
#define OH p->oh
#define SH p->sh
#define PH p->ph
#define KW p->kw
#define IW p->iw
#define OW p->ow
#define SW p->sw
#define PW p->pw
  const ssize_t ICOG = IC/G;
  const ssize_t ICOG_KH_KW = ICOG * KH * KW;
  const ssize_t OCOG = OC / G;
  const ssize_t OH_OW = OH * OW;
#else
  const int G = p->g;
  const int MB = p->mb;
  const int IC = p->ic;
  const int IH = p->ih;
  const int IW = p->iw;
  const int OC = p->oc;
  const int OH = p->oh;
  const int OW = p->ow;
  const int KH = p->kh;
  const int KW = p->kw;
  const int PH = p->ph;
  const int PW = p->pw;
  const int SH = p->sh;
  const int SW = p->sw;
  const int DH = p->dh + 1;
  const int DW = p->dw + 1;
  const ssize_t ICOG = IC/G;
  const ssize_t ICOG_KH_KW = ICOG * KH * KW;
  const ssize_t OCOG = OC / G;
  const ssize_t OH_OW = OH * OW;
#endif
  ssize_t khb[IH], khe[IH], ohb[IH];
  for (ssize_t ih = 0; ih < IH; ++ih) {
    khe[ih] = ih + PH;
    ohb[ih] = OH - 1;
    khb[ih] = khe[ih] - OH*SH + SH;
    if( khb[ih] < SH - 1 ){ // unlikely?
      ohb[ih] = khe[ih] / SH;
      khb[ih] = khe[ih] % SH;
    }
    if (++khe[ih] > KH) khe[ih] = KH;
  }
  ssize_t kwb[IW], kwe[IW], owb[IW];
  for (ssize_t iw = 0; iw < IW; ++iw) {
    kwe[iw] = iw + PW;
    owb[iw] = OW - 1;
    kwb[iw] = kwe[iw] - OW*SW + SW;
    if( kwb[iw] < SW - 1 ){ // unlikely?
      owb[iw] = kwe[iw] / SW;
      kwb[iw] = kwe[iw] % SW;
    }
    if (++kwe[iw] > KW) kwe[iw] = KW;
  }
  OMP(parallel for collapse(4))//;
  for (ssize_t g = 0; g < G; ++g) {
    for (ssize_t mb = 0; mb < MB; ++mb) {
      for (ssize_t ic = 0; ic < ICOG; ++ic) {
        for (ssize_t ih = 0; ih < IH; ++ih) {
          const ssize_t kh_b = khb[ih];
          const ssize_t kh_e = khe[ih];
          const ssize_t oh_b = ohb[ih];

          float tmp_iw[IW] alignas(128);
          for (ssize_t iw = 0; iw < IW; ++iw) {
            const ssize_t kw_b = kwb[iw];
            const ssize_t kw_e = kwe[iw];
            const ssize_t ow_b = owb[iw];

            float tmp = 0.f;
            if( kh_b < kh_e && kw_b < kw_e ){
              float tmp_oc[OCOG] alignas(128);
              //const ssize_t tmp_oc_sz = (kh_e-kh_b)*(kw_e-kw_b)*OCOG;
              //float tmp_oc[tmp_oc_sz] alignas(128); // introducing this was perhaps a bit slower
              //ssize_t ixoc = 0;
              const ssize_t w0 = (((g * OCOG + 0 ) * ICOG + ic) * KH + 0) * KW + 0;
              const ssize_t d0 = ((mb * OC + g * OCOG + 0) * OH + 0) * OW + 0;
              for (ssize_t kh = kh_b, oh=oh_b; kh < kh_e; --oh, kh+=SH) {
                for (ssize_t kw = kw_b, ow=ow_b; kw < kw_e; --ow, kw += SW) {
                  //const ssize_t wei_off0 = (((g * OCOG + 0 ) * ICOG + ic) * KH + kh) * KW + kw;
                  //const ssize_t dst_off0 = ((mb * OC + g * OCOG + 0) * OH + oh) * OW + ow;
                  const ssize_t wei_off0 = w0 + kh*KW+kw;
                  const ssize_t dst_off0 = d0 + oh*OW+ow;
                  for (ssize_t oc = 0; oc < OCOG; ++oc) {
                    //ds += pdiff_dst[dst_off0 + oc*OH_OW] * pwei[wei_off0 + oc*ICOG_KH_KW];
                    // slower index calc...
                    //tmp += pdiff_dst[dst_off0 + oc*OH_OW] * pwei[wei_off0 + oc*ICOG_KH_KW];
                    //tmp_oc[oc] = pdiff_dst[(int)((float)dst_off0 + (float)oc*OH_OW)]
                    //           * pwei[(int)((float)wei_off0 + (float)oc*ICOG_KH_KW)];
                    tmp_oc[oc]  = pdiff_dst[(dst_off0 + oc*OH_OW)];
                    tmp_oc[oc] *= pwei[(wei_off0 + oc*ICOG_KH_KW)];
                    tmp += tmp_oc[oc];
                    //itmp_oc[ixoc] = pdiff_dst[(int)((float)dst_off0 + (float)oc*OH_OW)]
                    //           * pwei[(int)((float)wei_off0 + (float)oc*ICOG_KH_KW)];
                    //++ixoc;
                  }
                }
              }
              //for (ssize_t oc = 0; oc < tmp_oc_sz; ++oc) { tmp += tmp_oc[oc]; }
            }

            //pdiff_src[src_off0+iw] = tmp; // MUST always be executed (even to store 0.f)
            tmp_iw[iw] = tmp; // MUST always be executed (even to store 0.f)
          }
          ssize_t src_off0 = src_off_f2(p, mb, g, ic, ih, 0);
          for (ssize_t iw = 0; iw < IW; ++iw) {
            pdiff_src[src_off0+iw] = tmp_iw[iw]; // MUST always be executed (even to store 0.f)
          }

        }
      }
    }
  }
#if MAC
#undef G
#undef MB
#undef IC
#undef OC
#undef KH
#undef IH
#undef OH
#undef SH
#undef PH
#undef KW
#undef IW
#undef OW
#undef SW
#undef PW
#endif
#else
#error "please enable one impl"
#endif
}

/** This one simplifies the hoisting function much
 * like \c sxconv_4_fwd.
 *
 * - simplify:
 * 1. When is oh_beg >= 1?
 *   - (PH - kh*DH +SH-1) / SH >= 1
 *   - PH - kh*DH + SH-1 >= SH
 *   - PH - kh*DH - 1 >= 0
 *   - kh*DH <= PH -1
 *   - kh*DH < PH
 *   - In this case, we can set oh_beg=(PH - kh*DH +SH-1) / SH,
 *     - and it is OK if this value is > OH.
 * 2. When is oh_end >= OH?
 *   - (IH + PH - kh*DH + SH - 1) / SH >= OH
 *   - IH + PH - kh*DH + SH - 1 >= OH*SH
 *   - kh*DH + 1 <= IH+PH+SH - OH*SH
 *   - kh*DH + OH*SH < IH+PH+SH
 *   - In this case, we may set oh_end = OH,
 * 3. When is oh_end >= 1?
 *   - this is a bit simpler: OH-->1 in above...
 *   - kh*DH < IH+PH
 *   - If this is *not* the case, we can set oh_end=0
 *     - otherwise (IH + PH - kh*DH + SH - 1) / SH
 *       - (subject to oh_end <= OH)
 */
void sxconv_4_bwd_w(const prb_t *p, dnn_mem_t &src_m,
    dnn_mem_t &diff_wei_m, dnn_mem_t &diff_bia_m, dnn_mem_t &diff_dst_m) {
  const ssize_t G = p->g;
  const ssize_t MB = p->mb;
  const ssize_t IC = p->ic;
  const ssize_t IH = p->ih;
  const ssize_t IW = p->iw;
  const ssize_t OC = p->oc;
  const ssize_t OH = p->oh;
  const ssize_t OW = p->ow;
  const ssize_t KH = p->kh;
  const ssize_t KW = p->kw;
  const ssize_t PH = p->ph;
  const ssize_t PW = p->pw;
  const ssize_t SH = p->sh;
  const ssize_t SW = p->sw;
  const ssize_t DH = p->dh + 1;
  const ssize_t DW = p->dw + 1;
  const ssize_t ICOG = IC/G;
  const ssize_t ICOG_KH_KW = ICOG * KH * KW;
  const ssize_t OCOG = OC / G;
  const ssize_t OH_OW = OH * OW;
  const ssize_t OC_OH_OW = OC * OH_OW;
  const ssize_t IH_IW = IH * IW;
  const ssize_t KH_KW = KH * KW;
  const ssize_t SH_IW = SH * IW;
  float const * restrict const psrc = (float*)src_m;
  float       * restrict const pdiff_wei = (float*)diff_wei_m;
  float       * restrict const pdiff_bia = (float*)diff_bia_m;
  float const * restrict const pdiff_dst = (float*)diff_dst_m;
#if 1
  // It is hard to measure any speed difference from modifying the bwd_w_bias_update
  auto bwd_w_bias_update = [&](const prb_t* p, dnn_mem_t &diff_bia_m, dnn_mem_t &diff_dst_m){
    if ((p->dir & FLAG_BIA)) {
      float       * restrict const pdiff_bia = (float*)diff_bia_m;
      float const * restrict const pdiff_dst = (float*)diff_dst_m;
#if 1
      for (int oc = 0; oc < OC     ; ++oc) {
        float tmp = 0.f;
        OMP(parallel for collapse(2) reduction(+:tmp))//;
        for (int mb = 0; mb < MB; ++mb) {
          for (int ohw = 0; ohw < OH*OW; ++ohw) {
            tmp += pdiff_dst[dst_off_f_nog_ohw(p,mb,/*g,*/oc,ohw)];
          }
        }
        size_t bia_off = bia_off_f_nog(p, /*g,*/ oc);
        pdiff_bia[ bia_off_f_nog(p,/*g,*/oc) ] = tmp;
      }
    }
#else
    OMP(for collapse(2) nowait)//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        const ssize_t bia_off = bia_off_f(p, g, oc);
        pdiff_bia[bia_off] = 0.f;
        const ssize_t dst_off000 = ((0 * OC + g * OCOG + oc) * OH + 0) * OW + 0;
        for (ssize_t mb = 0; mb < MB; ++mb) {
          //const ssize_t dst_off00 = ((mb * OC + g * OCOG + oc) * OH + 0) * OW + 0;
          for (ssize_t oh = 0; oh < OH; ++oh) {
            for (ssize_t ow = 0; ow < OW; ++ow) {
              //pdiff_bia[bia_off] += pdiff_dst[dst_off00 + oh*OW + ow];
              pdiff_bia[bia_off] += pdiff_dst[dst_off000 + mb*OC*OH*OW + oh*OW + ow];
            }
          }
        }
      }
    }
#endif
  };
#endif
  //memset( (float*)diff_bia_m, 0, diff_bia_m.size() ); // single loop, always equiv
  //zero_bia(p, diff_bia_m);
#if 0 // A1,A3 x86 cf. sx3 : 9.6, 8.0   (sxconv3: 7.1, 19.3)   x86:8.5,7.7,1.1
  OMP(parallel)//;
  {
    OMP(for collapse(3))//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        for (ssize_t ic = 0; ic < ICOG; ++ic) {
          const ssize_t wei_off00 = ((((size_t)g * OCOG + oc) * ICOG + ic) * KH + 0) * KW + 0;
          for (ssize_t kh = 0; kh < KH; ++kh) {
            for (ssize_t kw = 0; kw < KW; ++kw) {
              pdiff_wei[wei_off00 + kh*KW + kw] = 0.f;
            }
          }
        }
      }
    }
    // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
    OMP(for collapse(4))//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        //for (ssize_t ic = 0; ic < IC/G; ++ic)
          for (ssize_t kh = 0; kh < p->kh; ++kh) {
            for (ssize_t kw = 0; kw < KW; ++kw) {
              const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;
              // SX TODO simplify these to positive-integer solutions !!!
              ssize_t oh_beg, oh_end;
              oh_beg = div_floor(      SH - ih0 - 1, SH);//(c-a+b-1)/b
              oh_end = div_floor( IH + SH - ih0 - 1, SH);//(d-a+b-1)/b
              if (oh_beg < 0    ) oh_beg = 0;
              if (oh_end > OH) oh_end = OH;
#if 0 // establish non-negative equivalencies
              RT_ASSERT( (oh_beg >= 1) == (kh*DH < PH) );
              RT_ASSERT( (oh_end >= 1) == (kh*DH < IH+PH) );
              RT_ASSERT( (oh_end >= OH) == (kh*DH + OH*SH < IH+PH+SH) ); // ???
              size_t ohb2 = 0;
              if( kh*DH < PH ) ohb2 = ((PH-kh*DH) + SH-1)/ SH;
              RT_ASSERT( ohb2 == oh_beg );
              size_t ohe2 = 0;
              if( kh*DH < IH+PH ) {
                ohe2 = ((IH + PH - kh*DH) + SH-1) / SH;
                RT_ASSERT( ohe2 >= 1 );
              }
              if( ohe2 >= OH ) {
                ohe2 = OH;
                RT_ASSERT( (kh*DH + OH*SH < IH+PH+SH) );
              }
              // note: oh_end from div_floor and signed integer was allowed to be -ve
              RT_ASSERT( (oh_end<0 && ohe2==0) || (oh_end>=0 && ohe2==oh_end ) );
#endif
              ssize_t ow_beg, ow_end;
              ow_beg = div_floor(    + PW - kw*DW + SW - 1, SW);//(c-a+b-1)/b
              ow_end = div_floor( IW + PW - kw*DW + SW - 1, SW);//(d-a+b-1)/b
              if (ow_beg < 0    ) ow_beg = 0;
              if (ow_end > OW) ow_end = OW;
              if( ow_beg >= ow_end || oh_beg >= oh_end ) continue;
              //const size_t iw_beg = ow_beg * SW - PW + kw * DW;
              const size_t ow_emb = ow_end - ow_beg;

              float dw_ic[ICOG];
              //float dw_ic[ICOG][MB];
              for (ssize_t ic = 0; ic < ICOG; ++ic) {
                float tmp = 0.f;
                for (ssize_t mb = 0; mb < MB; ++mb) {
                  for (ssize_t oh = oh_beg; oh < oh_end; ++oh) {
                    const size_t dst_off_beg = ((mb * OC + g * OCOG + oc) * OH + oh) * OW + ow_beg;
                    const ssize_t ih = ih0 + oh * SH;
                    const size_t iw_beg = ow_beg * SW - PW + kw * DW;
                    const size_t src_off_beg = ((mb * IC + g * ICOG + ic ) * IH + ih) * IW + iw_beg;
                    //const size_t src_off_beg = ((mb * IC + g * ICOG + ic ) * IH + ih) * IW + ow_beg * SW - PW + kw * DW;
                    for (size_t ow = 0; ow < ow_emb; ++ow) {
                     tmp += pdiff_dst[dst_off_beg+ow] * psrc[src_off_beg + ow*SW];
                    }
                  }
                }
                dw_ic[ic] = tmp;
              }
              const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
              for (ssize_t ic = 0; ic < ICOG; ++ic){
                pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
              }

            }
          }
      }
    }

    if ((p->dir & FLAG_BIA)) {
      OMP(for collapse(2) nowait)//;
      for (ssize_t g = 0; g < G; ++g) {
        for (ssize_t oc = 0; oc < OCOG; ++oc) {
          ssize_t bia_off = bia_off_f(p, g, oc);
          float &db = ((float*)diff_bia_m)[bia_off];
          db = 0;
          for (ssize_t mb = 0; mb < MB; ++mb) {
            const ssize_t dst_off00 = ((mb * OC + g * OCOG + oc) * OH + 0) * OW + 0;
            for (ssize_t oh = 0; oh < OH; ++oh) {
              for (ssize_t ow = 0; ow < OW; ++ow) {
                db += pdiff_dst[dst_off00 + oh*OW + ow];
              }
            }
          }
        }
      }
    }
  }
#elif 0 // x86 9.9, 9.0   x86:8.4,7.4,1.8
  // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
  OMP(parallel for collapse(4))//;
  for (ssize_t g = 0; g < G; ++g) {
    for (ssize_t oc = 0; oc < OCOG; ++oc) {
      for (ssize_t kh = 0; kh < p->kh; ++kh) {
        for (ssize_t kw = 0; kw < KW; ++kw) {
          const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;
          ssize_t oh_beg, oh_end;
          oh_beg = div_floor(      SH - ih0 - 1, SH);//(c-a+b-1)/b
          oh_end = div_floor( IH + SH - ih0 - 1, SH);//(d-a+b-1)/b
          if (oh_beg < 0    ) oh_beg = 0;
          if (oh_end > OH) oh_end = OH;
          ssize_t ow_beg, ow_end;
          ow_beg = div_floor(    + PW - kw*DW + SW - 1, SW);//(c-a+b-1)/b
          ow_end = div_floor( IW + PW - kw*DW + SW - 1, SW);//(d-a+b-1)/b
          if (ow_beg < 0    ) ow_beg = 0;
          if (ow_end > OW) ow_end = OW;
          //if( ow_beg >= ow_end || oh_beg >= oh_end ) continue;
          //const size_t iw_beg = ow_beg * SW - PW + kw * DW;
          const size_t ow_emb = ow_end - ow_beg;

          float dw_ic[ICOG];
          for (ssize_t ic = 0; ic < ICOG; ++ic) dw_ic[ic] = 0.f;
          if (ow_beg < ow_end && oh_beg < oh_end) {
            for (ssize_t ic = 0; ic < ICOG; ++ic) {
              float tmp = 0.f;
              for (ssize_t mb = 0; mb < MB; ++mb) {
                for (ssize_t oh = oh_beg; oh < oh_end; ++oh) {
                  const size_t dst_off_beg = ((mb * OC + g * OCOG + oc) * OH + oh) * OW + ow_beg;
                  const ssize_t ih = ih0 + oh * SH;
                  const size_t iw_beg = ow_beg * SW - PW + kw * DW;
                  const size_t src_off_beg = ((mb * IC + g * ICOG + ic ) * IH + ih) * IW + iw_beg;
                  //const size_t src_off_beg = ((mb * IC + g * ICOG + ic ) * IH + ih) * IW + ow_beg * SW - PW + kw * DW;
                  for (size_t ow = 0; ow < ow_emb; ++ow) {
                    tmp += pdiff_dst[dst_off_beg+ow] * psrc[src_off_beg + ow*SW];
                  }
                }
              }
              dw_ic[ic] = tmp;
            }
          }
          const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
          for (ssize_t ic = 0; ic < ICOG; ++ic){
            pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
          }

        }
      }
    }
  }

  if ((p->dir & FLAG_BIA)) {
    OMP(for collapse(2) nowait)//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        ssize_t bia_off = bia_off_f(p, g, oc);
        float &db = ((float*)diff_bia_m)[bia_off];
        db = 0;
        for (ssize_t mb = 0; mb < MB; ++mb) {
          const ssize_t dst_off00 = ((mb * OC + g * OCOG + oc) * OH + 0) * OW + 0;
          for (ssize_t oh = 0; oh < OH; ++oh) {
            for (ssize_t ow = 0; ow < OW; ++ow) {
              db += pdiff_dst[dst_off00 + oh*OW + ow];
            }
          }
        }
      }
    }
  }
#elif 0 // x86 6.0, 8.2 x86:8.8,7.2,1.8
  // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
  OMP(parallel for collapse(4))//;
  for (ssize_t g = 0; g < G; ++g) {
    for (ssize_t oc = 0; oc < OCOG; ++oc) {
      ShortLoop() for (ssize_t kh = 0; kh < p->kh; ++kh) {
        ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
          const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;
          ssize_t oh_beg, oh_end;
          oh_beg = div_floor(      SH - ih0 - 1, SH);//(c-a+b-1)/b
          oh_end = div_floor( IH + SH - ih0 - 1, SH);//(d-a+b-1)/b
          if (oh_beg < 0    ) oh_beg = 0;
          if (oh_end > OH) oh_end = OH;
          ssize_t ow_beg, ow_end;
          ow_beg = div_floor(    + PW - kw*DW + SW - 1, SW);//(c-a+b-1)/b
          ow_end = div_floor( IW + PW - kw*DW + SW - 1, SW);//(d-a+b-1)/b
          if (ow_beg < 0    ) ow_beg = 0;
          if (ow_end > OW) ow_end = OW;
          //if( ow_beg >= ow_end || oh_beg >= oh_end ) continue;
          //const size_t iw_beg = ow_beg * SW - PW + kw * DW;

          float dw_ic[ICOG];
          for (ssize_t ic = 0; ic < ICOG; ++ic) dw_ic[ic] = 0.f;
          if (ow_beg < ow_end && oh_beg < oh_end) {
            ow_end -= ow_beg;
            oh_end -= oh_beg;
            const ssize_t d0 = ((0 * OC + g * OCOG + oc) * OH + oh_beg) * OW + ow_beg;
            const ssize_t s00 = ((0 * IC + g * ICOG + 0 ) * IH + (ih0+oh_beg*SH)) * IW + ow_beg*SW - PW + kw*DW;
            for (ssize_t ic = 0; ic < ICOG; ++ic) {
              float tmp = 0.f;
              for (ssize_t mb = 0; mb < MB; ++mb) {
                //const ssize_t dst_off_beg = ((mb * OC + g * OCOG + oc) * OH + oh_beg) * OW + ow_beg;
                const ssize_t dst_off_beg = d0 + mb * OC*OH*OW;
                //const ssize_t src_off_beg = ((mb * IC + g * ICOG + ic ) * IH + (ih0+oh_beg*SH)) * IW + ow_beg*SW - PW + kw*DW;
                const ssize_t src_off_beg = s00 + mb*IC*IH*IW + ic*IH*IW;
#if 1
                for (ssize_t oh = 0; oh < oh_end; ++oh) {
                  for (size_t ow = 0; ow < ow_end; ++ow) {
                    tmp += pdiff_dst[dst_off_beg+oh*OW+ow] * psrc[src_off_beg + oh*SH_IW + ow*SW];
                  }
                }
#elif 0
                if(oh_end < 256 && ow_end < 256){ // ShortLoop is faster, but speed destroyed by 'if'
                  ShortLoop() for (ssize_t oh = 0; oh < oh_end; ++oh) {
                    ShortLoop() for (size_t ow = 0; ow < ow_end; ++ow) {
                      tmp += pdiff_dst[dst_off_beg+oh*OW+ow] * psrc[src_off_beg + oh*SH_IW + ow*SW];
                    }
                  }
                }else{
                  for (ssize_t oh = 0; oh < oh_end; ++oh) {
                    for (size_t ow = 0; ow < ow_end; ++ow) {
                      tmp += pdiff_dst[dst_off_beg+oh*OW+ow] * psrc[src_off_beg + oh*SH_IW + ow*SW];
                    }
                  }
                }
#elif 0 // slower
                float s[oh_end*ow_end];
                float d[oh_end*ow_end];
                for (ssize_t oh = 0; oh < oh_end; ++oh) {
                  for (size_t ow = 0; ow < ow_end; ++ow) {
                    d[oh*ow_end+ow] = pdiff_dst[dst_off_beg + oh*OW + ow];
                    s[oh*ow_end+ow] = psrc[src_off_beg + oh*SH_IW + ow*SW];
                  }
                }
                for (ssize_t o = 0; o < ow_end*oh_end; ++o) {
                  tmp += d[o] * s[o];
                }
#elif 0 // very slow
                float s[OH][OW], d[OH][OW];
                for (ssize_t oh = 0; oh < OH; ++oh) {
                  for (size_t ow = 0; ow < OW; ++ow) {
                    d[oh][ow] = (oh<oh_end && ow<ow_end ? pdiff_dst[dst_off_beg + oh*OW + ow]: 0.f);
                    s[oh][ow] = (oh<oh_end && ow<ow_end ? psrc[src_off_beg + oh*SH_IW + ow*SW]: 0.f);
                  }
                }
                for (ssize_t oh = 0; oh < OH; ++oh)
                  for (size_t ow = 0; ow < OW; ++ow)
                    tmp += d[oh][ow] * s[oh][ow];
#endif
              }
              dw_ic[ic] = tmp;
            }
          }
          const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
          for (ssize_t ic = 0; ic < ICOG; ++ic){
            pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
          }

        }
      }
    }
  }

  if ((p->dir & FLAG_BIA)) {
    OMP(for collapse(2) nowait)//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        ssize_t bia_off = bia_off_f(p, g, oc);
        float &db = ((float*)diff_bia_m)[bia_off];
        db = 0;
        for (ssize_t mb = 0; mb < MB; ++mb) {
          const ssize_t dst_off00 = ((mb * OC + g * OCOG + oc) * OH + 0) * OW + 0;
          for (ssize_t oh = 0; oh < OH; ++oh) {
            for (ssize_t ow = 0; ow < OW; ++ow) {
              db += pdiff_dst[dst_off00 + oh*OW + ow];
            }
          }
        }
      }
    }
  }
#elif 0 // loop order change, much better 24x for regr.sh BWD_WB (x86:6.9,19.3)  // XXX FAIL GOOD for ic >~ omp_num_threads ??? XXX
  // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
  OMP(parallel for collapse(4))//;
  for (ssize_t g = 0; g < G; ++g) {
    for (ssize_t oc = 0; oc < OCOG; ++oc) {
      ShortLoop() for (ssize_t kh = 0; kh < p->kh; ++kh) {
        ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
          const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;
          ssize_t oh_beg, oh_end;
          oh_beg = div_floor(      SH - ih0 - 1, SH);//(c-a+b-1)/b
          oh_end = div_floor( IH + SH - ih0 - 1, SH);//(d-a+b-1)/b
          if (oh_beg < 0    ) oh_beg = 0;
          if (oh_end > OH) oh_end = OH;
          ssize_t ow_beg, ow_end;
          ow_beg = div_floor(    + PW - kw*DW + SW - 1, SW);//(c-a+b-1)/b
          ow_end = div_floor( IW + PW - kw*DW + SW - 1, SW);//(d-a+b-1)/b
          if (ow_beg < 0    ) ow_beg = 0;
          if (ow_end > OW) ow_end = OW;
          float dw_ic[ICOG];
          for (ssize_t ic = 0; ic < ICOG; ++ic) dw_ic[ic] = 0.f;
          if (ow_beg < ow_end && oh_beg < oh_end) {
            ow_end -= ow_beg;
            oh_end -= oh_beg;
            const ssize_t d0 = ((0 * OC + g * OCOG + oc) * OH + oh_beg) * OW + ow_beg;
            const ssize_t s00 = ((0 * IC + g * ICOG + 0 ) * IH + (ih0+oh_beg*SH)) * IW + ow_beg*SW - PW + kw*DW;
            for (ssize_t mb = 0; mb < MB; ++mb) {
              const ssize_t dst_off_beg = d0 + mb * OC*OH*OW;
              const ssize_t s0 = s00 + mb*IC*IH*IW;
              for (ssize_t oh = 0; oh < oh_end; ++oh) {
                for (size_t ow = 0; ow < ow_end; ++ow) {
                  for (ssize_t ic = 0; ic < ICOG; ++ic) {
                    dw_ic[ic] += pdiff_dst[dst_off_beg+oh*OW+ow] * psrc[s0+ic*IH*IW + oh*SH_IW + ow*SW];
                    // slower ...
                    //dw_ic[ic] += pdiff_dst[d0+mb*OC*OH*OW + oh*OW+ow] * psrc[s00+mb*IC*IH*IW + ic*IH*IW + oh*SH_IW + ow*SW];
                  }
                }
              }
            }
          }
          const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
          for (ssize_t ic = 0; ic < ICOG; ++ic){
            pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
          }

        }
      }
    }
  }
  if ((p->dir & FLAG_BIA)) {
    OMP(for collapse(2) nowait)//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        const ssize_t bia_off = bia_off_f(p, g, oc);
        pdiff_bia[bia_off] = 0.f;
        const ssize_t dst_off000 = ((0 * OC + g * OCOG + oc) * OH + 0) * OW + 0;
        for (ssize_t mb = 0; mb < MB; ++mb) {
          //const ssize_t dst_off00 = ((mb * OC + g * OCOG + oc) * OH + 0) * OW + 0;
          for (ssize_t oh = 0; oh < OH; ++oh) {
            for (ssize_t ow = 0; ow < OW; ++ow) {
              //pdiff_bia[bia_off] += pdiff_dst[dst_off00 + oh*OW + ow];
              pdiff_bia[bia_off] += pdiff_dst[dst_off000 + mb*OC*OH*OW + oh*OW + ow];
            }
          }
        }
      }
    }
  }
#elif 0 // BWD_W SX 24.2x FAIL
  // loop order change, much better 24x for regr.sh BWD_WB (x86:7.9,19.1)
  // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
  OMP(parallel for collapse(4))//;
  for (ssize_t g = 0; g < G; ++g) {
    for (ssize_t oc = 0; oc < OCOG; ++oc) {
      ShortLoop() for (ssize_t kh = 0; kh < p->kh; ++kh) {
        ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
          const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;
#if 1 // SX 24.5x BWD_WB regr.sh  x86:7.6,13.2,1.73
          ssize_t oh_beg, oh_end;
          oh_beg = div_floor(      SH - ih0 - 1, SH);//(c-a+b-1)/b
          oh_end = div_floor( IH + SH - ih0 - 1, SH);//(d-a+b-1)/b
          if (oh_beg < 0    ) oh_beg = 0;
          if (oh_end > OH) oh_end = OH;
          ssize_t ow_beg, ow_end;
          ow_beg = div_floor(    + PW - kw*DW + SW - 1, SW);//(c-a+b-1)/b
          ow_end = div_floor( IW + PW - kw*DW + SW - 1, SW);//(d-a+b-1)/b
          if (ow_beg < 0    ) ow_beg = 0;
          if (ow_end > OW) ow_end = OW;
          //const size_t iw_beg = ow_beg * SW - PW + kw * DW;
#elif 1 // SX 24.3x  x86:6.9,16.2 // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< XXX x86 XXX
          // equiv, but OK for unsigned hoist_t and "normal" division op
          typedef ssize_t hoist_t; /*but it could even be unsigned int, now*/
          hoist_t oh_beg = 0, oh_end=0;
          if( kh*DH < PH ) oh_beg = ((PH-kh*DH) + SH-1)/ SH;
          if( kh*DH < IH+PH ) oh_end = ((IH + PH - kh*DH) + SH-1) / SH;
          if( oh_end >= OH ) oh_end = OH;
          hoist_t ow_beg = 1, ow_end=0;
          if( kw*DW < PW ) ow_beg = ((PW-kw*DW) + SW-1)/ SW;
          if( kw*DW < IW+PW ) ow_end = ((IW + PW - kw*DW) + SW-1) / SW;
          if( ow_end >= OW ) ow_end = OW;
#elif 0 // SX 24.1x  x86:6.8,12.3
          const ssize_t oh_beg = ( kh*DH < PH ? ((PH-kh*DH) + SH-1)/ SH : 0 );
          ssize_t oh_end = ( kh*DH < IH+PH ? ((IH + PH - kh*DH) + SH-1) / SH : 0 );
          if( oh_end >= OH ) oh_end = OH;
          const ssize_t ow_beg = ( kw*DW < PW ? ((PW-kw*DW) + SW-1)/ SW : 0 );
          ssize_t ow_end = ( kw*DW < IW+PW ? ((IW + PW - kw*DW) + SW-1) / SW : 0 );
          if( ow_end >= OW ) ow_end = OW;
#endif
          float dw_ic[ICOG];
          for (ssize_t ic = 0; ic < ICOG; ++ic) dw_ic[ic] = 0.f;
          if (ow_beg < ow_end && oh_beg < oh_end) {
            ow_end -= ow_beg;
            oh_end -= oh_beg;
            const ssize_t d0 = ((0 * OC + g * OCOG + oc) * OH + oh_beg) * OW + ow_beg;
            const ssize_t s00 = ((0 * IC + g * ICOG + 0 ) * IH + (ih0+oh_beg*SH)) * IW + ow_beg*SW - PW + kw*DW;
            for (ssize_t mb = 0; mb < MB; ++mb) {
              const ssize_t dst_off_beg = d0 + mb * OC*OH*OW;
              const ssize_t s0 = s00 + mb*IC*IH*IW;
              for (ssize_t oh = 0; oh < oh_end; ++oh) {
                for (size_t ow = 0; ow < ow_end; ++ow) {
                  for (ssize_t ic = 0; ic < ICOG; ++ic) {
                    // pdiff_dst is always readable, even if not used
                    dw_ic[ic] += psrc[s0+ic*IH*IW + oh*SH_IW + ow*SW] * pdiff_dst[dst_off_beg+oh*OW+ow];
                  }
                }
              }
            }
          }
          const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
          for (ssize_t ic = 0; ic < ICOG; ++ic){
            pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
          }

        }
      }
    }
  }
  if ((p->dir & FLAG_BIA)) {
    OMP(for collapse(2) nowait)//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        const ssize_t bia_off = bia_off_f(p, g, oc);
        pdiff_bia[bia_off] = 0.f;
        const ssize_t dst_off000 = ((0 * OC + g * OCOG + oc) * OH + 0) * OW + 0;
        for (ssize_t mb = 0; mb < MB; ++mb) {
          //const ssize_t dst_off00 = ((mb * OC + g * OCOG + oc) * OH + 0) * OW + 0;
          for (ssize_t oh = 0; oh < OH; ++oh) {
            for (ssize_t ow = 0; ow < OW; ++ow) {
              //pdiff_bia[bia_off] += pdiff_dst[dst_off00 + oh*OW + ow];
              pdiff_bia[bia_off] += pdiff_dst[dst_off000 + mb*OC*OH*OW + oh*OW + ow];
            }
          }
        }
      }
    }
  }
#elif 0 // A1,A3 SX 1.5,1.1     x86:6.9,13.1
  // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
  OMP(parallel for collapse(4))//;
  for (ssize_t g = 0; g < G; ++g) {
    for (ssize_t oc = 0; oc < OCOG; ++oc) {
      ShortLoop() for (ssize_t kh = 0; kh < p->kh; ++kh) {
        ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
          const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;

          // conditionals --> loop limit calculations   "hoisting"
          // equiv, but OK for unsigned hoist_t and "normal" division op
          typedef ssize_t hoist_t; /*but it could even be unsigned int, now*/
          hoist_t oh_beg = 0, oh_end=0;
          if( kh*DH < PH ) oh_beg = ((PH-kh*DH) + SH-1)/ SH;
          if( kh*DH < IH+PH ) oh_end = ((IH + PH - kh*DH) + SH-1) / SH;
          if( oh_end >= OH ) oh_end = OH;
          hoist_t ow_beg = 0, ow_end=0;
          if( kw*DW < PW ) ow_beg = ((PW-kw*DW) + SW-1)/ SW;
          if( kw*DW < IW+PW ) ow_end = ((IW + PW - kw*DW) + SW-1) / SW;
          if( ow_end >= OW ) ow_end = OW;

          float dw_ic[ICOG];
          for (ssize_t ic = 0; ic < ICOG; ++ic) dw_ic[ic] = 0.f;
          if (ow_beg < ow_end && oh_beg < oh_end) {
            ow_end -= ow_beg;
            oh_end -= oh_beg;
            const ssize_t d0 = ((0 * OC + g * OCOG + oc) * OH + oh_beg) * OW + ow_beg;
            const ssize_t s00 = ((0 * IC + g * ICOG + 0 ) * IH + (ih0+oh_beg*SH)) * IW + ow_beg*SW - PW + kw*DW;
            for (ssize_t mb = 0; mb < MB; ++mb) {
              const ssize_t dst_off_beg = d0 + mb * OC*OH*OW;
              const ssize_t s0 = s00 + mb*IC*IH*IW;
              for (ssize_t oh = 0; oh < oh_end; ++oh) {
                for (size_t ow = 0; ow < ow_end; ++ow) {
                  for (ssize_t ic = 0; ic < ICOG; ++ic) {
                    // pdiff_dst is always readable, even if not used
                    dw_ic[ic] += psrc[s0+ic*IH*IW + oh*SH_IW + ow*SW] * pdiff_dst[dst_off_beg+oh*OW+ow];
                  }
                }
              }
            }
          }
          const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
          for (ssize_t ic = 0; ic < ICOG; ++ic){
            pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
          }

        }
      }
    }
  }
  bwd_w_bias_update( p, diff_bia_m, diff_dst_m);
#elif 0 // A1,A3 SX 1.5,1.1       x86:HORRIBLE
  // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
  OMP(parallel)//;
  {
    bool ook[OH*OW];
RETAIN(ook)
    int ohw_off[OH*OW];
RETAIN(ohw_off)
    float tmpsrc[OH*OW];
#ifdef __ve
    VREG(tmpsrc)
#else
    ALLOC_ON_VREG(tmpsrc)
#endif
    float dw_ic[ICOG];
#ifdef __ve
    VREG(dw_ic)
#else
    ALLOC_ON_VREG(dw_ic)
#endif
    ssize_t ohash;
    ssize_t ohash_prv = -1;
    float ohw_begend[4];
#ifdef __ve
    VREG(ohw_begend)
#else
        VREG(ohw_begend)
#endif
    float ohw_muls[4] = {1, OH, (1<<16), (1<<16)*OW};
#ifdef __ve
    VREG(ohw_muls)
#else
        VREG(ohw_muls)
#endif

    OMP(parallel for collapse(4))//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        ShortLoop() for (ssize_t kh = 0; kh < p->kh; ++kh) {
          ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
            const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;

            // conditionals --> loop limit calculations   "hoisting"
            // equiv, but OK for unsigned hoist_t and "normal" division op
            typedef ssize_t hoist_t; /*but it could even be unsigned int, now*/
            hoist_t oh_beg = 0, oh_end=0;
            if( kh*DH < PH ) oh_beg = ((PH-kh*DH) + SH-1)/ SH;
            if( kh*DH < IH+PH ) oh_end = ((IH + PH - kh*DH) + SH-1) / SH;
            if( oh_end >= OH ) oh_end = OH;
            hoist_t ow_beg = 0, ow_end=0;
            if( kw*DW < PW ) ow_beg = ((PW-kw*DW) + SW-1)/ SW;
            if( kw*DW < IW+PW ) ow_end = ((IW + PW - kw*DW) + SW-1) / SW;
            if( ow_end >= OW ) ow_end = OW;

            //float dw_ic[ICOG];
            for (ssize_t ic = 0; ic < ICOG; ++ic) dw_ic[ic] = 0.f;
            if (ow_beg < ow_end && oh_beg < oh_end) {
              //ow_end -= ow_beg;
              //oh_end -= oh_beg;
              //const ssize_t d0 = ((0 * OC + g * OCOG + oc) * OH + oh_beg) * OW + ow_beg;
              //const ssize_t s00 = ((0 * IC + g * ICOG + 0 ) * IH + (ih0+oh_beg*SH)) * IW + ow_beg*SW - PW + kw*DW;
              const ssize_t d0 = ((0 * OC + g * OCOG + oc) * OH + 0) * OW + 0;
              const ssize_t s00 = ((0 * IC + g * ICOG + 0 ) * IH + (ih0+0*SH)) * IW + 0*SW - PW + kw*DW;

              ohw_begend[0] = oh_beg;
              ohw_begend[1] = oh_end;
              ohw_begend[2] = ow_beg;
              ohw_begend[3] = ow_end;
              ohash = 0;
              for (size_t i=0; i<4; ++i)
                ohash += ohw_begend[i] * ohw_muls[i];
              if (ohash != ohash_prv){
                ohash_prv = ohash;
                //bool ook[OH*OW];
                //ssize_t ohw_off[OH*OW]
                for (ssize_t oh = 0; oh < OH; ++oh) {
                  for (size_t ow = 0; ow < OW; ++ow) {
                    //ook[oh*OW+ow] = (oh>=oh_beg && ow>=ow_beg) && (oh<oh_end && ow<ow_end);
                    if ( (oh>=oh_beg && ow>=ow_beg) && (oh<oh_end && ow<ow_end) )
                      ohw_off[oh*OW+ow] = (int)(oh*SH_IW + ow*SW);
                    else
                      ohw_off[oh*OW+ow] = -1;
                  }
                }
              }

              for (ssize_t mb = 0; mb < MB; ++mb) {
                const ssize_t dst_off_beg = d0 + mb * OC*OH*OW;
                const ssize_t s0 = s00 + mb*IC*IH*IW;
                for (ssize_t ic = 0; ic < ICOG; ++ic) {
                  for (ssize_t oh = 0; oh < OH; ++oh) {
                    for (size_t ow = 0; ow < OW; ++ow) {
                      // is pdiff_dst readable, even if not used ? YES (as suspected)
                      // yes RT_ASSERT( dst_off_beg + oh*OW + ow >= 0 );
                      // yes RT_ASSERT( dst_off_beg + oh*OW + ow < MB*G*OCOG*OH*OW );
                      // yes RT_ASSERT( s0+ic*IH*IW + oh*SH_IW + ow*SW >= 0 );
                      // FALSE ... RT_ASSERT( s0+ic*IH*IW + oh*SH_IW + ow*SW < MB*G*ICOG*IH*IW );
                      //if( oh>=oh_beg && ow>=ow_beg && oh<oh_end && ow<ow_end ){
                      //  dw_ic[ic] += psrc[s0+ic*IH*IW + oh*SH_IW + ow*SW] * pdiff_dst[dst_off_beg+oh*OW+ow];
                      //}
#if 1
                      if( ohw_off[oh*OW+ow] >= 0 ){
                        // fastest ?
                        dw_ic[ic] += psrc[s0+ic*IH*IW + oh*SH_IW + ow*SW] * pdiff_dst[dst_off_beg+oh*OW+ow];
                        //dw_ic[ic] += psrc[s00+mb*IC*IH*IW +ic*IW*IH+ ohw_off[oh*OW+ow]] * pdiff_dst[dst_off_beg+oh*OW+ow];
                        //dw_ic[ic] += psrc[s0+ic*IH*IW + ohw_off[oh*OW+ow]] * pdiff_dst[dst_off_beg+oh*OW+ow];
                      }
#else // slower
                      if( ohw_off[oh*OW+ow] >= 0 ){
                        tmpsrc[oh*OW+ow] = psrc[s00+mb*IC*IH*IW +ic*IW*IH+ ohw_off[oh*OW+ow]];
                      }else{
                        tmpsrc[oh*OW+ow] = 0.f;
                      }
                      dw_ic[ic] += tmpsrc[oh*OW+ow] * pdiff_dst[dst_off_beg+oh*OW+ow];
#endif

                    }
                  }
                }
              }
            }
            const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
            for (ssize_t ic = 0; ic < ICOG; ++ic){
              pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
            }
          }
        }
      }
    }
  }
  bwd_w_bias_update( p, diff_bia_m, diff_dst_m);
#elif 0 // A1,A3,BWD_WB SX 13.8,13.8,14    x86:HORRIBLE
  // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
  OMP(parallel)//;
  {
    bool ook[OH*OW];
RETAIN(ook)
    int ohw_off[OH*OW];
RETAIN(ohw_off)
    float tmpsrc[OH*OW];
#ifdef __ve
    VREG(tmpsrc)
#else
    ALLOC_ON_VREG(tmpsrc)
#endif
    float dw_ic[ICOG];
#ifdef __ve
    VREG(dw_ic)
#else
    ALLOC_ON_VREG(dw_ic)
#endif
    ssize_t ohash;
    ssize_t ohash_prv = -1;
    float ohw_begend[4];
#ifdef __ve
    VREG(ohw_begend)
#else
        VREG(ohw_begend)
#endif
    float ohw_muls[4] = {1, OH, (1<<16), (1<<16)*OW};
#ifdef __ve
    VREG(ohw_muls)
#else
        VREG(ohw_muls)
#endif

    OMP(parallel for collapse(4))//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        ShortLoop() for (ssize_t kh = 0; kh < p->kh; ++kh) {
          ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
            const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;

            // conditionals --> loop limit calculations   "hoisting"
            // equiv, but OK for unsigned hoist_t and "normal" division op
            typedef ssize_t hoist_t; /*but it could even be unsigned int, now*/
            hoist_t oh_beg = 0, oh_end=0;
            if( kh*DH < PH ) oh_beg = ((PH-kh*DH) + SH-1)/ SH;
            if( kh*DH < IH+PH ) oh_end = ((IH + PH - kh*DH) + SH-1) / SH;
            if( oh_end >= OH ) oh_end = OH;
            hoist_t ow_beg = 0, ow_end=0;
            if( kw*DW < PW ) ow_beg = ((PW-kw*DW) + SW-1)/ SW;
            if( kw*DW < IW+PW ) ow_end = ((IW + PW - kw*DW) + SW-1) / SW;
            if( ow_end >= OW ) ow_end = OW;

            for (ssize_t ic = 0; ic < ICOG; ++ic) dw_ic[ic] = 0.f;
            if (ow_beg < ow_end && oh_beg < oh_end) {
              const ssize_t d0 = ((0 * OC + g * OCOG + oc) * OH + 0) * OW + 0;
              const ssize_t s00 = ((0 * IC + g * ICOG + 0 ) * IH + (ih0+0*SH)) * IW + 0*SW - PW + kw*DW;

              ohw_begend[0] = oh_beg; ohw_begend[1] = oh_end; ohw_begend[2] = ow_beg; ohw_begend[3] = ow_end;
              ohash = 0; for (size_t i=0; i<4; ++i) ohash += ohw_begend[i] * ohw_muls[i];
              if (ohash != ohash_prv){
                ohash_prv = ohash;
                for (ssize_t oh = 0; oh < OH; ++oh) {
                  for (size_t ow = 0; ow < OW; ++ow) {
                    if ( (oh>=oh_beg && ow>=ow_beg) && (oh<oh_end && ow<ow_end) )
                      ohw_off[oh*OW+ow] = (int)(oh*SH_IW + ow*SW);
                    else
                      ohw_off[oh*OW+ow] = -1;
                  }
                }
              }

              for (ssize_t mb = 0; mb < MB; ++mb) {
                const ssize_t dst_off_beg = d0 + mb * OC*OH*OW;
                const ssize_t s0 = s00 + mb*IC*IH*IW;
                for (ssize_t ic = 0; ic < ICOG; ++ic) {
                  for (ssize_t ohw = 0; ohw< OH_OW; ++ohw) {
                    if( ohw_off[ohw] >= 0 ){
                      // fastest ?
                      dw_ic[ic] += psrc[s0+ic*IH*IW + ohw_off[ohw]] * pdiff_dst[dst_off_beg+ohw];
                    }
                  }
                }
              }
            }
            const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
            for (ssize_t ic = 0; ic < ICOG; ++ic){
              pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
            }
          }
        }
      }
    }
  }
  bwd_w_bias_update( p, diff_bia_m, diff_dst_m);
#elif 0 //
  // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
  OMP(parallel)//;
  {
    int ohw_off[OH*OW];
RETAIN(ohw_off)
    float tmpsrc[OH*OW];
#ifdef __ve
    VREG(tmpsrc)
#else
    ALLOC_ON_VREG(tmpsrc)
#endif
    float dw_ic[ICOG];
#ifdef __ve
    VREG(dw_ic)
#else
    ALLOC_ON_VREG(dw_ic)
#endif
    ssize_t ohash;
    ssize_t ohash_prv = -1;
    float ohw_begend[4];
#ifdef __ve
    VREG(ohw_begend)
#else
        VREG(ohw_begend)
#endif
    float ohw_muls[4] = {1, OH, (1<<16), (1<<16)*OW};
#ifdef __ve
    VREG(ohw_muls)
#else
        VREG(ohw_muls)
#endif

    OMP(parallel for collapse(4))//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        ShortLoop() for (ssize_t kh = 0; kh < p->kh; ++kh) {
          ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
            const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;

            // conditionals --> loop limit calculations   "hoisting"
            // equiv, but OK for unsigned hoist_t and "normal" division op
            typedef ssize_t hoist_t; /*but it could even be unsigned int, now*/
            hoist_t oh_beg = 0, oh_end=0;
            if( kh*DH < PH ) oh_beg = ((PH-kh*DH) + SH-1)/ SH;
            if( kh*DH < IH+PH ) oh_end = ((IH + PH - kh*DH) + SH-1) / SH;
            if( oh_end >= OH ) oh_end = OH;
            hoist_t ow_beg = 0, ow_end=0;
            if( kw*DW < PW ) ow_beg = ((PW-kw*DW) + SW-1)/ SW;
            if( kw*DW < IW+PW ) ow_end = ((IW + PW - kw*DW) + SW-1) / SW;
            if( ow_end >= OW ) ow_end = OW;

            for (ssize_t ic = 0; ic < ICOG; ++ic) dw_ic[ic] = 0.f;
            if (ow_beg < ow_end && oh_beg < oh_end) {
              const ssize_t d0 = ((0 * OC + g * OCOG + oc) * OH + 0) * OW + 0;
              const ssize_t s00 = ((0 * IC + g * ICOG + 0 ) * IH + (ih0+0*SH)) * IW + 0*SW - PW + kw*DW;

              ohw_begend[0] = oh_beg; ohw_begend[1] = oh_end; ohw_begend[2] = ow_beg; ohw_begend[3] = ow_end;
              ohash = 0; for (size_t i=0; i<4; ++i) ohash += ohw_begend[i] * ohw_muls[i];
              if (ohash != ohash_prv){
                ohash_prv = ohash;
                for (ssize_t oh = 0; oh < OH; ++oh) {
                  for (size_t ow = 0; ow < OW; ++ow) {
                    if ( (oh>=oh_beg && ow>=ow_beg) && (oh<oh_end && ow<ow_end) )
                      ohw_off[oh*OW+ow] = (int)(oh*SH_IW + ow*SW);
                    else
                      ohw_off[oh*OW+ow] = -1;
                  }
                }
              }

              for (ssize_t mb = 0; mb < MB; ++mb) {
                const ssize_t dst_off_beg = d0 + mb * OC*OH*OW;
                const ssize_t s0 = s00 + mb*IC*IH*IW;
                for (ssize_t ic = 0; ic < ICOG; ++ic) {
                  for (ssize_t ohw = 0; ohw< OH_OW; ++ohw) {
#if 1 // x86:HORRIBLE
                    if( ohw_off[ohw] >= 0 ){
                      dw_ic[ic] += psrc[s0+ic*IH*IW + ohw_off[ohw]] * pdiff_dst[dst_off_beg+ohw];
                    }
#elif 0 // SX:half-speed
                    tmpsrc[ohw] = (ohw_off[ohw] >= 0? psrc[s0+ic*IH*IW + ohw_off[ohw]]: 0.f);
                    dw_ic[ic] += tmpsrc[ohw] * pdiff_dst[dst_off_beg+ohw];
#elif 1 // very slightly slower, but allows checking loop re-orderings
                    if( ohw_off[ohw] >= 0 ){
                      dw_ic[ic] += psrc[s00+(mb*IC+ic)*IH_IW + ohw_off[ohw]] * pdiff_dst[d0+mb*OC_OH_OW+ohw];
                    }
#endif
                  }
                }
              }
            }
            const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
            for (ssize_t ic = 0; ic < ICOG; ++ic){
              pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
            }
          }
        }
      }
    }
  }
  bwd_w_bias_update( p, diff_bia_m, diff_dst_m);
#elif 0 // experimental
  // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
  OMP(parallel)//;
  {
    int ohw_off[OH*OW];
RETAIN(ohw_off)
    float tmpsrc[OH*OW];
#ifdef __ve
    VREG(tmpsrc)
#else
    ALLOC_ON_VREG(tmpsrc)
#endif
    float dw_ic[ICOG];
#ifdef __ve
    VREG(dw_ic)
#else
    ALLOC_ON_VREG(dw_ic)
#endif
    ssize_t ohash;
    ssize_t ohash_prv = -1;
    float ohw_begend[4];
#ifdef __ve
    VREG(ohw_begend)
#else
        VREG(ohw_begend)
#endif
    float ohw_muls[4] = {1, OH, (1<<16), (1<<16)*OW};
#ifdef __ve
    VREG(ohw_muls)
#else
    VREG(ohw_muls)
#endif

    OMP(parallel for collapse(4))//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        ShortLoop() for (ssize_t kh = 0; kh < p->kh; ++kh) {
          ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
            const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;

            // conditionals --> loop limit calculations   "hoisting"
            // equiv, but OK for unsigned hoist_t and "normal" division op
            typedef ssize_t hoist_t; /*but it could even be unsigned int, now*/
            hoist_t oh_beg = 0, oh_end=0;
            if( kh*DH < PH ) oh_beg = ((PH-kh*DH) + SH-1)/ SH;
            if( kh*DH < IH+PH ) oh_end = ((IH + PH - kh*DH) + SH-1) / SH;
            if( oh_end >= OH ) oh_end = OH;
            hoist_t ow_beg = 0, ow_end=0;
            if( kw*DW < PW ) ow_beg = ((PW-kw*DW) + SW-1)/ SW;
            if( kw*DW < IW+PW ) ow_end = ((IW + PW - kw*DW) + SW-1) / SW;
            if( ow_end >= OW ) ow_end = OW;

            for (ssize_t ic = 0; ic < ICOG; ++ic) dw_ic[ic] = 0.f;
            if (ow_beg < ow_end && oh_beg < oh_end) {
              const ssize_t d0 = ((0 * OC + g * OCOG + oc) * OH + 0) * OW + 0;
              const ssize_t s00 = ((0 * IC + g * ICOG + 0 ) * IH + (ih0+0*SH)) * IW + 0*SW - PW + kw*DW;

              ohw_begend[0] = oh_beg; ohw_begend[1] = oh_end; ohw_begend[2] = ow_beg; ohw_begend[3] = ow_end;
              ohash = 0; for (size_t i=0; i<4; ++i) ohash += ohw_begend[i] * ohw_muls[i];
              if (ohash != ohash_prv){
                ohash_prv = ohash;
                for (ssize_t oh = 0; oh < OH; ++oh) {
                  for (size_t ow = 0; ow < OW; ++ow) {
                    if ( (oh>=oh_beg && ow>=ow_beg) && (oh<oh_end && ow<ow_end) )
                      ohw_off[oh*OW+ow] = (int)(oh*SH_IW + ow*SW);
                    else
                      ohw_off[oh*OW+ow] = -1;
                  }
                }
              }
#if 1
              ssize_t src_mbic[MB*ICOG];
              for (ssize_t mb = 0; mb < MB; ++mb) {
                for (ssize_t ic = 0; ic < ICOG; ++ic) {
                  src_mbic[mb*IC+ic] = s00 + (mb*IC+ic)*IH_IW;
                }
              }
#endif

              // mb, ohw, ic SLOW
              for (ssize_t mb = 0; mb < MB; ++mb) {
                for (ssize_t ic = 0; ic < ICOG; ++ic) {
                  for (ssize_t ohw = 0; ohw< OH_OW; ++ohw) {
#if 0
                    if( ohw_off[ohw] >= 0 ){
                      dw_ic[ic] += psrc[s00+(mb*IC+ic)*IH_IW + ohw_off[ohw]] * pdiff_dst[d0+mb*OC_OH_OW+ohw];
                    }
#elif 1
                    if( ohw_off[ohw] >= 0 ){
                      dw_ic[ic] += psrc[src_mbic[mb*IC+ic] + ohw_off[ohw]] * pdiff_dst[d0+mb*OC_OH_OW+ohw];
                    }
#else
                    tmpsrc[ohw] = (ohw_off[ohw] >= 0? psrc[s00+(mb*IC+ic)*IH_IW + ohw_off[ohw]]: 0.f);
                      dw_ic[ic] += tmpsrc[ohw] * pdiff_dst[d0+mb*OC_OH_OW+ohw];
#endif
                  }
                }
              }
            }
            const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
            for (ssize_t ic = 0; ic < ICOG; ++ic){
              pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
            }
          }
        }
      }
    }
  }
  bwd_w_bias_update( p, diff_bia_m, diff_dst_m);
#elif 1 // cleaned up, SX ~ 11x speedup.    SUCKS on x86 with gcc (0.34x speedup)!
  // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
  OMP(parallel)//;
  {
    int ohw_off[OH*OW];
    RETAIN(ohw_off)
    //int icohw_off[ICOG*OH*OW]
    float tmpsrc[OH*OW];
    //ALLOC_ON_VREG(tmpsrc)
    float dw_ic[ICOG];
#ifdef __ve
    VREG(dw_ic)
#else
    ALLOC_ON_VREG(dw_ic)
#endif
    ssize_t ohash;
    ssize_t ohash_prv = -1;
    float ohw_begend[4];
#ifdef __ve
    VREG(ohw_begend)
#else
        VREG(ohw_begend)
#endif
    float ohw_muls[4] = {1, OH, (1<<16), (1<<16)*OW};
#ifdef __ve
    VREG(ohw_muls)
#else
        VREG(ohw_muls)
#endif

    OMP(parallel for collapse(4))//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        ShortLoop() for (ssize_t kh = 0; kh < p->kh; ++kh) {
          ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
            const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;

            // conditionals --> loop limit calculations   "hoisting"
            // equiv, but OK for unsigned hoist_t and "normal" division op
            //   kh,kw + constants ---> oh/ow_beg/end
            typedef ssize_t hoist_t; /*but it could even be unsigned int, now*/
            hoist_t oh_beg = 0, oh_end=0;
            //if( kh*DH < PH ) oh_beg = ((PH-kh*DH) + SH-1)/ SH;
            //if( kh*DH < IH+PH ) oh_end = ((IH + PH - kh*DH) + SH-1) / SH;
            if( kh*DH < PH ) oh_beg =    (   - ih0 + SH - 1)/ SH;
            if( kh*DH < IH+PH ) oh_end = (IH - ih0 + SH - 1) / SH;
            if( oh_end >= OH ) oh_end = OH;
            hoist_t ow_beg = 0, ow_end=0;
            if( kw*DW < PW ) ow_beg =    ((     PW - kw*DW) + SW-1)/ SW;
            if( kw*DW < IW+PW ) ow_end = ((IW + PW - kw*DW) + SW-1) / SW;
            if( ow_end >= OW ) ow_end = OW;

            for (ssize_t ic = 0; ic < ICOG; ++ic) dw_ic[ic] = 0.f;
            if (ow_beg < ow_end && oh_beg < oh_end) {
              const ssize_t d0 = ((0 * OC + g * OCOG + oc) * OH + 0) * OW + 0;
              const ssize_t s00 = ((0 * IC + g * ICOG + 0 ) * IH + (ih0+0*SH)) * IW + 0*SW - PW + kw*DW;

              ohw_begend[0] = oh_beg; ohw_begend[1] = oh_end; ohw_begend[2] = ow_beg; ohw_begend[3] = ow_end;
              ohash = 0; for (size_t i=0; i<4; ++i) ohash += ohw_begend[i] * ohw_muls[i];
              if (ohash != ohash_prv){
                ohash_prv = ohash;
                for (ssize_t oh = 0; oh < OH; ++oh) {
                  for (size_t ow = 0; ow < OW; ++ow) {
                    if ( (oh>=oh_beg && ow>=ow_beg) && (oh<oh_end && ow<ow_end) )
                      ohw_off[oh*OW+ow] = (int)(oh*SH_IW + ow*SW);
                    else
                      ohw_off[oh*OW+ow] = -1;
                  }
                }
              }

              // mb, ohw, ic SLOW
              for (ssize_t mb = 0; mb < MB; ++mb) {
                for (ssize_t ic = 0; ic < ICOG; ++ic) {
                  for (ssize_t ohw = 0; ohw< OH_OW; ++ohw) {
#if 1 || defined(_SX)
                    if( ohw_off[ohw] >= 0 ){
                      dw_ic[ic] += psrc[s00+(mb*IC+ic)*IH_IW + ohw_off[ohw]] * pdiff_dst[d0+mb*OC_OH_OW+ohw];
                    }
#elif 1 // slightly slower
                      dw_ic[ic] += (ohw_off[ohw] >= 0? psrc[s00+(mb*IC+ic)*IH_IW + ohw_off[ohw]]: 0.f)
                        * pdiff_dst[d0+mb*OC_OH_OW+ohw];
#endif
                  }
                }
              }
            }
            const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
            for (ssize_t ic = 0; ic < ICOG; ++ic){
              pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
            }
          }
        }
      }
    }
  }
  bwd_w_bias_update( p, diff_bia_m, diff_dst_m);
#elif 0 // illustrative SLOW
  // writing to dw at wei_off_f(p, g, oc, ic, kh, kw);
  OMP(parallel for collapse(4))//;
  for (ssize_t g = 0; g < G; ++g) {
    for (ssize_t oc = 0; oc < OCOG; ++oc) {
      ShortLoop() for (ssize_t kh = 0; kh < p->kh; ++kh) {
        ShortLoop() for (ssize_t kw = 0; kw < KW; ++kw) {
          const ssize_t ih0 = /*0 * SH*/ - PH + kh * DH;

          // conditionals --> loop limit calculations   "hoisting"
          // equiv, but OK for unsigned hoist_t and "normal" division op
          typedef ssize_t hoist_t; /*but it could even be unsigned int, now*/
          hoist_t oh_beg = 0, oh_end=0;
          if( kh*DH < PH ) oh_beg = ((PH-kh*DH) + SH-1)/ SH;
          if( kh*DH < IH+PH ) oh_end = ((IH + PH - kh*DH) + SH-1) / SH;
          if( oh_end >= OH ) oh_end = OH;
          hoist_t ow_beg = 0, ow_end=0;
          if( kw*DW < PW ) ow_beg = ((PW-kw*DW) + SW-1)/ SW;
          if( kw*DW < IW+PW ) ow_end = ((IW + PW - kw*DW) + SW-1) / SW;
          if( ow_end >= OW ) ow_end = OW;

          float dw_ic[ICOG];
          for (ssize_t ic = 0; ic < ICOG; ++ic) dw_ic[ic] = 0.f;

          if (ow_beg < ow_end && oh_beg < oh_end) {
            ow_end -= ow_beg;
            oh_end -= oh_beg;
            const ssize_t d0 = ((0 * OC + g * OCOG + oc) * OH + oh_beg) * OW + ow_beg;
            const ssize_t s00 = ((0 * IC + g * ICOG + 0 ) * IH + (ih0+oh_beg*SH)) * IW + ow_beg*SW - PW + kw*DW;

            bool ook[OH][OW];
            for (ssize_t oh = 0; oh < oh_end; ++oh) {
              for (size_t ow = 0; ow < ow_end; ++ow) {
                //six[ic*oh_end*ow_end + oh*ow_end + ow] = ic*IH*IW + oh*SH_IW + ow*SW;
                ook[oh][ow] = (oh>=oh_beg && ow>=ow_beg) && (oh<oh_end && ow<ow_end);
              }
            }
            ssize_t six[ICOG*OH*OW];
            //float src[ICOG*OH*OW];
            for (ssize_t ic = 0; ic < ICOG; ++ic) {
              for (ssize_t oh = 0; oh < oh_end; ++oh) {
                for (size_t ow = 0; ow < ow_end; ++ow) {
                  six[ic*oh_end*ow_end + oh*ow_end + ow] = ic*IH*IW + oh*SH_IW + ow*SW;
                  //src[ic*oh_end*ow_end + oh*ow_end + ow] = psrc[s0+ic*IH*IW + oh*SH_IW + ow*SW];
                  //  ooops s0 depends on mb!
                }
              }
            }
            for (ssize_t mb = 0; mb < MB; ++mb) {
              const ssize_t dst_off_beg = d0 + mb * OC*OH*OW;
              const ssize_t s0 = s00 + mb*IC*IH*IW;
              for (ssize_t ic = 0; ic < ICOG; ++ic) {
                for (ssize_t oh = 0; oh < oh_end; ++oh) {
                  for (size_t ow = 0; ow < ow_end; ++ow) {
                    // pdiff_dst is always readable, even if not used
                    //dw_ic[ic] += psrc[s0+ic*IH*IW + oh*SH_IW + ow*SW] * pdiff_dst[dst_off_beg+oh*OW+ow];
                    dw_ic[ic] += psrc[s0+six[ic*oh_end*ow_end+oh*ow_end+ow]] * pdiff_dst[dst_off_beg+oh*OW+ow];

                    //src[ic*oh_end*ow_end + oh*ow_end + ow] = psrc[s0+ic*IH*IW + oh*SH_IW + ow*SW];
                    //dw_ic[ic] += src[ic*oh_end*ow_end+oh*ow_end+ow] * pdiff_dst[dst_off_beg+oh*OW+ow];
                  }
                }
              }
            }
          }
          const ssize_t wei_off0 = wei_off_f2(p, g, oc, 0, kh, kw); // ic=0
          for (ssize_t ic = 0; ic < ICOG; ++ic){
            pdiff_wei[wei_off0 + ic*KH_KW] = dw_ic[ic];
          }

        }
      }
    }
  }
  bwd_w_bias_update( p, diff_bia_m, diff_dst_m);
#elif 0 // from ref_conv4 -- slightly flawed ! mb loop CANNOT be outside omp.
#if 0
  OMP(for collapse(3))//;
  for (ssize_t g = 0; g < G; ++g) {
    for (ssize_t oc = 0; oc < OCOG; ++oc) {
      for (ssize_t ic = 0; ic < ICOG; ++ic) {
        const ssize_t wei_off00 = (((g * OCOG + oc) * ICOG + ic) * KH + 0) * KW + 0;
        for (ssize_t kh = 0; kh < KH; ++kh) {
          for (ssize_t kw = 0; kw < KW; ++kw) {
            pdiff_wei[wei_off00 + kh*KW + kw] = 0.f;
          }
        }
      }
    }
  }
#else
#define TMP_IC 0
#if 1 || TMP_IC==0
  zero_wei(p, diff_wei_m);
#endif
#endif
  //bwd_w_bias_update(p, diff_bia_m, diff_dst_m);
  for (int mb = 0; mb < MB; ++mb) // XXX <-- take care about zeroing wei
  {
    OMP(parallel)//;
    {
    float tmp[ICOG];
    OMP(parallel for collapse(4))//;
    for (int g = 0; g < G; ++g) {
      for (int oc = 0; oc < OC/G; ++oc) {
        for (int kh = 0; kh < p->kh; ++kh) {
          for (int kw = 0; kw < KW; ++kw) {
            int oh_beg, oh_end;
            oh_beg=div_floor(    + PH - kh * (p->dh + 1) + SH - 1, SH);//(c-a+b-1)/b
            oh_end=div_floor( IH + PH - kh * (p->dh + 1) + SH - 1, SH);//(d-a+b-1)/b
            if (oh_beg < 0    ) oh_beg = 0;
            if (oh_end > OH) oh_end = OH;
            int ow_beg, ow_end;
            ow_beg=div_floor(    + PW - kw * (p->dw + 1) + SW - 1, SW);//(c-a+b-1)/b
            ow_end=div_floor( IW + PW - kw * (p->dw + 1) + SW - 1, SW);//(d-a+b-1)/b
            if (ow_beg < 0    ) ow_beg = 0;
            if (ow_end > OW) ow_end = OW;
#if TMP_IC
            for (int ic = 0; ic < ICOG; ++ic)
              tmp[ic] = 0.f; // MUST always execute!
            //if( oh_beg < oh_end || ow_beg < ow_end )
            if( oh_beg >= oh_end || ow_beg >= ow_end ) continue; // oh, still need to set dw=0
            else
#else
            if( oh_beg >= oh_end || ow_beg >= ow_end ) continue; // oh, still need to set dw=0
            else
#endif
            {
              for (int ic = 0; ic < ICOG; ++ic) { // B
                //size_t wei_off = wei_off_f(p, g, oc, ic, kh, kw); // WRITTEN
                //float &dw = pdiff_wei[wei_off];
                //dw = 0.f; // 2.2x --> 2.0x
                float x=0.f;
                for (int oh = oh_beg; oh < oh_end; ++oh) {
                  const int ih = oh * SH - PH + kh * (p->dh + 1);
                  for (int ow = ow_beg; ow < ow_end; ++ow) {
                    size_t dst_off = dst_off_f(p, mb, g, oc, oh, ow);
                    const int iw = ow * SW - PW + kw * (p->dw + 1);
                    size_t src_off = src_off_f(p, mb, g, ic, ih, iw);
#if 1 || TMP_IC
                    //tmp[ic] += pdiff_dst[dst_off] * psrc[src_off];
                    x  += pdiff_dst[dst_off] * psrc[src_off];
#else
                    dw += pdiff_dst[dst_off] * psrc[src_off];
#endif
                  }
                }
#if 1 || TMP_IC
                tmp[ic] = x;
#else
                tmp[ic] = dw;
#endif
              }
            }
            // Flaw: possible write clash from differing mb's
            //size_t wei_off = wei_off_f2(p, g, oc, 0, kh, kw); // WRITTEN
            for (int ic = 0; ic < ICOG; ++ic) { // B
              size_t wei_off = wei_off_f(p, g, oc, ic, kh, kw); // WRITTEN
              OMP(atomic)//;
              pdiff_wei[wei_off] += tmp[ic];
              //pdiff_wei[wei_off + ic*KH*KW] = tmp[ic];
            }
          }
        }
      }
    }
  }
  }
  if ((p->dir & FLAG_BIA)) {
    OMP(for collapse(2) nowait)//;
    for (ssize_t g = 0; g < G; ++g) {
      for (ssize_t oc = 0; oc < OCOG; ++oc) {
        const ssize_t bia_off = bia_off_f(p, g, oc);
        pdiff_bia[bia_off] = 0.f;
        const ssize_t dst_off000 = ((0 * OC + g * OCOG + oc) * OH + 0) * OW + 0;
        for (ssize_t mb = 0; mb < MB; ++mb) {
          //const ssize_t dst_off00 = ((mb * OC + g * OCOG + oc) * OH + 0) * OW + 0;
          for (ssize_t oh = 0; oh < OH; ++oh) {
            for (ssize_t ow = 0; ow < OW; ++ow) {
              //pdiff_bia[bia_off] += pdiff_dst[dst_off00 + oh*OW + ow];
              pdiff_bia[bia_off] += pdiff_dst[dst_off000 + mb*OC*OH*OW + oh*OW + ow];
            }
          }
        }
      }
    }
  }
#else
#error "select one"
#endif
}

}
// vim: et ts=2 sw=2 cindent nopaste ai cino=^=l0,\:0,N-s
