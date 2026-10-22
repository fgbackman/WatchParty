/* wp_ec.c — P-256 ECDH with Apple's Security framework (macOS 10.12+, iOS 10+).
 * See wp_lobby.h. */
#include "wp_lobby.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <string.h>

static CFMutableDictionaryRef ec_attrs(int public_key)
{
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int bits = 256;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &bits);
    CFDictionarySetValue(d, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(d, kSecAttrKeySizeInBits, n);
    if (public_key) CFDictionarySetValue(d, kSecAttrKeyClass, kSecAttrKeyClassPublic);
    CFRelease(n);
    return d;
}

int wp_ec_generate(wp_eckey *k)
{
    memset(k, 0, sizeof *k);
    CFMutableDictionaryRef attrs = ec_attrs(0);
    CFErrorRef err = NULL;
    SecKeyRef priv = SecKeyCreateRandomKey(attrs, &err);
    CFRelease(attrs);
    if (!priv) { if (err) CFRelease(err); return 0; }
    SecKeyRef pub = SecKeyCopyPublicKey(priv);
    CFDataRef data = pub ? SecKeyCopyExternalRepresentation(pub, &err) : NULL;
    if (pub) CFRelease(pub);
    if (!data || CFDataGetLength(data) != WP_EC_PUB_LEN) {
        if (data) CFRelease(data);
        if (err) CFRelease(err);
        CFRelease(priv);
        return 0;
    }
    memcpy(k->pub, CFDataGetBytePtr(data), WP_EC_PUB_LEN);
    CFRelease(data);
    k->priv = (void *)priv;                               /* owned */
    return 1;
}

int wp_ec_shared(const wp_eckey *k, const uint8_t peer_pub[WP_EC_PUB_LEN], uint8_t shared[32])
{
    if (!k || !k->priv || peer_pub[0] != 0x04) return 0;
    CFDataRef pd = CFDataCreate(NULL, peer_pub, WP_EC_PUB_LEN);
    CFMutableDictionaryRef attrs = ec_attrs(1);
    CFErrorRef err = NULL;
    SecKeyRef peer = SecKeyCreateWithData(pd, attrs, &err);    /* rejects points not on the curve */
    CFRelease(pd);
    CFRelease(attrs);
    if (!peer) { if (err) CFRelease(err); return 0; }
    CFDictionaryRef params = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef s = SecKeyCopyKeyExchangeResult((SecKeyRef)k->priv, kSecKeyAlgorithmECDHKeyExchangeStandard, peer, params, &err);
    CFRelease(params);
    CFRelease(peer);
    if (!s || CFDataGetLength(s) != 32) {
        if (s) CFRelease(s);
        if (err) CFRelease(err);
        return 0;
    }
    memcpy(shared, CFDataGetBytePtr(s), 32);
    CFRelease(s);
    return 1;
}

void wp_ec_free(wp_eckey *k)
{
    if (k && k->priv) CFRelease((SecKeyRef)k->priv);
    if (k) memset(k, 0, sizeof *k);
}
