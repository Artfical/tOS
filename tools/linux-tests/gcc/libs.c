#include <stdio.h>
#include <zlib.h>
#include <sqlite3.h>
#include <readline/readline.h>
#include <curses.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
int main(void){
  unsigned char in[]="hello hello hello hello", out[100]; uLongf n=sizeof(out);
  compress(out,&n,in,sizeof(in)); printf("zlib %lu->%lu %s\n",(unsigned long)sizeof(in),(unsigned long)n,zlibVersion());
  printf("sqlite %s\n",sqlite3_libversion());
  unsigned char h[SHA256_DIGEST_LENGTH]; SHA256(in,strlen((char*)in),h); printf("sha256 %02x%02x\n",h[0],h[1]);
  printf("readline %d ncurses %s\n", rl_readline_version, curses_version());
  return 0; }
