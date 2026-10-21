/* RSA-2048 / PKCS#1 v1.5 / SHA-256 index authentication.
 * The public key is exactly 512 lowercase hex digits plus LF (modulus,
 * exponent fixed at 65537). No private-key operations live in the client. */
#include "pkg.h"
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef PKG_WOLFSSL
#include "ripple_crypto.h"
#include <wolfssl/wolfcrypt/rsa.h>
#endif
static int unhex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
int pkg_verify(const char *index, const char *signature, const char *keyfile) {
    static const PkgByte prefix[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0,    0x04, 0x20};
    PkgByte modulus[256], sig[256], actual[256], expected[51];
    char hash[65];
    unsigned i;
    FILE *f;
    f = fopen(keyfile, "r");
    if (!f)
        return pkg_fail("missing pinned repository key", keyfile);
    for (i = 0; i < 256; i++) {
        int a = unhex((char)fgetc(f)), b = unhex((char)fgetc(f));
        if (a < 0 || b < 0) {
            fclose(f);
            return pkg_fail("invalid RSA public modulus", keyfile);
        }
        modulus[i] = (PkgByte)(a * 16 + b);
    }
    if (fgetc(f) != '\n' || fgetc(f) != EOF || !(modulus[0] & 128) || !(modulus[255] & 1)) {
        fclose(f);
        return pkg_fail("invalid RSA-2048 public key", keyfile);
    }
    fclose(f);
    f = fopen(signature, "rb");
    if (!f)
        return pkg_fail("missing repository signature", signature);
    if (fread(sig, 1, 256, f) != 256 || fgetc(f) != EOF) {
        fclose(f);
        return pkg_fail("invalid signature length", signature);
    }
    fclose(f);
    if (pkg_hash_file(index, hash, NULL))
        return -1;
    memcpy(expected, prefix, sizeof(prefix));
    for (i = 0; i < 32; i++)
        expected[19 + i] = (PkgByte)(unhex(hash[2 * i]) * 16 + unhex(hash[2 * i + 1]));
#ifdef PKG_WOLFSSL
    {
        RsaKey key;
        PkgByte exponent[] = {1, 0, 1};
        int n;
        ripple_crypto_init();
        if (wc_InitRsaKey(&key, NULL))
            return pkg_fail("cannot initialize RSA verifier", "");
        n = wc_RsaPublicKeyDecodeRaw(modulus, 256, exponent, 3, &key);
        if (!n)
            n = wc_RsaSSL_Verify(sig, 256, actual, 256, &key);
        wc_FreeRsaKey(&key);
        if (n != 51 || memcmp(actual, expected, 51))
            return pkg_fail("repository signature rejected", index);
    }
#else
    /* Host bootstrap delegates public RSA arithmetic to the system OpenSSL.
     * Work files are private and unique; argv is passed without a shell. */
    {
        char directory[512], der[512], out[512];
        const char *tmp = getenv("TMPDIR");
        int status = 0;
        pid_t child;
        PkgByte head[] = {0x30, 0x82, 1, 0x0a, 2, 0x82, 1, 1, 0}, tail[] = {2, 3, 1, 0, 1};
        if (!tmp)
            tmp = "/tmp";
        if (strlen(tmp) > 440)
            return pkg_fail("temporary directory too long", tmp);
        snprintf(directory, sizeof(directory), "%s/pkg-verify-XXXXXX", tmp);
        if (!mkdtemp(directory))
            return pkg_fail("cannot create verifier workspace", tmp);
        pkg_join(der, directory, "key.der");
        pkg_join(out, directory, "digest");
        f = fopen(der, "wb");
        if (!f) {
            pkg_remove_tree(directory);
            return pkg_fail("cannot stage public key", der);
        }
        if (fwrite(head, 1, sizeof(head), f) != sizeof(head) || fwrite(modulus, 1, 256, f) != 256 ||
            fwrite(tail, 1, sizeof(tail), f) != sizeof(tail))
            status = 1;
        if (fclose(f))
            status = 1;
        child = status ? -1 : fork();
        if (!child) {
            execlp("openssl", "openssl", "pkeyutl", "-verifyrecover", "-pubin", "-keyform", "DER",
                   "-inkey", der, "-in", signature, "-out", out, (char *)NULL);
            _exit(127);
        }
        if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            status = 1;
        else {
            f = fopen(out, "rb");
            if (!f)
                status = 1;
            else {
                status = fread(actual, 1, 51, f) != 51 || fgetc(f) != EOF ||
                         memcmp(actual, expected, 51);
                fclose(f);
            }
        }
        pkg_remove_tree(directory);
        if (status)
            return pkg_fail("repository signature rejected", index);
    }
#endif
    return 0;
}
