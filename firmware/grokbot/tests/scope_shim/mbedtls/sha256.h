#pragma once
// Host-only SHA API adapter: CommonCrypto on macOS, OpenSSL EVP elsewhere.
// Production compiles voice_scope.cpp against ESP-IDF's actual mbedtls headers.
#include <cstddef>
#include <cstring>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
using mbedtls_sha256_context = CC_SHA256_CTX;
inline void mbedtls_sha256_init(mbedtls_sha256_context *ctx) { std::memset(ctx, 0, sizeof(*ctx)); }
inline void mbedtls_sha256_free(mbedtls_sha256_context *ctx) { std::memset(ctx, 0, sizeof(*ctx)); }
inline int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224)
{ return !is224 && CC_SHA256_Init(ctx) ? 0 : -1; }
inline int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *data, size_t size)
{ return CC_SHA256_Update(ctx, data, static_cast<CC_LONG>(size)) ? 0 : -1; }
inline int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char *out)
{ return CC_SHA256_Final(out, ctx) ? 0 : -1; }
#else
#include <openssl/evp.h>
struct mbedtls_sha256_context { EVP_MD_CTX *value; };
inline void mbedtls_sha256_init(mbedtls_sha256_context *ctx) { ctx->value = EVP_MD_CTX_new(); }
inline void mbedtls_sha256_free(mbedtls_sha256_context *ctx) { EVP_MD_CTX_free(ctx->value); }
inline int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224)
{ return ctx->value && !is224 && EVP_DigestInit_ex(ctx->value, EVP_sha256(), nullptr) == 1 ? 0 : -1; }
inline int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *data, size_t size)
{ return EVP_DigestUpdate(ctx->value, data, size) == 1 ? 0 : -1; }
inline int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char *out)
{ return EVP_DigestFinal_ex(ctx->value, out, nullptr) == 1 ? 0 : -1; }
#endif
