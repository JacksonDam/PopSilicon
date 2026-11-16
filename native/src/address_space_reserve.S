/*
 * Reserve the recovered executable's complete 32-bit image range before
 * dyld, Rosetta, or libmalloc can claim low pages.  game_loader releases this
 * zerofill segment immediately before installing the legacy Mach-O segments.
 */
.macro lp32_reserve_mib
.zerofill __LEGACY,__reserve,_lp32_legacy_reservation_\@,1048576,12
.endm

.globl _lp32_legacy_reservation
.zerofill __LEGACY,__reserve,_lp32_legacy_reservation,1048576,12
.rept 2046
lp32_reserve_mib
.endr
.zerofill __LEGACY,__reserve,_lp32_legacy_reservation_tail,1044480,12
