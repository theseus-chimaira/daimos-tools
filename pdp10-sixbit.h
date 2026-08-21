#ifndef PDP10_SIXBIT_H
#define PDP10_SIXBIT_H

/* Compile-time construction of PDP-10 SIXBIT words.  No I/O is performed. */
#define PDP10_SIXBIT_CHAR(c) \
    ((((unsigned long)(c)) - 040UL) & 077UL)
#define PDP10_SIXBIT6(a,b,c,d,e,f) \
    ((PDP10_SIXBIT_CHAR(a) << 30) | (PDP10_SIXBIT_CHAR(b) << 24) | \
     (PDP10_SIXBIT_CHAR(c) << 18) | (PDP10_SIXBIT_CHAR(d) << 12) | \
     (PDP10_SIXBIT_CHAR(e) << 6) | PDP10_SIXBIT_CHAR(f))
#define PDP10_SIXBIT1(a) PDP10_SIXBIT6(a,' ',' ',' ',' ',' ')
#define PDP10_SIXBIT2(a,b) PDP10_SIXBIT6(a,b,' ',' ',' ',' ')
#define PDP10_SIXBIT3(a,b,c) PDP10_SIXBIT6(a,b,c,' ',' ',' ')
#define PDP10_SIXBIT4(a,b,c,d) PDP10_SIXBIT6(a,b,c,d,' ',' ')
#define PDP10_SIXBIT5(a,b,c,d,e) PDP10_SIXBIT6(a,b,c,d,e,' ')

#endif
