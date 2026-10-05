/* Host test driver for kernel/net/x509.c.
 *   parse FILE.der                          dump the parsed fields
 *   selfsig FILE.der                        verify a certificate against its own key
 *   chain HOST YYYYMMDDHHMMSS FLAGS ROOT|- CERT.der...   validate a chain (ROOT.der becomes the trust store)
 *   fuzz SEED N CERT.der...                 mutate DER bytes, parse + validate, must never crash */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "x509.h"

unsigned char net_mac[6] = {1, 2, 3, 4, 5, 6};
const x509_der_t ca_store_builtin[1] = { { 0, 0 } };
const int ca_store_builtin_count = 0;

static uint8_t *slurp(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

static x509_time_t parse_now(const char *s)
{
    int y, mo, d, h, mi, sec;
    sscanf(s, "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &sec);
    return x509_time_from_ymdhms(y, mo, d, h, mi, sec);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "parse")) {
        uint32_t l; uint8_t *d = slurp(argv[2], &l);
        x509_cert_t c;
        int rc = x509_parse(d, l, &c);
        printf("rc=%d\n", rc);
        if (rc == 0) {
            printf("rsa=%d n_len=%u e=%u sig_hash=%d sig_status=%d\n", c.has_rsa_key, c.n_len, c.e, c.sig_hash, c.sig_alg_status);
            printf("ca=%d bc=%d pathlen=%d ku=%d/%04x eku=%d/%d san=%d crit_unsup=%d\n", c.is_ca, c.has_bc, c.path_len, c.has_ku, c.ku, c.has_eku, c.eku_server_auth, c.has_san, c.critical_unsupported);
            printf("nb=%d/%u na=%d/%u\n", c.not_before.day, c.not_before.sec, c.not_after.day, c.not_after.sec);
            printf("cn=%.*s\n", (int)c.cn_len, c.cn ? (const char *)c.cn : "");
        }
        return 0;
    }
    if (!strcmp(argv[1], "selfsig")) {
        uint32_t l; uint8_t *d = slurp(argv[2], &l);
        x509_cert_t c;
        int rc = x509_parse(d, l, &c);
        if (rc) { printf("parse %d\n", rc); return 0; }
        printf("%d\n", x509_check_signature(&c, &c));
        return 0;
    }
    if (!strcmp(argv[1], "chain") && argc >= 7) {
        const char *host = argv[2];
        x509_time_t now = parse_now(argv[3]);
        int flags = atoi(argv[4]);
        if (strcmp(argv[5], "-")) {
            uint32_t rl; uint8_t *r = slurp(argv[5], &rl);
            int rc = x509_trust_add(r, rl);
            if (rc) { printf("trust_add %d\n", rc); return 0; }
        }
        x509_der_t chain[X509_MAX_CHAIN + 4];
        int n = 0;
        for (int i = 6; i < argc && n < X509_MAX_CHAIN + 4; i++) { chain[n].der = slurp(argv[i], &chain[n].len); n++; }
        x509_cert_t leaf;
        int rc = x509_verify_chain(chain, n, host, now, flags, &leaf);
        printf("%d %s\n", rc, x509_strerror(rc));
        return 0;
    }
    if (!strcmp(argv[1], "fuzz") && argc >= 5) {
        srand((unsigned)atoi(argv[2]));
        int iters = atoi(argv[3]);
        x509_der_t base[8]; int nb = 0;
        for (int i = 4; i < argc && nb < 8; i++) { base[nb].der = slurp(argv[i], &base[nb].len); nb++; }
        long ok = 0;
        for (int it = 0; it < iters; it++) {
            x509_der_t m[8];
            for (int i = 0; i < nb; i++) {
                uint8_t *copy = malloc(base[i].len);
                memcpy(copy, base[i].der, base[i].len);
                uint32_t len = base[i].len;
                int muts = 1 + rand() % 4;
                for (int k = 0; k < muts; k++) {
                    int kind = rand() % 4;
                    uint32_t pos = (uint32_t)rand() % len;
                    if (kind == 0) copy[pos] ^= (uint8_t)(1u << (rand() % 8));
                    else if (kind == 1) copy[pos] = (uint8_t)rand();
                    else if (kind == 2 && len > 20) len -= 1 + (uint32_t)rand() % 8;   /* truncate */
                    else { copy[pos] = 0xFF; }
                }
                m[i].der = copy; m[i].len = len;
            }
            x509_cert_t c;
            x509_parse(m[0].der, m[0].len, &c);
            x509_cert_t leaf;
            if (x509_verify_chain(m, nb, "example.test", x509_time_from_ymdhms(2026, 10, 5, 12, 0, 0), 0, &leaf) == 0) ok++;
            for (int i = 0; i < nb; i++) free((void *)m[i].der);
        }
        printf("fuzz done, %ld accepted\n", ok);
        return 0;
    }
    return 2;
}
