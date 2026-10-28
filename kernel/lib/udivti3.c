/* 128-bit division, which GCC calls for unsigned __int128 on x86-64 (QuickJS uses it). The kernel
 * does not link libgcc: its routines may use the red zone, which interrupts would overwrite. */
typedef unsigned __int128 u128;

u128 __udivmodti4(u128 num, u128 den, u128 *rem)
{
    u128 quot = 0, qbit = 1;
    if (den == 0) { if (rem) *rem = 0; return 0; }
    while ((__int128)den > 0 && den <= num) {
        den <<= 1;
        qbit <<= 1;
    }
    while (qbit) {
        if (num >= den) {
            num -= den;
            quot |= qbit;
        }
        den >>= 1;
        qbit >>= 1;
    }
    if (rem) *rem = num;
    return quot;
}

u128 __udivti3(u128 num, u128 den) { return __udivmodti4(num, den, 0); }

u128 __umodti3(u128 num, u128 den)
{
    u128 r;
    __udivmodti4(num, den, &r);
    return r;
}
