#include "wgtun.h"
#include "chacha20.h"
#include "net.h"
#include "udp.h"
#include "route.h"
#include "ip.h"
#include "string.h"
#include "terminal.h"
#include "memory.h"

static wgtun_t tunnels[WGTUN_MAX];

void wgtun_init(void)
{
    memset(tunnels, 0, sizeof(tunnels));
}

int wgtun_add(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
              const uint8_t psk[WGTUN_KEY_LEN], uint32_t peer_id)
{
    for (int i = 0; i < WGTUN_MAX; i++) {
        if (!tunnels[i].valid) {
            tunnels[i].remote_ip   = remote_ip;
            tunnels[i].remote_port = remote_port;
            tunnels[i].local_port  = local_port;
            memcpy(tunnels[i].preshared_key, psk, WGTUN_KEY_LEN);
            tunnels[i].peer_id     = peer_id;
            tunnels[i].tx_nonce_ctr = 1;
            tunnels[i].valid       = 1;
            return i;
        }
    }
    return -1;
}

int wgtun_del(int idx)
{
    if (idx < 0 || idx >= WGTUN_MAX || !tunnels[idx].valid) return -1;
    tunnels[idx].valid = 0;
    return 0;
}

static void print_ip(uint32_t ip)
{
    uint8_t b[4];
    b[0]=ip&0xFF;b[1]=(ip>>8)&0xFF;b[2]=(ip>>16)&0xFF;b[3]=(ip>>24)&0xFF;
    char buf[16];int i=0;
    for(int n=0;n<4;n++){uint8_t v=b[n];if(v>=100)buf[i++]='0'+v/100;if(v>=10)buf[i++]='0'+(v/10)%10;buf[i++]='0'+v%10;if(n<3)buf[i++]='.';}
    buf[i]='\0';terminal_writestring(buf);
}

static void print_uint16(uint16_t n)
{
    char buf[8]; int i=7; buf[7]='\0';
    if (!n) { terminal_putchar('0'); return; }
    while (n>0&&i>0){buf[--i]='0'+(n%10);n/=10;}
    terminal_writestring(buf+i);
}

void wgtun_list(void)
{
    terminal_writestring("WG tunnels:\n");
    for (int i = 0; i < WGTUN_MAX; i++) {
        wgtun_t *t = &tunnels[i];
        if (!t->valid) continue;
        terminal_writestring("  ["); terminal_putchar('0'+i); terminal_writestring("] ");
        terminal_writestring("remote="); print_ip(t->remote_ip);
        terminal_putchar(':'); print_uint16(t->remote_port);
        terminal_writestring(" local_port="); print_uint16(t->local_port);
        terminal_writestring(" peer_id=0x");
        static const char hex[]="0123456789abcdef";
        for (int b=28;b>=0;b-=4) terminal_putchar(hex[(t->peer_id>>b)&0xF]);
        terminal_putchar('\n');
    }
}

int wgtun_send(int idx, const void *inner_ip, int inner_len)
{
    if (idx < 0 || idx >= WGTUN_MAX || !tunnels[idx].valid) return -1;
    wgtun_t *t = &tunnels[idx];

    /* Build nonce from counter (little-endian 8 bytes + 16 zero bytes) */
    uint8_t nonce[WGTUN_NONCE_LEN];
    memset(nonce, 0, WGTUN_NONCE_LEN);
    uint64_t ctr = t->tx_nonce_ctr++;
    for (int i = 0; i < 8; i++) nonce[i] = (uint8_t)(ctr >> (i*8));

    /* Allocate encrypted buffer: peer_id(4) + nonce(24) + ciphertext(inner_len+16) */
    int enc_len = inner_len + 16;
    int pkt_len = 4 + WGTUN_NONCE_LEN + enc_len;
    uint8_t *pkt = (uint8_t *)malloc(pkt_len);
    if (!pkt) return -1;

    /* peer_id */
    pkt[0] = (uint8_t)(t->peer_id);
    pkt[1] = (uint8_t)(t->peer_id >> 8);
    pkt[2] = (uint8_t)(t->peer_id >> 16);
    pkt[3] = (uint8_t)(t->peer_id >> 24);
    /* nonce */
    memcpy(pkt + 4, nonce, WGTUN_NONCE_LEN);
    /* encrypt */
    xchacha20poly1305_encrypt(t->preshared_key, nonce,
                              (const uint8_t *)inner_ip, inner_len,
                              pkt + 4 + WGTUN_NONCE_LEN);

    /* Send via UDP */
    int r = udp_send(t->remote_ip, t->remote_port, t->local_port, pkt, pkt_len);
    free(pkt);
    return r;
}

void wgtun_rx(uint32_t src_ip, uint16_t src_port,
              const uint8_t *data, int len)
{
    if (len < 4 + WGTUN_NONCE_LEN + 16) return;

    uint32_t peer_id = (uint32_t)data[0] | ((uint32_t)data[1]<<8) |
                       ((uint32_t)data[2]<<16) | ((uint32_t)data[3]<<24);

    /* Find matching tunnel by peer_id and remote_ip */
    wgtun_t *t = 0;
    for (int i = 0; i < WGTUN_MAX; i++) {
        if (tunnels[i].valid &&
            tunnels[i].peer_id == peer_id &&
            tunnels[i].remote_ip == src_ip) {
            t = &tunnels[i]; break;
        }
    }
    if (!t) {
        /* also accept from any source if peer_id matches */
        for (int i = 0; i < WGTUN_MAX; i++) {
            if (tunnels[i].valid && tunnels[i].peer_id == peer_id) {
                t = &tunnels[i]; break;
            }
        }
    }
    if (!t) return;

    const uint8_t *nonce      = data + 4;
    const uint8_t *ciphertext = data + 4 + WGTUN_NONCE_LEN;
    int cipher_len            = len - 4 - WGTUN_NONCE_LEN;

    uint8_t *plain = (uint8_t *)malloc(cipher_len);
    if (!plain) return;

    if (xchacha20poly1305_decrypt(t->preshared_key, nonce,
                                  ciphertext, cipher_len, plain) != 0) {
        terminal_writestring("[WG] MAC verification failed from ");
        print_ip(src_ip);
        terminal_putchar('\n');
        free(plain);
        return;
    }

    terminal_writestring("[WG] decrypted packet from ");
    print_ip(src_ip);
    terminal_putchar(':');
    print_uint16(src_port);
    terminal_putchar('\n');

    /* Re-inject decrypted inner IP packet */
    extern void ip_handle(uint8_t *data, int len);
    ip_handle(plain, cipher_len - 16);
    free(plain);
}
