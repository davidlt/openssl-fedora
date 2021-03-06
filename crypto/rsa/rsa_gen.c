/* crypto/rsa/rsa_gen.c */
/* Copyright (C) 1995-1998 Eric Young (eay@cryptsoft.com)
 * Copyright (C) 2013 Red Hat, Inc.
 * All rights reserved.
 *
 * This package is an SSL implementation written
 * by Eric Young (eay@cryptsoft.com).
 * The implementation was written so as to conform with Netscapes SSL.
 *
 * This library is free for commercial and non-commercial use as long as
 * the following conditions are aheared to.  The following conditions
 * apply to all code found in this distribution, be it the RC4, RSA,
 * lhash, DES, etc., code; not just the SSL code.  The SSL documentation
 * included with this distribution is covered by the same copyright terms
 * except that the holder is Tim Hudson (tjh@cryptsoft.com).
 *
 * Copyright remains Eric Young's, and as such any Copyright notices in
 * the code are not to be removed.
 * If this package is used in a product, Eric Young should be given attribution
 * as the author of the parts of the library used.
 * This can be in the form of a textual message at program startup or
 * in documentation (online or textual) provided with the package.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    "This product includes cryptographic software written by
 *     Eric Young (eay@cryptsoft.com)"
 *    The word 'cryptographic' can be left out if the rouines from the library
 *    being used are not cryptographic related :-).
 * 4. If you include any Windows specific code (or a derivative thereof) from
 *    the apps directory (application code) you must include an acknowledgement:
 *    "This product includes software written by Tim Hudson (tjh@cryptsoft.com)"
 *
 * THIS SOFTWARE IS PROVIDED BY ERIC YOUNG ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The licence and distribution terms for any publically available version or
 * derivative of this code cannot be changed.  i.e. this code cannot simply be
 * copied and put under another distribution licence
 * [including the GNU Public Licence.]
 */

/*
 * NB: these functions have been "upgraded", the deprecated versions (which
 * are compatibility wrappers using these functions) are in rsa_depr.c. -
 * Geoff
 */

#include <stdio.h>
#include <time.h>
#include "cryptlib.h"
#include <openssl/bn.h>
#include <openssl/rsa.h>
#ifdef OPENSSL_FIPS
# include <openssl/fips.h>
# include <openssl/err.h>
# include <openssl/evp.h>

static int fips_rsa_pairwise_fail = 0;

void FIPS_corrupt_rsa_keygen(void)
{
    fips_rsa_pairwise_fail = 1;
}

int fips_check_rsa(RSA *rsa)
{
    const unsigned char tbs[] = "RSA Pairwise Check Data";
    unsigned char *ctbuf = NULL, *ptbuf = NULL;
    int len, ret = 0;
    EVP_PKEY *pk;

    if ((pk = EVP_PKEY_new()) == NULL)
        goto err;

    EVP_PKEY_set1_RSA(pk, rsa);

    /* Perform pairwise consistency signature test */
    if (!fips_pkey_signature_test(pk, tbs, -1,
                                  NULL, 0, EVP_sha1(),
                                  EVP_MD_CTX_FLAG_PAD_PKCS1, NULL)
        || !fips_pkey_signature_test(pk, tbs, -1, NULL, 0, EVP_sha1(),
                                     EVP_MD_CTX_FLAG_PAD_X931, NULL)
        || !fips_pkey_signature_test(pk, tbs, -1, NULL, 0, EVP_sha1(),
                                     EVP_MD_CTX_FLAG_PAD_PSS, NULL))
        goto err;
    /* Now perform pairwise consistency encrypt/decrypt test */
    ctbuf = OPENSSL_malloc(RSA_size(rsa));
    if (!ctbuf)
        goto err;

    len =
        RSA_public_encrypt(sizeof(tbs) - 1, tbs, ctbuf, rsa,
                           RSA_PKCS1_PADDING);
    if (len <= 0)
        goto err;
    /* Check ciphertext doesn't match plaintext */
    if ((len == (sizeof(tbs) - 1)) && !memcmp(tbs, ctbuf, len))
        goto err;
    ptbuf = OPENSSL_malloc(RSA_size(rsa));

    if (!ptbuf)
        goto err;
    len = RSA_private_decrypt(len, ctbuf, ptbuf, rsa, RSA_PKCS1_PADDING);
    if (len != (sizeof(tbs) - 1))
        goto err;
    if (memcmp(ptbuf, tbs, len))
        goto err;

    ret = 1;

    if (!ptbuf)
        goto err;

 err:
    if (ret == 0) {
        fips_set_selftest_fail();
        FIPSerr(FIPS_F_FIPS_CHECK_RSA, FIPS_R_PAIRWISE_TEST_FAILED);
    }

    if (ctbuf)
        OPENSSL_free(ctbuf);
    if (ptbuf)
        OPENSSL_free(ptbuf);
    if (pk)
        EVP_PKEY_free(pk);

    return ret;
}
#endif

static int rsa_builtin_keygen(RSA *rsa, int bits, BIGNUM *e_value,
                              BN_GENCB *cb);

/*
 * NB: this wrapper would normally be placed in rsa_lib.c and the static
 * implementation would probably be in rsa_eay.c. Nonetheless, is kept here
 * so that we don't introduce a new linker dependency. Eg. any application
 * that wasn't previously linking object code related to key-generation won't
 * have to now just because key-generation is part of RSA_METHOD.
 */
int RSA_generate_key_ex(RSA *rsa, int bits, BIGNUM *e_value, BN_GENCB *cb)
{
#ifdef OPENSSL_FIPS
    if (FIPS_module_mode() && !(rsa->meth->flags & RSA_FLAG_FIPS_METHOD)
        && !(rsa->flags & RSA_FLAG_NON_FIPS_ALLOW)) {
        RSAerr(RSA_F_RSA_GENERATE_KEY_EX, RSA_R_NON_FIPS_RSA_METHOD);
        return 0;
    }
#endif
    if (rsa->meth->rsa_keygen)
        return rsa->meth->rsa_keygen(rsa, bits, e_value, cb);
    return rsa_builtin_keygen(rsa, bits, e_value, cb);
}

#ifdef OPENSSL_FIPS
static int FIPS_rsa_builtin_keygen(RSA *rsa, int bits, BIGNUM *e_value,
                                   BN_GENCB *cb)
{
    BIGNUM *r0 = NULL, *r1 = NULL, *r2 = NULL, *r3 = NULL, *tmp;
    BIGNUM local_r0, local_d, local_p;
    BIGNUM *pr0, *d, *p;
    BN_CTX *ctx = NULL;
    int ok = -1;
    int i;
    int n = 0;
    int test = 0;
    int pbits = bits / 2;

    if (FIPS_selftest_failed()) {
        FIPSerr(FIPS_F_RSA_BUILTIN_KEYGEN, FIPS_R_FIPS_SELFTEST_FAILED);
        return 0;
    }

    if ((pbits & 0xFF)
        || (getenv("OPENSSL_ENFORCE_MODULUS_BITS") && bits != 2048
            && bits != 3072)) {
        FIPSerr(FIPS_F_RSA_BUILTIN_KEYGEN, FIPS_R_INVALID_KEY_LENGTH);
        return 0;
    }

    ctx = BN_CTX_new();
    if (ctx == NULL)
        goto err;
    BN_CTX_start(ctx);
    r0 = BN_CTX_get(ctx);
    r1 = BN_CTX_get(ctx);
    r2 = BN_CTX_get(ctx);
    r3 = BN_CTX_get(ctx);

    if (r3 == NULL)
        goto err;

    /* We need the RSA components non-NULL */
    if (!rsa->n && ((rsa->n = BN_new()) == NULL))
        goto err;
    if (!rsa->d && ((rsa->d = BN_new()) == NULL))
        goto err;
    if (!rsa->e && ((rsa->e = BN_new()) == NULL))
        goto err;
    if (!rsa->p && ((rsa->p = BN_new()) == NULL))
        goto err;
    if (!rsa->q && ((rsa->q = BN_new()) == NULL))
        goto err;
    if (!rsa->dmp1 && ((rsa->dmp1 = BN_new()) == NULL))
        goto err;
    if (!rsa->dmq1 && ((rsa->dmq1 = BN_new()) == NULL))
        goto err;
    if (!rsa->iqmp && ((rsa->iqmp = BN_new()) == NULL))
        goto err;

    if (!BN_set_word(r0, RSA_F4))
        goto err;
    if (BN_cmp(e_value, r0) < 0 || BN_num_bits(e_value) > 256) {
        ok = 0;                 /* we set our own err */
        RSAerr(RSA_F_RSA_BUILTIN_KEYGEN, RSA_R_BAD_E_VALUE);
        goto err;
    }

    /* prepare approximate minimum p and q */
    if (!BN_set_word(r0, 0xB504F334))
        goto err;
    if (!BN_lshift(r0, r0, pbits - 32))
        goto err;

    /* prepare minimum p and q difference */
    if (!BN_one(r3))
        goto err;
    if (!BN_lshift(r3, r3, pbits - 100))
        goto err;

    BN_copy(rsa->e, e_value);

    if (!BN_is_zero(rsa->p) && !BN_is_zero(rsa->q))
        test = 1;

 retry:
    /* generate p and q */
    for (i = 0; i < 5 * pbits; i++) {
 ploop:
        if (!test)
            if (!BN_rand(rsa->p, pbits, 0, 1))
                goto err;
        if (BN_cmp(rsa->p, r0) < 0) {
            if (test)
                goto err;
            goto ploop;
        }

        if (!BN_sub(r2, rsa->p, BN_value_one()))
            goto err;
        if (!BN_gcd(r1, r2, rsa->e, ctx))
            goto err;
        if (BN_is_one(r1)) {
            int r;
            r = BN_is_prime_fasttest_ex(rsa->p, pbits > 1024 ? 4 : 5, ctx, 0,
                                        cb);
            if (r == -1 || (test && r <= 0))
                goto err;
            if (r > 0)
                break;
        }

        if (!BN_GENCB_call(cb, 2, n++))
            goto err;
    }

    if (!BN_GENCB_call(cb, 3, 0))
        goto err;

    if (i >= 5 * pbits)
        /* prime not found */
        goto err;

    for (i = 0; i < 5 * pbits; i++) {
 qloop:
        if (!test)
            if (!BN_rand(rsa->q, pbits, 0, 1))
                goto err;
        if (BN_cmp(rsa->q, r0) < 0) {
            if (test)
                goto err;
            goto qloop;
        }
        if (!BN_sub(r2, rsa->q, rsa->p))
            goto err;
        if (BN_ucmp(r2, r3) <= 0) {
            if (test)
                goto err;
            goto qloop;
        }

        if (!BN_sub(r2, rsa->q, BN_value_one()))
            goto err;
        if (!BN_gcd(r1, r2, rsa->e, ctx))
            goto err;
        if (BN_is_one(r1)) {
            int r;
            r = BN_is_prime_fasttest_ex(rsa->q, pbits > 1024 ? 4 : 5, ctx, 0,
                                        cb);
            if (r == -1 || (test && r <= 0))
                goto err;
            if (r > 0)
                break;
        }

        if (!BN_GENCB_call(cb, 2, n++))
            goto err;
    }

    if (!BN_GENCB_call(cb, 3, 1))
        goto err;

    if (i >= 5 * pbits)
        /* prime not found */
        goto err;

    if (test) {
        /* do not try to calculate the remaining key values */
        BN_clear(rsa->n);
        ok = 1;
        goto err;
    }

    if (BN_cmp(rsa->p, rsa->q) < 0) {
        tmp = rsa->p;
        rsa->p = rsa->q;
        rsa->q = tmp;
    }

    /* calculate n */
    if (!BN_mul(rsa->n, rsa->p, rsa->q, ctx))
        goto err;

    /* calculate d */
    if (!BN_sub(r1, rsa->p, BN_value_one()))
        goto err;               /* p-1 */
    if (!BN_sub(r2, rsa->q, BN_value_one()))
        goto err;               /* q-1 */

    if (!BN_gcd(r0, r1, r2, ctx))
        goto err;
    if (!(rsa->flags & RSA_FLAG_NO_CONSTTIME)) {
        pr0 = &local_r0;
        BN_with_flags(pr0, r0, BN_FLG_CONSTTIME);
    } else
        pr0 = r0;
    if (!BN_div(r0, NULL, r1, pr0, ctx))
        goto err;
    if (!BN_mul(r0, r0, r2, ctx))
        goto err;               /* lcm(p-1, q-1) */

    if (!(rsa->flags & RSA_FLAG_NO_CONSTTIME)) {
        pr0 = &local_r0;
        BN_with_flags(pr0, r0, BN_FLG_CONSTTIME);
    } else
        pr0 = r0;
    if (!BN_mod_inverse(rsa->d, rsa->e, pr0, ctx))
        goto err;               /* d */

    if (BN_num_bits(rsa->d) < pbits)
        goto retry;             /* d is too small */

    /* set up d for correct BN_FLG_CONSTTIME flag */
    if (!(rsa->flags & RSA_FLAG_NO_CONSTTIME)) {
        d = &local_d;
        BN_with_flags(d, rsa->d, BN_FLG_CONSTTIME);
    } else
        d = rsa->d;

    /* calculate d mod (p-1) */
    if (!BN_mod(rsa->dmp1, d, r1, ctx))
        goto err;

    /* calculate d mod (q-1) */
    if (!BN_mod(rsa->dmq1, d, r2, ctx))
        goto err;

    /* calculate inverse of q mod p */
    if (!(rsa->flags & RSA_FLAG_NO_CONSTTIME)) {
        p = &local_p;
        BN_with_flags(p, rsa->p, BN_FLG_CONSTTIME);
    } else
        p = rsa->p;
    if (!BN_mod_inverse(rsa->iqmp, rsa->q, p, ctx))
        goto err;

    if (fips_rsa_pairwise_fail)
        BN_add_word(rsa->n, 1);

    if (!fips_check_rsa(rsa))
        goto err;

    ok = 1;
 err:
    if (ok == -1) {
        RSAerr(RSA_F_RSA_BUILTIN_KEYGEN, ERR_LIB_BN);
        ok = 0;
    }
    if (ctx != NULL) {
        BN_CTX_end(ctx);
        BN_CTX_free(ctx);
    }

    return ok;
}
#endif

static int rsa_builtin_keygen(RSA *rsa, int bits, BIGNUM *e_value,
                              BN_GENCB *cb)
{
    BIGNUM *r0 = NULL, *r1 = NULL, *r2 = NULL, *r3 = NULL, *tmp;
    BIGNUM local_r0, local_d, local_p;
    BIGNUM *pr0, *d, *p;
    int bitsp, bitsq, ok = -1, n = 0;
    BN_CTX *ctx = NULL;

#ifdef OPENSSL_FIPS
    if (FIPS_module_mode()) {
        if (bits < OPENSSL_RSA_FIPS_MIN_MODULUS_BITS) {
            FIPSerr(FIPS_F_RSA_BUILTIN_KEYGEN, FIPS_R_KEY_TOO_SHORT);
            return 0;
        }
        return FIPS_rsa_builtin_keygen(rsa, bits, e_value, cb);
    }
#endif

    ctx = BN_CTX_new();
    if (ctx == NULL)
        goto err;
    BN_CTX_start(ctx);
    r0 = BN_CTX_get(ctx);
    r1 = BN_CTX_get(ctx);
    r2 = BN_CTX_get(ctx);
    r3 = BN_CTX_get(ctx);
    if (r3 == NULL)
        goto err;

    bitsp = (bits + 1) / 2;
    bitsq = bits - bitsp;

    /* We need the RSA components non-NULL */
    if (!rsa->n && ((rsa->n = BN_new()) == NULL))
        goto err;
    if (!rsa->d && ((rsa->d = BN_new()) == NULL))
        goto err;
    if (!rsa->e && ((rsa->e = BN_new()) == NULL))
        goto err;
    if (!rsa->p && ((rsa->p = BN_new()) == NULL))
        goto err;
    if (!rsa->q && ((rsa->q = BN_new()) == NULL))
        goto err;
    if (!rsa->dmp1 && ((rsa->dmp1 = BN_new()) == NULL))
        goto err;
    if (!rsa->dmq1 && ((rsa->dmq1 = BN_new()) == NULL))
        goto err;
    if (!rsa->iqmp && ((rsa->iqmp = BN_new()) == NULL))
        goto err;

    /* prepare minimum p and q difference */
    if (!BN_one(r3))
        goto err;
    if (bitsp > 100 && !BN_lshift(r3, r3, bitsp - 100))
        goto err;

    BN_copy(rsa->e, e_value);

    /* generate p and q */
    for (;;) {
        if (!BN_generate_prime_ex(rsa->p, bitsp, 0, NULL, NULL, cb))
            goto err;
        if (!BN_sub(r2, rsa->p, BN_value_one()))
            goto err;
        if (!BN_gcd(r1, r2, rsa->e, ctx))
            goto err;
        if (BN_is_one(r1))
            break;
        if (!BN_GENCB_call(cb, 2, n++))
            goto err;
    }
    if (!BN_GENCB_call(cb, 3, 0))
        goto err;
    for (;;) {
        /*
         * When generating ridiculously small keys, we can get stuck
         * continually regenerating the same prime values. Check for this and
         * bail if it happens 3 times.
         */
        unsigned int degenerate = 0;
        do {
            if (!BN_generate_prime_ex(rsa->q, bitsq, 0, NULL, NULL, cb))
                goto err;
            if (!BN_sub(r2, rsa->q, rsa->p))
                goto err;
        } while ((BN_ucmp(r2, r3) <= 0) && (++degenerate < 3));
        if (degenerate == 3) {
            ok = 0;             /* we set our own err */
            RSAerr(RSA_F_RSA_BUILTIN_KEYGEN, RSA_R_KEY_SIZE_TOO_SMALL);
            goto err;
        }
        if (!BN_sub(r2, rsa->q, BN_value_one()))
            goto err;
        if (!BN_gcd(r1, r2, rsa->e, ctx))
            goto err;
        if (BN_is_one(r1))
            break;
        if (!BN_GENCB_call(cb, 2, n++))
            goto err;
    }
    if (!BN_GENCB_call(cb, 3, 1))
        goto err;
    if (BN_cmp(rsa->p, rsa->q) < 0) {
        tmp = rsa->p;
        rsa->p = rsa->q;
        rsa->q = tmp;
    }

    /* calculate n */
    if (!BN_mul(rsa->n, rsa->p, rsa->q, ctx))
        goto err;

    /* calculate d */
    if (!BN_sub(r1, rsa->p, BN_value_one()))
        goto err;               /* p-1 */
    if (!BN_sub(r2, rsa->q, BN_value_one()))
        goto err;               /* q-1 */
    if (!BN_mul(r0, r1, r2, ctx))
        goto err;               /* (p-1)(q-1) */
    if (!(rsa->flags & RSA_FLAG_NO_CONSTTIME)) {
        pr0 = &local_r0;
        BN_with_flags(pr0, r0, BN_FLG_CONSTTIME);
    } else
        pr0 = r0;
    if (!BN_mod_inverse(rsa->d, rsa->e, pr0, ctx))
        goto err;               /* d */

    /* set up d for correct BN_FLG_CONSTTIME flag */
    if (!(rsa->flags & RSA_FLAG_NO_CONSTTIME)) {
        d = &local_d;
        BN_with_flags(d, rsa->d, BN_FLG_CONSTTIME);
    } else
        d = rsa->d;

    /* calculate d mod (p-1) */
    if (!BN_mod(rsa->dmp1, d, r1, ctx))
        goto err;

    /* calculate d mod (q-1) */
    if (!BN_mod(rsa->dmq1, d, r2, ctx))
        goto err;

    /* calculate inverse of q mod p */
    if (!(rsa->flags & RSA_FLAG_NO_CONSTTIME)) {
        p = &local_p;
        BN_with_flags(p, rsa->p, BN_FLG_CONSTTIME);
    } else
        p = rsa->p;
    if (!BN_mod_inverse(rsa->iqmp, rsa->q, p, ctx))
        goto err;

    ok = 1;
 err:
    if (ok == -1) {
        RSAerr(RSA_F_RSA_BUILTIN_KEYGEN, ERR_LIB_BN);
        ok = 0;
    }
    if (ctx != NULL) {
        BN_CTX_end(ctx);
        BN_CTX_free(ctx);
    }

    return ok;
}
