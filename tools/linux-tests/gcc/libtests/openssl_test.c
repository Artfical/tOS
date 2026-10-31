#include "common.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
static void hex(const unsigned char *b, int n, char *out) { for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }
int main(void)
{
    unsigned char h[64]; char hx[160];
    SHA256((const unsigned char *)"abc", 3, h); hex(h, 32, hx);
    CHECK(!strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of abc");
    SHA512((const unsigned char *)"abc", 3, h); hex(h, 8, hx);
    CHECK(!strncmp(hx, "ddaf35a193617aba", 16), "SHA-512 of abc");
    unsigned int hl = 0;
    HMAC(EVP_sha256(), "key", 3, (const unsigned char *)"The quick brown fox jumps over the lazy dog", 43, h, &hl); hex(h, 32, hx);
    CHECK(hl == 32 && !strcmp(hx, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"), "HMAC-SHA256 test vector");
    unsigned char dk[32];
    PKCS5_PBKDF2_HMAC("password", 8, (const unsigned char *)"salt", 4, 1, EVP_sha256(), 32, dk); hex(dk, 32, hx);
    CHECK(!strcmp(hx, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"), "PBKDF2-HMAC-SHA256 vector");
    /* AES-256-GCM: encrypt, decrypt, and a tampered message is refused */
    unsigned char key[32], iv[12], tag[16], pt[1000], ct[1000], back[1000];
    CHECK(RAND_bytes(key, 32) == 1 && RAND_bytes(iv, 12) == 1, "RAND_bytes");
    for (int i = 0; i < 1000; i++) pt[i] = (unsigned char)(i * 7);
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); int l, total = 0;
    EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), 0, key, iv);
    EVP_EncryptUpdate(c, 0, &l, (const unsigned char *)"header", 6);
    EVP_EncryptUpdate(c, ct, &l, pt, 1000); total = l;
    EVP_EncryptFinal_ex(c, ct + total, &l);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(c);
    CHECK(memcmp(ct, pt, 1000) != 0, "ciphertext differs");
    for (int tamper = 0; tamper < 2; tamper++) {
        unsigned char *use = ct; unsigned char bad[1000];
        if (tamper) { memcpy(bad, ct, 1000); bad[500] ^= 1; use = bad; }
        c = EVP_CIPHER_CTX_new();
        EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), 0, key, iv);
        EVP_DecryptUpdate(c, 0, &l, (const unsigned char *)"header", 6);
        EVP_DecryptUpdate(c, back, &l, use, 1000); total = l;
        EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag);
        int ok = EVP_DecryptFinal_ex(c, back + total, &l);
        EVP_CIPHER_CTX_free(c);
        if (!tamper) CHECK(ok > 0 && !memcmp(back, pt, 1000), "AES-GCM round trip");
        else CHECK(ok <= 0, "AES-GCM refuses a changed message");
    }
    /* RSA 2048: make a key, sign, verify, and a changed message fails */
    EVP_PKEY *pk = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)2048);
    CHECK(pk != NULL, "RSA-2048 key generation");
    if (pk) {
        unsigned char sig[512]; size_t sl = sizeof(sig);
        EVP_MD_CTX *m = EVP_MD_CTX_new();
        EVP_DigestSignInit(m, NULL, EVP_sha256(), NULL, pk);
        CHECK(EVP_DigestSign(m, sig, &sl, (const unsigned char *)"message", 7) == 1 && sl == 256, "RSA signature");
        EVP_MD_CTX_free(m);
        for (int bad = 0; bad < 2; bad++) {
            m = EVP_MD_CTX_new();
            EVP_DigestVerifyInit(m, NULL, EVP_sha256(), NULL, pk);
            int ok = EVP_DigestVerify(m, sig, sl, (const unsigned char *)(bad ? "messagf" : "message"), 7);
            EVP_MD_CTX_free(m);
            CHECK(bad ? ok != 1 : ok == 1, bad ? "a changed message does not verify" : "signature verifies");
        }
        EVP_PKEY_free(pk);
    }
    printf("openssl %s\n", OpenSSL_version(OPENSSL_VERSION));
    DONE("openssl");
}
