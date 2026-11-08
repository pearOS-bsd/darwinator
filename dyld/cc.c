/*
 * CommonCrypto (the hashes, HMAC, PBKDF2 and the AES cryptor that libarchive uses) on top of FreeBSD's libmd and libcrypto.
 * The contexts live in memory of the Mach-O program, with the sizes of Apple's headers: CC_MD5_CTX 92 bytes, CC_SHA1_CTX 96,
 * CC_SHA256_CTX 104, CC_SHA512_CTX (and SHA384) 208, CCHmacContext 384.
 */
#include <sys/types.h>
#include <md5.h>
#include <sha.h>
#include <sha256.h>
#include <sha384.h>
#include <sha512.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(MD5_CTX) <= 92, "CC_MD5_CTX");
_Static_assert(sizeof(SHA1_CTX) <= 96, "CC_SHA1_CTX");
_Static_assert(sizeof(SHA256_CTX) <= 104, "CC_SHA256_CTX");
_Static_assert(sizeof(SHA384_CTX) <= 208 && sizeof(SHA512_CTX) <= 208, "CC_SHA512_CTX");

/* ---- plain hashes: int CC_xxx_Init(ctx), Update(ctx, data, len), Final(digest, ctx); they return 1 ---- */
int cc_MD5_Init(MD5_CTX *c) { MD5Init(c); return (1); }
int cc_MD5_Update(MD5_CTX *c, const void *d, uint32_t n) { MD5Update(c, d, n); return (1); }
int cc_MD5_Final(unsigned char *md, MD5_CTX *c) { MD5Final(md, c); return (1); }
int cc_SHA1_Init(SHA1_CTX *c) { SHA1_Init(c); return (1); }
int cc_SHA1_Update(SHA1_CTX *c, const void *d, uint32_t n) { SHA1_Update(c, d, n); return (1); }
int cc_SHA1_Final(unsigned char *md, SHA1_CTX *c) { SHA1_Final(md, c); return (1); }
int cc_SHA256_Init(SHA256_CTX *c) { SHA256_Init(c); return (1); }
int cc_SHA256_Update(SHA256_CTX *c, const void *d, uint32_t n) { SHA256_Update(c, d, n); return (1); }
int cc_SHA256_Final(unsigned char *md, SHA256_CTX *c) { SHA256_Final(md, c); return (1); }
int cc_SHA384_Init(SHA384_CTX *c) { SHA384_Init(c); return (1); }
int cc_SHA384_Update(SHA384_CTX *c, const void *d, uint32_t n) { SHA384_Update(c, d, n); return (1); }
int cc_SHA384_Final(unsigned char *md, SHA384_CTX *c) { SHA384_Final(md, c); return (1); }
int cc_SHA512_Init(SHA512_CTX *c) { SHA512_Init(c); return (1); }
int cc_SHA512_Update(SHA512_CTX *c, const void *d, uint32_t n) { SHA512_Update(c, d, n); return (1); }
int cc_SHA512_Final(unsigned char *md, SHA512_CTX *c) { SHA512_Final(md, c); return (1); }

/* ---- HMAC: CCHmacInit(ctx, alg, key, keylen), Update, Final(ctx, mac); alg: 0 SHA1, 1 MD5, 2 SHA256, 3 SHA384, 4 SHA512 ---- */
union hctx { MD5_CTX md5; SHA1_CTX sha1; SHA256_CTX s256; SHA384_CTX s384; SHA512_CTX s512; };
struct cchmac { int alg; int blk, dlen; union hctx in; unsigned char okey[128]; };
_Static_assert(sizeof(struct cchmac) <= 384, "CCHmacContext");

static void hinit(int a, union hctx *c) { switch (a) { case 0: SHA1_Init(&c->sha1); break; case 1: MD5Init(&c->md5); break; case 2: SHA256_Init(&c->s256); break; case 3: SHA384_Init(&c->s384); break; default: SHA512_Init(&c->s512); } }
static void hupd(int a, union hctx *c, const void *d, size_t n) { switch (a) { case 0: SHA1_Update(&c->sha1, d, n); break; case 1: MD5Update(&c->md5, d, n); break; case 2: SHA256_Update(&c->s256, d, n); break; case 3: SHA384_Update(&c->s384, d, n); break; default: SHA512_Update(&c->s512, d, n); } }
static void hfin(int a, union hctx *c, unsigned char *md) { switch (a) { case 0: SHA1_Final(md, &c->sha1); break; case 1: MD5Final(md, &c->md5); break; case 2: SHA256_Final(md, &c->s256); break; case 3: SHA384_Final(md, &c->s384); break; default: SHA512_Final(md, &c->s512); } }

void
cc_CCHmacInit(struct cchmac *h, uint32_t alg, const void *key, size_t keylen)
{
	static const int blk[5] = { 64, 64, 64, 128, 128 }, dl[5] = { 20, 16, 32, 48, 64 };
	unsigned char k[128], ipad[128]; int i;

	h->alg = alg > 4 ? 4 : alg; h->blk = blk[h->alg]; h->dlen = dl[h->alg];
	memset(k, 0, sizeof(k));
	if (keylen > (size_t)h->blk) { union hctx t; hinit(h->alg, &t); hupd(h->alg, &t, key, keylen); hfin(h->alg, &t, k); }
	else memcpy(k, key, keylen);
	for (i = 0; i < h->blk; i++) { ipad[i] = k[i] ^ 0x36; h->okey[i] = k[i] ^ 0x5c; }
	hinit(h->alg, &h->in); hupd(h->alg, &h->in, ipad, h->blk);
}
void cc_CCHmacUpdate(struct cchmac *h, const void *d, size_t n) { hupd(h->alg, &h->in, d, n); }
void
cc_CCHmacFinal(struct cchmac *h, void *mac)
{
	unsigned char inner[64]; union hctx o;
	hfin(h->alg, &h->in, inner);
	hinit(h->alg, &o); hupd(h->alg, &o, h->okey, h->blk); hupd(h->alg, &o, inner, h->dlen); hfin(h->alg, &o, mac);
}

/* ---- PBKDF2: CCKeyDerivationPBKDF(2, pass, passlen, salt, saltlen, prf, rounds, out, outlen); prf: 1 SHA1, 2 SHA224, 3 SHA256, 4 SHA384, 5 SHA512 ---- */
int
cc_CCKeyDerivationPBKDF(uint32_t algorithm, const char *pass, size_t passlen, const uint8_t *salt, size_t saltlen,
    uint32_t prf, uint32_t rounds, uint8_t *out, size_t outlen)
{
	const EVP_MD *md;
	if (algorithm != 2) return (-4300);			/* kCCParamError */
	switch (prf) { case 1: md = EVP_sha1(); break; case 2: md = EVP_sha224(); break; case 3: md = EVP_sha256(); break;
	case 4: md = EVP_sha384(); break; case 5: md = EVP_sha512(); break; default: return (-4300); }
	return (PKCS5_PBKDF2_HMAC(pass, (int)passlen, salt, (int)saltlen, (int)rounds, md, (int)outlen, out) ? 0 : -4300);
}

/* ---- the cryptor: AES in ECB, CBC and CTR (what libarchive asks for) ---- */
struct cccryptor { EVP_CIPHER_CTX *ctx; int encrypt; int mode; int keylen; unsigned char key[32]; };

int
cc_CCCryptorCreateWithMode(uint32_t op, uint32_t mode, uint32_t alg, uint32_t padding, const void *iv, const void *key, size_t keylen,
    const void *tweak, size_t tweaklen, int rounds, uint32_t options, struct cccryptor **ref)
{
	const EVP_CIPHER *c = NULL; struct cccryptor *cr;
	if (alg != 0) return (-4304);				/* only AES: kCCUnimplemented */
	switch (mode) {						/* 1 ECB, 2 CBC, 4 CTR */
	case 1: c = keylen == 16 ? EVP_aes_128_ecb() : keylen == 24 ? EVP_aes_192_ecb() : keylen == 32 ? EVP_aes_256_ecb() : NULL; break;
	case 2: c = keylen == 16 ? EVP_aes_128_cbc() : keylen == 24 ? EVP_aes_192_cbc() : keylen == 32 ? EVP_aes_256_cbc() : NULL; break;
	case 4: c = keylen == 16 ? EVP_aes_128_ctr() : keylen == 24 ? EVP_aes_192_ctr() : keylen == 32 ? EVP_aes_256_ctr() : NULL; break;
	}
	if (c == NULL) return (-4304);
	cr = calloc(1, sizeof(*cr)); cr->encrypt = op == 0; cr->mode = mode; cr->keylen = (int)keylen; memcpy(cr->key, key, keylen);
	cr->ctx = EVP_CIPHER_CTX_new();
	if (EVP_CipherInit_ex(cr->ctx, c, NULL, cr->key, iv, cr->encrypt) != 1) { EVP_CIPHER_CTX_free(cr->ctx); free(cr); return (-4300); }
	EVP_CIPHER_CTX_set_padding(cr->ctx, padding != 0 ? 1 : 0);
	*ref = cr;
	return (0);
}
int
cc_CCCryptorUpdate(struct cccryptor *cr, const void *in, size_t inlen, void *out, size_t outavail, size_t *moved)
{
	int n = 0;
	if (outavail < inlen) return (-4301);			/* kCCBufferTooSmall */
	if (EVP_CipherUpdate(cr->ctx, out, &n, in, (int)inlen) != 1) return (-4300);
	if (moved) *moved = n;
	return (0);
}
int
cc_CCCryptorReset(struct cccryptor *cr, const void *iv)
{
	return (EVP_CipherInit_ex(cr->ctx, NULL, NULL, NULL, iv, cr->encrypt) == 1 ? 0 : -4300);
}
int cc_CCCryptorRelease(struct cccryptor *cr) { if (cr) { EVP_CIPHER_CTX_free(cr->ctx); free(cr); } return (0); }
int
cc_CCCryptorFinal(struct cccryptor *cr, void *out, size_t outavail, size_t *moved)
{
	int n = 0;
	if (EVP_CipherFinal_ex(cr->ctx, out, &n) != 1) return (-4300);
	if (moved) *moved = n;
	return (0);
}
