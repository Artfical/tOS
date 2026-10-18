/* The build this kernel is: the commit count of the tree it was built from (it only grows, so
 * Software Update compares builds by it) and that commit. Rebuilt every time (see Makefile). */
#ifndef TOS_BUILD
#define TOS_BUILD 0
#endif
#ifndef TOS_COMMIT
#define TOS_COMMIT "unknown"
#endif
const unsigned tos_build = TOS_BUILD;
const char tos_commit[] = TOS_COMMIT;
