/*
 * Reserve the recovered executable's complete 32-bit image range before
 * dyld, Rosetta, or libmalloc can claim low pages.  game_loader releases this
 * zerofill segment immediately before installing the legacy Mach-O segments.
 */
.globl _lp32_legacy_reservation
.zerofill __LEGACY,__reserve,_lp32_legacy_reservation,2147479552,12
