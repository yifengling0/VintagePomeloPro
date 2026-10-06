/* Build as PE32, then run in a fresh process for option absent, 0, and 1. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#if !defined(__i386__)
#error "This differential probe must be built for a 32-bit x86 guest"
#endif

static void sequence_f32(void) {
  uint16_t before, after, original_cw, masked_cw;
  uint32_t flags_before, flags_after, bits = 0;
  __asm__ volatile("fnstcw %0" : "=m"(original_cw));
  masked_cw = original_cw | 1u;
  __asm__ volatile(
      "fldcw %5\n\t"
      "fnclex\n\t"
      "fldz\n\t"
      "fldz\n\t"
      "fdivp %%st, %%st(1)\n\t" /* 0/0: set sticky IE */
      "fstp %%st(0)\n\t"
      "fnstsw %0\n\t"
      "pushfl\n\tpopl %1\n\t"
      "fld1\n\t"
      "fstps %2\n\t"           /* accepted exact store */
      "pushfl\n\tpopl %3\n\t"
      "fnstsw %4\n\t"
      "fldcw %6\n\t"
      : "=m"(before), "=m"(flags_before), "=m"(bits),
        "=m"(flags_after), "=m"(after)
      : "m"(masked_cw), "m"(original_cw)
      : "memory");
  printf("f32 bits=%08" PRIx32 " ie_before=%u ie_after=%u flags_before=%08" PRIx32
         " flags_after=%08" PRIx32 "\n", bits, before & 1, after & 1,
         flags_before, flags_after);
}

static void sequence_f64(void) {
  uint16_t before, after, original_cw, masked_cw;
  uint32_t flags_before, flags_after;
  uint64_t bits = 0;
  __asm__ volatile("fnstcw %0" : "=m"(original_cw));
  masked_cw = original_cw | 1u;
  __asm__ volatile(
      "fldcw %5\n\t"
      "fnclex\n\t"
      "fldz\n\t"
      "fldz\n\t"
      "fdivp %%st, %%st(1)\n\t"
      "fstp %%st(0)\n\t"
      "fnstsw %0\n\t"
      "pushfl\n\tpopl %1\n\t"
      "fld1\n\t"
      "fstpl %2\n\t"
      "pushfl\n\tpopl %3\n\t"
      "fnstsw %4\n\t"
      "fldcw %6\n\t"
      : "=m"(before), "=m"(flags_before), "=m"(bits),
        "=m"(flags_after), "=m"(after)
      : "m"(masked_cw), "m"(original_cw)
      : "memory");
  printf("f64 bits=%016" PRIx64 " ie_before=%u ie_after=%u flags_before=%08" PRIx32
         " flags_after=%08" PRIx32 "\n", bits, before & 1, after & 1,
         flags_before, flags_after);
}

int main(void) {
  sequence_f32();
  sequence_f64();
  return 0;
}
