/* B44 O2 Win32-backend provider test (runs as a PE; off-device under wine64).
 * 1. Every GnuTLS symbol Wine's Unix side looks up resolves through
 *    uwph_dlopen("libgnutls.so.30") / uwph_dlsym from the static registry.
 * 2. The linked-in provider completes an in-memory TLS handshake and its
 *    X.509 verification accepts the trusted chain and rejects an untrusted
 *    issuer and a wrong host name.  Validation is never disabled. */
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <gnutls/crypto.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "uwph/uwph.h"

int uwph_tls_provider_register(void);

static const char *const wine_syms[] = {
#define X(f) #f,
#include "wine_gnutls_symbols.h"
#undef X
};

static int failures;
#define CHECK(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); failures += !ok_; } while (0)

struct pipe { unsigned char buf[1 << 16]; size_t len; };
struct end { struct pipe *in, *out; gnutls_session_t s; };

static ssize_t push(gnutls_transport_ptr_t p, const void *d, size_t n)
{
    struct end *e = p;
    if (e->out->len + n > sizeof(e->out->buf)) { gnutls_transport_set_errno(e->s, EAGAIN); return -1; }
    memcpy(e->out->buf + e->out->len, d, n);
    e->out->len += n;
    return n;
}

static ssize_t pull(gnutls_transport_ptr_t p, void *d, size_t n)
{
    struct end *e = p;
    if (!e->in->len) { gnutls_transport_set_errno(e->s, EAGAIN); return -1; }
    if (n > e->in->len) n = e->in->len;
    memcpy(d, e->in->buf, n);
    memmove(e->in->buf, e->in->buf + n, e->in->len - n);
    e->in->len -= n;
    return n;
}

static int make_cert(gnutls_x509_crt_t *crt, gnutls_x509_privkey_t *key, const char *cn,
                     gnutls_x509_crt_t issuer, gnutls_x509_privkey_t issuer_key)
{
    unsigned char serial[8];
    time_t now = time(NULL);
    int rc;
    if ((rc = gnutls_x509_privkey_init(key)) ||
        (rc = gnutls_x509_privkey_generate(*key, GNUTLS_PK_ECDSA, GNUTLS_CURVE_TO_BITS(GNUTLS_ECC_CURVE_SECP256R1), 0)) ||
        (rc = gnutls_x509_crt_init(crt)) || (rc = gnutls_rnd(GNUTLS_RND_NONCE, serial, sizeof(serial))))
        return rc;
    serial[0] &= 0x7f;
    if ((rc = gnutls_x509_crt_set_version(*crt, 3)) || (rc = gnutls_x509_crt_set_serial(*crt, serial, sizeof(serial))) ||
        (rc = gnutls_x509_crt_set_activation_time(*crt, now - 3600)) ||
        (rc = gnutls_x509_crt_set_expiration_time(*crt, now + 86400)) ||
        (rc = gnutls_x509_crt_set_dn_by_oid(*crt, GNUTLS_OID_X520_COMMON_NAME, 0, cn, strlen(cn))) ||
        (rc = gnutls_x509_crt_set_key(*crt, *key)))
        return rc;
    if (!issuer) {
        if ((rc = gnutls_x509_crt_set_basic_constraints(*crt, 1, -1)) ||
            (rc = gnutls_x509_crt_set_key_usage(*crt, GNUTLS_KEY_KEY_CERT_SIGN | GNUTLS_KEY_CRL_SIGN)))
            return rc;
    } else if ((rc = gnutls_x509_crt_set_basic_constraints(*crt, 0, -1)) ||
               (rc = gnutls_x509_crt_set_key_usage(*crt, GNUTLS_KEY_DIGITAL_SIGNATURE)) ||
               (rc = gnutls_x509_crt_set_key_purpose_oid(*crt, GNUTLS_KP_TLS_WWW_SERVER, 0)) ||
               (rc = gnutls_x509_crt_set_subject_alt_name(*crt, GNUTLS_SAN_DNSNAME, cn, strlen(cn), GNUTLS_FSAN_SET)))
        return rc;
    return gnutls_x509_crt_sign2(*crt, issuer ? issuer : *crt, issuer ? issuer_key : *key, GNUTLS_DIG_SHA256, 0);
}

/* Handshake a client trusting `trust` against a server presenting `crt`,
 * then verify the peer as `host`.  Returns the GnuTLS status bits, or -1. */
static int handshake(gnutls_x509_crt_t trust, gnutls_x509_crt_t crt, gnutls_x509_privkey_t key, const char *host, char *info, size_t infolen)
{
    static struct pipe c2s, s2c;
    gnutls_certificate_credentials_t sc, cc;
    gnutls_session_t c, s;
    struct end ce, se;
    int rc_c = GNUTLS_E_AGAIN, rc_s = GNUTLS_E_AGAIN, ret = -1;
    unsigned status = ~0u;
    char buf[16];

    c2s.len = s2c.len = 0;
    gnutls_certificate_allocate_credentials(&sc);
    gnutls_certificate_allocate_credentials(&cc);
    gnutls_certificate_set_x509_key(sc, &crt, 1, key);
    gnutls_certificate_set_x509_trust(cc, &trust, 1);
    gnutls_init(&s, GNUTLS_SERVER | GNUTLS_NONBLOCK);
    gnutls_init(&c, GNUTLS_CLIENT | GNUTLS_NONBLOCK);
    gnutls_set_default_priority(s);
    gnutls_set_default_priority(c);
    gnutls_credentials_set(s, GNUTLS_CRD_CERTIFICATE, sc);
    gnutls_credentials_set(c, GNUTLS_CRD_CERTIFICATE, cc);
    gnutls_server_name_set(c, GNUTLS_NAME_DNS, host, strlen(host));
    ce = (struct end){ &s2c, &c2s, c };
    se = (struct end){ &c2s, &s2c, s };
    gnutls_transport_set_ptr(c, &ce);
    gnutls_transport_set_ptr(s, &se);
    gnutls_transport_set_push_function(c, push);
    gnutls_transport_set_push_function(s, push);
    gnutls_transport_set_pull_function(c, pull);
    gnutls_transport_set_pull_function(s, pull);
    for (int i = 0; i < 64 && (rc_c || rc_s); i++) {
        if (rc_c) rc_c = gnutls_handshake(c);
        if (rc_s) rc_s = gnutls_handshake(s);
        if ((rc_c < 0 && gnutls_error_is_fatal(rc_c)) || (rc_s < 0 && gnutls_error_is_fatal(rc_s))) break;
    }
    if (rc_c || rc_s) {
        snprintf(info, infolen, "handshake client=%s server=%s", gnutls_strerror(rc_c), gnutls_strerror(rc_s));
    } else if (gnutls_certificate_verify_peers3(c, host, &status) == 0 &&
               gnutls_record_send(c, "ping", 4) == 4 && gnutls_record_recv(s, buf, sizeof(buf)) == 4 && !memcmp(buf, "ping", 4)) {
        snprintf(info, infolen, "%s %s", gnutls_protocol_get_name(gnutls_protocol_get_version(c)),
                 gnutls_cipher_get_name(gnutls_cipher_get(c)));
        ret = (int)status;
    } else {
        snprintf(info, infolen, "verify/record failed");
    }
    gnutls_deinit(c);
    gnutls_deinit(s);
    gnutls_certificate_free_credentials(cc);
    gnutls_certificate_free_credentials(sc);
    return ret;
}

int main(void)
{
    gnutls_x509_crt_t ca, other_ca, srv;
    gnutls_x509_privkey_t ca_key, other_key, srv_key;
    int (*global_init)(void);
    char info[128];
    void *h;
    int n = 0, st;

    CHECK(uwph_tls_provider_register() == 0, "register libgnutls.so.30 in the uwph dl registry");
    h = uwph_dlopen("/usr/lib/x86_64-linux-gnu/libgnutls.so.30", 0);   /* the name Wine's bcrypt.so asks for */
    CHECK(h != NULL, "uwph_dlopen(libgnutls.so.30) from the static registry");
    for (size_t i = 0; i < sizeof(wine_syms) / sizeof(wine_syms[0]); i++) {
        if (h && uwph_dlsym(h, wine_syms[i])) n++;
        else printf("  missing %s\n", wine_syms[i]);
    }
    CHECK(n == (int)(sizeof(wine_syms) / sizeof(wine_syms[0])), "uwph_dlsym resolves %d/%d Wine GnuTLS symbols", n,
          (int)(sizeof(wine_syms) / sizeof(wine_syms[0])));
    global_init = h ? (int (*)(void))uwph_dlsym(h, "gnutls_global_init") : NULL;
    /* gnutls_check_version is not one Wine looks up, so it is not in the registry. */
    CHECK(!strcmp(gnutls_check_version(NULL), "3.8.9"), "linked provider is GnuTLS %s", gnutls_check_version(NULL));
    CHECK(global_init && global_init() == 0, "gnutls_global_init via dlsym");

    CHECK(make_cert(&ca, &ca_key, "uwph B44 test CA", NULL, NULL) == 0 &&
          make_cert(&other_ca, &other_key, "uwph B44 other CA", NULL, NULL) == 0 &&
          make_cert(&srv, &srv_key, "uwph.test", ca, ca_key) == 0, "generate ECDSA P-256 CA, other CA, server cert");
    st = handshake(ca, srv, srv_key, "uwph.test", info, sizeof(info));
    CHECK(st == 0, "trusted chain: handshake + verify status=0x%x (%s)", (unsigned)st, info);
    st = handshake(other_ca, srv, srv_key, "uwph.test", info, sizeof(info));
    CHECK(st > 0 && (st & GNUTLS_CERT_SIGNER_NOT_FOUND), "untrusted issuer rejected status=0x%x (%s)", (unsigned)st, info);
    st = handshake(ca, srv, srv_key, "wrong.test", info, sizeof(info));
    CHECK(st > 0 && (st & GNUTLS_CERT_UNEXPECTED_OWNER), "wrong host rejected status=0x%x (%s)", (unsigned)st, info);
    printf("SUMMARY provider_test failures=%d\n", failures);
    fflush(stdout);
    return failures != 0;
}
