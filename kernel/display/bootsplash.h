#ifndef BOOTSPLASH_H
#define BOOTSPLASH_H

/* The boot screen, like a Mac's: the logo on white with a thin progress bar under it, shown
 * from early boot until the first question. Without a VBE display it simply isn't shown. */
int  bootsplash_begin(void);          /* 0 = it is up */
void bootsplash_progress(int percent);
void bootsplash_end(void);
int  bootsplash_active(void);
void bootsplash_status(const char *text);   /* e.g. "Updating tOS…" under the bar */
void bootsplash_handoff(void);        /* the desktop has the screen now */
void bootsplash_paging_ready(void);   /* paging is on now: map the framebuffer */            /* back to the text console */

#endif
