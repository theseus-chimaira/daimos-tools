#ifndef PDP10_SIXBIT_H
#define PDP10_SIXBIT_H

#ifndef __PDP10__
#error "pdp10-sixbit.h requires the PDP-10 target"
#endif

/* Return the first six char6 elements of a string literal as one
   PDP-10 SIXBIT word.  This constructs data only; it performs no I/O. */
#define SIXBIT(name) (*((int *)((char6 *)(name))))

#endif
