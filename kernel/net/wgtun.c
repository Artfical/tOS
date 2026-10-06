#include "wgtun.h"
#include "chacha20.h"
#include "net.h"
#include "udp.h"
#include "route.h"
#include "ip.h"
#include "string.h"
#include "terminal.h"
#include "memory.h"
#include "csprng.h"
#include "cmos.h"

static wgtun_t tunnels[WGTUN_MAX];

void wgtun_init(void)
{
    memset(tunnels, 0, sizeof(tunnels));
}

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int32_t wg_days(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* First sequence number of a tunnel: seconds since the epoch in the high bits (a
 * restarted peer then starts above everything it sent before, as long as the
 * clock is sane), 20 bits of per-second packet counter below. */
static uint64_t wg_seq_base(void)
{
    cmos_time_t t;
    cmos_get_time(&t);
    if (t.year < 2024 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31) return 1;
    uint64_t secs = (uint64_t)((uint32_t)wg_days(t.year, t.month, t.day) * 86400u +
                               (uint32_t)(t.hour * 3600 + t.minute * 60 + t.second));
    return (secs << 20) | 1;
}

int wgtun_add(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
              const uint8_t psk[WGTUN_KEY_LEN], uint32_t peer_id)
{
    for (int i = 0; i < WGTUN_MAX; i++)            /* peer_id selects the key: it must be unique */
        if (tunnels[i].valid && tunnels[i].peer_id == peer_id) return -1;
    for (int i = 0; i < WGTUN_MAX; i++) {
        if (!tunnels[i].valid) {
            memset(&tunnels[i], 0, sizeof(tunnels[i]));
            tunnels[i].remote_ip   = remote_ip;
            tunnels[i].remote_port = remote_port;
            tunnels[i].local_port  = local_port;
            memcpy(tunnels[i].preshared_key, psk, WGTUN_KEY_LEN);
            tunnels[i].peer_id     = peer_id;
            tunnels[i].tx_seq      = wg_seq_base();
            tunnels[i].valid       = 1;
            return i;
        }
    }
    return -1;
}

int wgtun_del(int idx)
{
    if (idx < 0 || idx >= WGTUN_MAX || !tunnels[idx].valid) return -1;
    volatile uint8_t *k = tunnels[idx].preshared_key;       /* do not leave the key behind */
    for (int i = 0; i < WGTUN_KEY_LEN; i++) k[i] = 0;
    tunnels[idx].valid = 0;
    return 0;
}

int wgtun_owns_port(uint16_t port)
{
    for (int i = 0; i < WGTUN_MAX; i++)
        if (tunnels[i].valid && tunnels[i].local_port == port) return 1;
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

/* largest inner packet: UDP payload 1472 - peer_id 4 - nonce 24 - sequence 8 - tag 16 */
#define WGTUN_MAX_INNER (1472 - 4 - WGTUN_NONCE_LEN - 8 - 16)

int wgtun_send(int idx, const void *inner_ip, int inner_len)
{
    if (idx < 0 || idx >= WGTUN_MAX || !tunnels[idx].valid) return -1;
    /* a negative length made the size computations below wrap into a huge malloc */
    if (inner_len < 0 || inner_len > WGTUN_MAX_INNER) return -1;
    wgtun_t *t = &tunnels[idx];

    /* Random 192-bit nonce per packet. The old nonce was a counter that started
     * again at 1 whenever the tunnel was created (reboot, wg del + add) with the
     * same pre-shared key: reusing a (key, nonce) pair in ChaCha20-Poly1305 leaks
     * the XOR of the plaintexts and lets an attacker forge packets. */
    uint8_t nonce[WGTUN_NONCE_LEN];
    csprng_fill(nonce, WGTUN_NONCE_LEN);

    int plain_len = 8 + inner_len;
    int pkt_len = 4 + WGTUN_NONCE_LEN + plain_len + 16;
    uint8_t *pkt = (uint8_t *)malloc((size_t)pkt_len);
    uint8_t *plain = (uint8_t *)malloc((size_t)plain_len);
    if (!pkt || !plain) { free(pkt); free(plain); return -1; }

    uint64_t seq = t->tx_seq++;
    for (int i = 0; i < 8; i++) plain[i] = (uint8_t)(seq >> (56 - 8 * i));
    memcpy(plain + 8, inner_ip, (size_t)inner_len);

    pkt[0] = (uint8_t)(t->peer_id);
    pkt[1] = (uint8_t)(t->peer_id >> 8);
    pkt[2] = (uint8_t)(t->peer_id >> 16);
    pkt[3] = (uint8_t)(t->peer_id >> 24);
    memcpy(pkt + 4, nonce, WGTUN_NONCE_LEN);
    xchacha20poly1305_encrypt(t->preshared_key, nonce, plain, plain_len, pkt + 4 + WGTUN_NONCE_LEN);

    volatile uint8_t *w = plain;                            /* the plaintext sequence/packet copy */
    for (int i = 0; i < plain_len; i++) w[i] = 0;
    free(plain);

    int r = udp_send(t->remote_ip, t->remote_port, t->local_port, pkt, pkt_len);
    free(pkt);
    return r;
}

static int wg_depth;    /* tunnel inside tunnel nesting, bounded below */

void wgtun_rx(uint32_t src_ip, uint16_t src_port,
              const uint8_t *data, int len)
{
    if (len < 4 + WGTUN_NONCE_LEN + 8 + 16) return;
    if (len > 4 + WGTUN_NONCE_LEN + 8 + WGTUN_MAX_INNER + 16) return;

    uint32_t peer_id = (uint32_t)data[0] | ((uint32_t)data[1]<<8) |
                       ((uint32_t)data[2]<<16) | ((uint32_t)data[3]<<24);

    /* The peer id selects the key. A packet from another address is accepted only
     * when it authenticates (the endpoint may roam), so no source check is needed. */
    wgtun_t *t = 0;
    for (int i = 0; i < WGTUN_MAX; i++) {
        if (tunnels[i].valid && tunnels[i].peer_id == peer_id) { t = &tunnels[i]; break; }
    }
    if (!t) return;

    const uint8_t *nonce      = data + 4;
    const uint8_t *ciphertext = data + 4 + WGTUN_NONCE_LEN;
    int cipher_len            = len - 4 - WGTUN_NONCE_LEN;

    uint8_t *plain = (uint8_t *)malloc((size_t)cipher_len);
    if (!plain) return;

    if (xchacha20poly1305_decrypt(t->preshared_key, nonce,
                                  ciphertext, cipher_len, plain) != 0) {
        terminal_writestring("[WG] MAC verification failed from ");
        print_ip(src_ip);
        terminal_putchar('\n');
        goto out;
    }
    int plain_len = cipher_len - 16;
    if (plain_len < 8) goto out;

    /* Anti-replay, only for authenticated packets: 64-packet sliding window. A
     * captured packet used to be accepted again and again. */
    uint64_t seq = 0;
    for (int i = 0; i < 8; i++) seq = (seq << 8) | plain[i];
    if (seq == 0) goto out;
    if (seq > t->rx_max) {
        uint64_t shift = seq - t->rx_max;
        t->rx_window = shift >= 64 ? 0 : (t->rx_window << shift);
        t->rx_window |= 1;
        t->rx_max = seq;
    } else {
        uint64_t d = t->rx_max - seq;
        if (d >= 64 || (t->rx_window & ((uint64_t)1 << d))) {
            terminal_writestring("[WG] replayed or too old packet dropped\n");
            goto out;
        }
        t->rx_window |= (uint64_t)1 << d;
    }

    terminal_writestring("[WG] decrypted packet from ");
    print_ip(src_ip);
    terminal_putchar(':');
    print_uint16(src_port);
    terminal_putchar('\n');

    /* Re-inject the decrypted inner IP packet (at most one level deep: a tunnel
     * carrying a tunnel carrying a tunnel ... recursed without bound). */
    if (wg_depth < 2) {
        extern void ip_handle(uint8_t *data, int len);
        wg_depth++;
        ip_handle(plain + 8, plain_len - 8);
        wg_depth--;
    }
out:
    {
        volatile uint8_t *w = plain;
        for (int i = 0; i < cipher_len; i++) w[i] = 0;
    }
    free(plain);
}
