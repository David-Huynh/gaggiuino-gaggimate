#include "Ed25519Verify.h"

#include <array>
#include <cstring>
#include <memory>

namespace EspressoRL {
namespace {

// ---------------------------------------------------------------------------
// Streaming FIPS 180-4 SHA-512 (zero heap allocation)
// ---------------------------------------------------------------------------
class Sha512 {
  public:
    Sha512() {
        state_[0] = 0x6a09e667f3bcc908ULL;
        state_[1] = 0xbb67ae8584caa73bULL;
        state_[2] = 0x3c6ef372fe94f82bULL;
        state_[3] = 0xa54ff53a5f1d36f1ULL;
        state_[4] = 0x510e527fade682d1ULL;
        state_[5] = 0x9b05688c2b3e6c1fULL;
        state_[6] = 0x1f83d9abfb41bd6bULL;
        state_[7] = 0x5be0cd19137e2179ULL;
    }

    void update(const std::uint8_t *data, std::size_t len) {
        if (data == nullptr || len == 0) {
            return;
        }
        totalBytes_ += static_cast<std::uint64_t>(len);
        std::size_t pos = 0;
        if (bufLen_ > 0) {
            const std::size_t take = (len < (128 - bufLen_)) ? len : (128 - bufLen_);
            std::memcpy(buf_ + bufLen_, data, take);
            bufLen_ += take;
            pos += take;
            if (bufLen_ == 128) {
                compress(buf_);
                bufLen_ = 0;
            }
        }
        while (pos + 128 <= len) {
            compress(data + pos);
            pos += 128;
        }
        if (pos < len) {
            bufLen_ = len - pos;
            std::memcpy(buf_, data + pos, bufLen_);
        }
    }

    void finalize(std::uint8_t out[64]) {
        const std::uint64_t bitLenLo = totalBytes_ << 3;
        const std::uint64_t bitLenHi = totalBytes_ >> 61;

        buf_[bufLen_++] = 0x80u;
        if (bufLen_ > 112) {
            std::memset(buf_ + bufLen_, 0, 128 - bufLen_);
            compress(buf_);
            bufLen_ = 0;
        }
        std::memset(buf_ + bufLen_, 0, 112 - bufLen_);
        for (int i = 0; i < 8; ++i) {
            buf_[112 + i] = static_cast<std::uint8_t>((bitLenHi >> (56 - 8 * i)) & 0xffu);
            buf_[120 + i] = static_cast<std::uint8_t>((bitLenLo >> (56 - 8 * i)) & 0xffu);
        }
        compress(buf_);
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 8; ++j) {
                out[8 * i + j] = static_cast<std::uint8_t>((state_[i] >> (56 - 8 * j)) & 0xffu);
            }
        }
    }

  private:
    static std::uint64_t rotr(std::uint64_t x, unsigned int n) { return (x >> n) | (x << (64u - n)); }

    void compress(const std::uint8_t block[128]) {
        static constexpr std::uint64_t K[80] = {
            0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
            0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
            0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
            0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
            0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
            0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
            0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
            0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
            0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
            0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
            0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
            0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
            0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
            0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
            0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
            0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
            0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
            0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
            0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
            0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
        };

        std::uint64_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint64_t>(block[8 * i]) << 56) |
                   (static_cast<std::uint64_t>(block[8 * i + 1]) << 48) |
                   (static_cast<std::uint64_t>(block[8 * i + 2]) << 40) |
                   (static_cast<std::uint64_t>(block[8 * i + 3]) << 32) |
                   (static_cast<std::uint64_t>(block[8 * i + 4]) << 24) |
                   (static_cast<std::uint64_t>(block[8 * i + 5]) << 16) |
                   (static_cast<std::uint64_t>(block[8 * i + 6]) << 8) |
                   static_cast<std::uint64_t>(block[8 * i + 7]);
        }
        for (int i = 16; i < 80; ++i) {
            const std::uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const std::uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint64_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint64_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 80; ++i) {
            const std::uint64_t S1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
            const std::uint64_t ch = (e & f) ^ ((~e) & g);
            const std::uint64_t temp1 = h + S1 + ch + K[i] + w[i];
            const std::uint64_t S0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
            const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint64_t temp2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::uint64_t state_[8]{};
    std::uint8_t buf_[128]{};
    std::size_t bufLen_ = 0;
    std::uint64_t totalBytes_ = 0;
};

// ---------------------------------------------------------------------------
// Curve25519 Field GF(2^255 - 19) in 16 x 16-bit limbs (portable int64_t)
// ---------------------------------------------------------------------------
using gf = std::int64_t[16];

constexpr gf GF0 = {0};
constexpr gf GF1 = {1};
constexpr gf D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                  0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
constexpr gf D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                   0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
constexpr gf I_SQRT_M1 = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                          0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
constexpr gf BX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                   0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
constexpr gf BY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                   0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};

constexpr std::uint8_t ORDER_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

std::int64_t floorDivPow2(std::int64_t x, unsigned int shift) {
    const std::int64_t mask = (1LL << shift) - 1LL;
    return (x >= 0) ? (x >> shift) : -(((-x) + mask) >> shift);
}

void set25519(gf o, const gf a) {
    for (int i = 0; i < 16; ++i) {
        o[i] = a[i];
    }
}

void car25519(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += 65536LL;
        const std::int64_t c = floorDivPow2(o[i], 16);
        if (i < 15) {
            o[i + 1] += c - 1LL;
        } else {
            o[0] += 38LL * (c - 1LL);
        }
        o[i] -= c * 65536LL;
    }
}

void A(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) {
        o[i] = a[i] + b[i];
    }
}

void Z(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) {
        o[i] = a[i] - b[i];
    }
}

void M(gf o, const gf a, const gf b) {
    std::int64_t t[31] = {};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; ++i) {
        t[i] += 38LL * t[i + 16];
    }
    for (int i = 0; i < 16; ++i) {
        o[i] = t[i];
    }
    car25519(o);
    car25519(o);
}

void S(gf o, const gf a) { M(o, a, a); }

void inv25519(gf o, const gf i) {
    gf c;
    set25519(c, i);
    for (int a = 253; a >= 0; --a) {
        S(c, c);
        if (a != 2 && a != 4) {
            M(c, c, i);
        }
    }
    set25519(o, c);
}

void pow2523(gf o, const gf i) {
    gf c;
    set25519(c, i);
    for (int a = 250; a >= 0; --a) {
        S(c, c);
        if (a != 1) {
            M(c, c, i);
        }
    }
    set25519(o, c);
}

void pack25519(std::uint8_t o[32], const gf n) {
    gf t, m;
    set25519(t, n);
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffedLL;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffffLL - ((m[i - 1] < 0) ? 1LL : 0LL);
            m[i - 1] &= 0xffffLL;
        }
        m[15] = t[15] - 0x7fffLL - ((m[14] < 0) ? 1LL : 0LL);
        m[14] &= 0xffffLL;
        if (m[15] >= 0) {
            set25519(t, m);
        }
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = static_cast<std::uint8_t>(t[i] & 0xffLL);
        o[2 * i + 1] = static_cast<std::uint8_t>((t[i] >> 8) & 0xffLL);
    }
}

bool unpack25519Canonical(gf o, const std::uint8_t n[32]) {
    // Reject non-canonical y >= 2^255 - 19 (RFC 8032 Section 5.1.3).
    bool allHighFf = ((n[31] & 0x7fu) == 0x7fu);
    for (int i = 30; i >= 1 && allHighFf; --i) {
        if (n[i] != 0xffu) {
            allHighFf = false;
        }
    }
    if (allHighFf && n[0] >= 0xedu) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        o[i] = static_cast<std::int64_t>(n[2 * i]) + (static_cast<std::int64_t>(n[2 * i + 1]) << 8);
    }
    o[15] &= 0x7fffLL;
    return true;
}

bool gfEqual(const gf a, const gf b) {
    std::uint8_t pa[32], pb[32];
    pack25519(pa, a);
    pack25519(pb, b);
    return std::memcmp(pa, pb, 32) == 0;
}

// Extended twisted Edwards coordinates: (X, Y, Z, T)
struct Point {
    gf x;
    gf y;
    gf z;
    gf t;
};

void pointIdentity(Point &p) {
    set25519(p.x, GF0);
    set25519(p.y, GF1);
    set25519(p.z, GF1);
    set25519(p.t, GF0);
}

void pointAdd(Point &r, const Point &p, const Point &q) {
    gf a, b, c, d, e, f, g, h, t;
    Z(a, p.y, p.x);
    Z(t, q.y, q.x);
    M(a, a, t);
    A(b, p.x, p.y);
    A(t, q.x, q.y);
    M(b, b, t);
    M(c, p.t, q.t);
    M(c, c, D2);
    M(d, p.z, q.z);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(r.x, e, f);
    M(r.y, h, g);
    M(r.z, g, f);
    M(r.t, e, h);
}

void scalarMult(Point &out, const Point &base, const std::uint8_t s[32], Point &acc, Point &cur, Point &tmp) {
    pointIdentity(acc);
    cur = base;
    for (int bit = 0; bit < 256; ++bit) {
        if (((s[bit >> 3] >> (bit & 7)) & 1u) != 0u) {
            pointAdd(tmp, acc, cur);
            acc = tmp;
        }
        pointAdd(tmp, cur, cur);
        cur = tmp;
    }
    out = acc;
}

void encodePoint(std::uint8_t out[32], const Point &p) {
    gf zi, tx, ty;
    inv25519(zi, p.z);
    M(tx, p.x, zi);
    M(ty, p.y, zi);
    pack25519(out, ty);
    std::uint8_t px[32];
    pack25519(px, tx);
    out[31] ^= static_cast<std::uint8_t>((px[0] & 1u) << 7);
}

bool decodePoint(Point &out, const std::uint8_t enc[32]) {
    if (!unpack25519Canonical(out.y, enc)) {
        return false;
    }
    set25519(out.z, GF1);

    gf y2, u, v, v3, v7, uv7, x, x2, vx2, negU;
    S(y2, out.y);
    Z(u, y2, GF1);
    M(v, y2, D);
    A(v, v, GF1);
    S(v3, v);
    M(v3, v3, v);
    S(v7, v3);
    M(v7, v7, v);
    M(uv7, u, v7);
    pow2523(x, uv7);
    M(x, x, v3);
    M(x, x, u);

    S(x2, x);
    M(vx2, v, x2);
    if (!gfEqual(vx2, u)) {
        Z(negU, GF0, u);
        if (!gfEqual(vx2, negU)) {
            return false;
        }
        M(x, x, I_SQRT_M1);
    }

    std::uint8_t px[32];
    pack25519(px, x);
    bool xIsZero = true;
    for (int i = 0; i < 32; ++i) {
        if (px[i] != 0u) {
            xIsZero = false;
            break;
        }
    }
    const std::uint8_t sign = (enc[31] >> 7) & 1u;
    if (xIsZero && sign == 1u) {
        return false;
    }
    if ((px[0] & 1u) != sign) {
        Z(x, GF0, x);
    }
    set25519(out.x, x);
    M(out.t, out.x, out.y);
    return true;
}

bool scalarLessThanL(const std::uint8_t s[32]) {
    for (int i = 31; i >= 0; --i) {
        if (s[i] < ORDER_L[i]) {
            return true;
        }
        if (s[i] > ORDER_L[i]) {
            return false;
        }
    }
    return false;
}

void reduceScalarModL(std::uint8_t out[32], const std::uint8_t in64[64]) {
    std::int64_t x[64];
    for (int i = 0; i < 64; ++i) {
        x[i] = static_cast<std::int64_t>(in64[i]);
    }
    for (int i = 63; i >= 32; --i) {
        std::int64_t carry = 0;
        for (int j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16LL * x[i] * static_cast<std::int64_t>(ORDER_L[j - (i - 32)]);
            carry = floorDivPow2(x[j] + 128LL, 8);
            x[j] -= carry * 256LL;
        }
        x[i - 12] += carry;
        x[i] = 0;
    }
    std::int64_t carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * static_cast<std::int64_t>(ORDER_L[j]);
        carry = floorDivPow2(x[j], 8);
        x[j] -= carry * 256LL;
    }
    for (int j = 0; j < 32; ++j) {
        x[j] -= carry * static_cast<std::int64_t>(ORDER_L[j]);
    }
    for (int i = 0; i < 32; ++i) {
        const std::int64_t c = floorDivPow2(x[i], 8);
        x[i] -= c * 256LL;
        if (i + 1 < 32) {
            x[i + 1] += c;
        }
        out[i] = static_cast<std::uint8_t>(x[i] & 0xffLL);
    }
}

} // namespace

bool ed25519Verify(const std::uint8_t publicKey[ED25519_PUBLIC_KEY_BYTES], const std::uint8_t *message,
                   std::size_t messageLen, const std::uint8_t signature[ED25519_SIGNATURE_BYTES]) {
    if (publicKey == nullptr || signature == nullptr || (message == nullptr && messageLen > 0)) {
        return false;
    }
    if (!scalarLessThanL(signature + 32)) {
        return false;
    }

    // Keep the ~5 KB working points & SHA-512 state off small embedded task stacks.
    struct VerifyWorkspace {
        Point aPt;
        Point rPt;
        Point baseB;
        Point lhs;
        Point kA;
        Point rhs;
        Point acc;
        Point cur;
        Point tmp;
        Sha512 hasher;
    };
    auto ws = std::make_unique<VerifyWorkspace>();

    if (!decodePoint(ws->aPt, publicKey) || !decodePoint(ws->rPt, signature)) {
        return false;
    }

    ws->hasher.update(signature, 32);
    ws->hasher.update(publicKey, 32);
    if (messageLen > 0) {
        ws->hasher.update(message, messageLen);
    }
    std::uint8_t hDigest[64];
    ws->hasher.finalize(hDigest);

    std::uint8_t kScalar[32];
    reduceScalarModL(kScalar, hDigest);

    set25519(ws->baseB.x, BX);
    set25519(ws->baseB.y, BY);
    set25519(ws->baseB.z, GF1);
    M(ws->baseB.t, ws->baseB.x, ws->baseB.y);

    scalarMult(ws->lhs, ws->baseB, signature + 32, ws->acc, ws->cur, ws->tmp);
    scalarMult(ws->kA, ws->aPt, kScalar, ws->acc, ws->cur, ws->tmp);
    pointAdd(ws->rhs, ws->rPt, ws->kA);

    std::uint8_t encLhs[32], encRhs[32];
    encodePoint(encLhs, ws->lhs);
    encodePoint(encRhs, ws->rhs);
    return std::memcmp(encLhs, encRhs, 32) == 0;
}

} // namespace EspressoRL
