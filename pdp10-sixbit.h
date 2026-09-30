#ifndef PDP10_SIXBIT_H
#define PDP10_SIXBIT_H

/*
 * Compile-time SIXBIT packing.
 *
 * These macros are ordinary C integer constant expressions and are therefore
 * usable both by PDP-10 target code and by host-side tools/tests.
 */
#define PDP10_SIXCHAR(ch) \
        ((unsigned long)(((unsigned int)(ch) - 040U) & 077U))
#define PDP10_SIX3(a,b,c) \
        ((PDP10_SIXCHAR(a) << 12) | (PDP10_SIXCHAR(b) << 6) | \
        PDP10_SIXCHAR(c))
#define PDP10_SIX6(a,b,c,d,e,f) \
        ((PDP10_SIXCHAR(a) << 30) | (PDP10_SIXCHAR(b) << 24) | \
        (PDP10_SIXCHAR(c) << 18) | (PDP10_SIXCHAR(d) << 12) | \
        (PDP10_SIXCHAR(e) << 6) | PDP10_SIXCHAR(f))

/*
 * SIXBIT() is the convenient string-literal form for PDP-10 target code.
 * KCC and GCC use different spellings for the six-bit character type.
 * Unlike the compile-time packers above, SIXBIT() is not an integer constant
 * expression under KCC and therefore cannot initialize static objects.
 */
#if defined(__COMPILER_KCC__)
#define PDP10_CHAR6 _KCCtype_char6
#define SIXBIT(name) (*((int *)((PDP10_CHAR6 *)(name))))
#elif defined(__PDP10__)
#define PDP10_CHAR6 char6
#define SIXBIT(name) (*((int *)((PDP10_CHAR6 *)(name))))
#endif

#endif
