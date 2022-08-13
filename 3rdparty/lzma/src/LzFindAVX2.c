/* LzFindAVX2.c -- Stupid workaround for clang-cl being terrible */

#include "Precomp.h"
#include "CpuArch.h"
#include "LzFind.h"

#include <immintrin.h>

#ifdef MY_CPU_X86_OR_AMD64

#define SASUB_256(i) *(__m256i *)(void *)(items + (i) * 8) = _mm256_sub_epi32(_mm256_max_epu32(*(const __m256i *)(const void *)(items + (i) * 8), sub2), sub2); // AVX2

MY_NO_INLINE
void
MY_FAST_CALL
LzFind_SaturSub_256(UInt32 subValue, CLzRef *items, const CLzRef *lim)
{
  __m256i sub2 = _mm256_set_epi32(
      (Int32)subValue, (Int32)subValue, (Int32)subValue, (Int32)subValue,
      (Int32)subValue, (Int32)subValue, (Int32)subValue, (Int32)subValue);
  do
  {
    SASUB_256(0)
    SASUB_256(1)
    items += 2 * 8;
  }
  while (items != lim);
}

#endif
