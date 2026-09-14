#ifndef PDP10_SIXBIT_H
#define PDP10_SIXBIT_H

/*
 * KCC and GCC use different spellings for the PDP-10 six-bit character
 * type.  Keep that compiler detail here so users of SIXBIT() do not need
 * compiler-specific source or build flags.
 */
#if defined(__COMPILER_KCC__)
#define PDP10_CHAR6 _KCCtype_char6
#elif defined(__PDP10__)
#define PDP10_CHAR6 char6
#else
#error "pdp10-sixbit.h requires a PDP-10 target compiler"
#endif

/* Return the first six six-bit characters of a string literal as one
   PDP-10 SIXBIT word.  This constructs data only; it performs no I/O. */
#define SIXBIT(name) (*((int *)((PDP10_CHAR6 *)(name))))

#endif
