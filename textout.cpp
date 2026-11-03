// TPMExtractor fork - deterministic printf-style formatting and CRLF text files.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#include "textout.h"

#include "platform.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace tpmx {

namespace {

// Unsigned big integer, little-endian base 2^32 limbs, fixed capacity. format_fixed needs at
// most 53 (mantissa) + 971 (largest binary exponent) + 333 (10^100) = 1357 bits = 43 limbs.
struct BigUint
{
    static const int kCapacity = 48;
    uint32_t d[kCapacity];
    int n = 0;  // limbs in use; d[n-1] != 0 when n > 0

    void set_u64(uint64_t v)
    {
        n = 0;
        while (v) {
            d[n++] = (uint32_t)v;
            v >>= 32;
        }
    }
    bool is_zero() const { return n == 0; }
    void trim()
    {
        while (n > 0 && d[n - 1] == 0)
            --n;
    }
    void mul_small(uint32_t m)
    {
        uint64_t carry = 0;
        for (int i = 0; i < n; i++) {
            uint64_t t = (uint64_t)d[i] * m + carry;
            d[i] = (uint32_t)t;
            carry = t >> 32;
        }
        if (carry)
            d[n++] = (uint32_t)carry;
    }
    void add_small(uint32_t a)
    {
        uint64_t carry = a;
        for (int i = 0; i < n && carry; i++) {
            uint64_t t = (uint64_t)d[i] + carry;
            d[i] = (uint32_t)t;
            carry = t >> 32;
        }
        if (carry)
            d[n++] = (uint32_t)carry;
    }
    void shl(int bits)
    {
        if (n == 0 || bits <= 0)
            return;
        const int limbs = bits / 32, r = bits % 32;
        if (r) {
            uint32_t carry = 0;
            for (int i = 0; i < n; i++) {
                uint32_t v = d[i];
                d[i] = (v << r) | carry;
                carry = v >> (32 - r);
            }
            if (carry)
                d[n++] = carry;
        }
        if (limbs) {
            for (int i = n - 1; i >= 0; i--)
                d[i + limbs] = d[i];
            for (int i = 0; i < limbs; i++)
                d[i] = 0;
            n += limbs;
        }
    }
    bool bit(int i) const
    {
        if (i < 0)
            return false;
        const int l = i / 32;
        if (l >= n)
            return false;
        return ((d[l] >> (i % 32)) & 1u) != 0;
    }
    // True if any bit below position i is set.
    bool any_below(int i) const
    {
        if (i <= 0)
            return false;
        const int l = i / 32, r = i % 32;
        for (int k = 0; k < l && k < n; k++)
            if (d[k])
                return true;
        if (l < n && r && (d[l] & ((1u << r) - 1u)))
            return true;
        return false;
    }
    void shr(int bits)
    {
        if (bits <= 0 || n == 0)
            return;
        const int limbs = bits / 32, r = bits % 32;
        if (limbs >= n) {
            n = 0;
            return;
        }
        for (int i = 0; i + limbs < n; i++)
            d[i] = d[i + limbs];
        n -= limbs;
        if (r) {
            for (int i = 0; i < n; i++) {
                uint32_t hi = (i + 1 < n) ? d[i + 1] : 0;
                d[i] = (d[i] >> r) | (hi << (32 - r));
            }
        }
        trim();
    }
    // this /= m; returns the remainder.
    uint32_t divmod_small(uint32_t m)
    {
        uint64_t rem = 0;
        for (int i = n - 1; i >= 0; i--) {
            uint64_t cur = (rem << 32) | d[i];
            d[i] = (uint32_t)(cur / m);
            rem = cur % m;
        }
        trim();
        return (uint32_t)rem;
    }
    std::string to_decimal() const
    {
        if (n == 0)
            return "0";
        BigUint t = *this;
        std::vector<uint32_t> parts;  // base 10^9, least significant first
        while (!t.is_zero())
            parts.push_back(t.divmod_small(1000000000u));
        std::string s = std::to_string(parts.back());
        for (size_t i = parts.size() - 1; i-- > 0;) {
            std::string chunk = std::to_string(parts[i]);
            s.append(9 - chunk.size(), '0');
            s += chunk;
        }
        return s;
    }
};

inline char upper_ascii(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

const char kDigitsLower[] = "0123456789abcdef";
const char kDigitsUpper[] = "0123456789ABCDEF";

std::string to_base(unsigned long long v, unsigned base, bool upper)
{
    if (v == 0)
        return "0";
    const char* digits = upper ? kDigitsUpper : kDigitsLower;
    char buf[64];
    int i = 64;
    while (v) {
        buf[--i] = digits[v % base];
        v /= base;
    }
    return std::string(buf + i, buf + 64);
}

struct Spec
{
    bool left = false, plus = false, space = false, alt = false, zero = false;
    int width = 0;
    int prec = -1;  // -1: not given
};

// sign/prefix, then zero padding (if requested), then the body; or space padding.
void emit_padded(std::string& out, const Spec& sp, const std::string& sign, const std::string& body, bool zero_ok)
{
    const size_t len = sign.size() + body.size();
    if (sp.width <= 0 || len >= (size_t)sp.width) {
        out += sign;
        out += body;
        return;
    }
    const size_t pad = (size_t)sp.width - len;
    if (sp.left) {
        out += sign;
        out += body;
        out.append(pad, ' ');
    } else if (sp.zero && zero_ok) {
        out += sign;
        out.append(pad, '0');
        out += body;
    } else {
        out.append(pad, ' ');
        out += sign;
        out += body;
    }
}

void emit_integer(std::string& out, const Spec& sp, bool negative, unsigned long long magnitude, unsigned base,
                  bool upper, bool is_signed)
{
    std::string digits;
    if (sp.prec == 0 && magnitude == 0)
        digits = "";
    else
        digits = to_base(magnitude, base, upper);
    if (sp.prec > 0 && digits.size() < (size_t)sp.prec)
        digits.insert(0, (size_t)sp.prec - digits.size(), '0');
    std::string sign;
    if (is_signed) {
        if (negative)
            sign = "-";
        else if (sp.plus)
            sign = "+";
        else if (sp.space)
            sign = " ";
    }
    if (sp.alt) {
        if (base == 8 && (digits.empty() || digits[0] != '0'))
            digits.insert(0, "0");
        else if (base == 16 && magnitude != 0)
            sign += upper ? "0X" : "0x";
    }
    // The 0 flag is ignored when a precision is given (C11 7.21.6.1).
    emit_padded(out, sp, sign, digits, sp.prec < 0);
}

void emit_fixed(std::string& out, const Spec& sp, double v, bool upper)
{
    std::string body = format_fixed(v, sp.prec < 0 ? 6 : sp.prec, upper);
    std::string sign;
    if (!body.empty() && body[0] == '-') {
        sign = "-";
        body.erase(0, 1);
    } else if (sp.plus) {
        sign = "+";
    } else if (sp.space) {
        sign = " ";
    }
    const bool finite = !body.empty() && body[0] >= '0' && body[0] <= '9';
    if (finite && sp.alt && sp.prec == 0)
        body += '.';
    emit_padded(out, sp, sign, body, finite);
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
// %e %g %a: not used by any output file; delegated to the C library (platform-dependent).
void emit_libc_float(std::string& out, const Spec& sp, char conv, double v)
{
    std::string f = "%";
    if (sp.left)
        f += '-';
    if (sp.plus)
        f += '+';
    if (sp.space)
        f += ' ';
    if (sp.alt)
        f += '#';
    if (sp.zero)
        f += '0';
    if (sp.width > 0)
        f += std::to_string(sp.width);
    if (sp.prec >= 0)
        f += "." + std::to_string(sp.prec);
    f += conv;
    char small[128];
    int n = std::snprintf(small, sizeof small, f.c_str(), v);
    if (n < 0)
        return;
    if ((size_t)n < sizeof small) {
        out.append(small, (size_t)n);
        return;
    }
    std::vector<char> big((size_t)n + 1);
    std::snprintf(big.data(), big.size(), f.c_str(), v);
    out.append(big.data(), (size_t)n);
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace

std::string format_fixed(double v, int precision, bool upper)
{
    if (precision < 0)
        precision = 6;
    if (precision > 100)
        precision = 100;
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    const bool neg = (bits >> 63) != 0;
    const int bexp = (int)((bits >> 52) & 0x7FF);
    const uint64_t frac = bits & 0xFFFFFFFFFFFFFull;
    if (bexp == 0x7FF) {
        std::string s;
        if (frac == 0)
            s = neg ? "-inf" : "inf";
        else if (bits == 0xFFF8000000000000ull)
            s = "-nan(ind)";
        else
            s = neg ? "-nan" : "nan";
        if (upper)
            for (char& c : s)
                c = upper_ascii(c);
        return s;
    }
    uint64_t m;
    int e;
    if (bexp == 0) {
        m = frac;
        e = -1074;
    } else {
        m = frac | (1ull << 52);
        e = bexp - 1075;
    }
    // |v| = m * 2^e. Compute N = round_half_even(|v| * 10^precision) exactly.
    static const uint32_t kPow10[10] = {1u,      10u,      100u,      1000u,      10000u,
                                        100000u, 1000000u, 10000000u, 100000000u, 1000000000u};
    BigUint x;
    x.set_u64(m);
    for (int p = precision; p > 0;) {
        const int k = p > 9 ? 9 : p;
        x.mul_small(kPow10[k]);
        p -= k;
    }
    if (e >= 0) {
        x.shl(e);
    } else {
        const int s = -e;
        const bool half = x.bit(s - 1);
        const bool sticky = x.any_below(s - 1);
        x.shr(s);
        if (half && (sticky || x.bit(0)))
            x.add_small(1);
    }
    std::string digits = x.to_decimal();
    if (digits.size() <= (size_t)precision)
        digits.insert(0, (size_t)precision + 1 - digits.size(), '0');
    std::string out;
    if (neg)
        out += '-';
    out.append(digits, 0, digits.size() - (size_t)precision);
    if (precision > 0) {
        out += '.';
        out.append(digits, digits.size() - (size_t)precision, std::string::npos);
    }
    return out;
}

void vformat_append(std::string& out, const char* fmt, va_list ap)
{
    const char* p = fmt;
    while (*p) {
        if (*p != '%') {
            const char* q = std::strchr(p, '%');
            if (!q)
                q = p + std::strlen(p);
            out.append(p, q);
            p = q;
            continue;
        }
        const char* start = p++;
        if (*p == '%') {
            out += '%';
            ++p;
            continue;
        }
        Spec sp;
        for (;; ++p) {
            if (*p == '-')
                sp.left = true;
            else if (*p == '+')
                sp.plus = true;
            else if (*p == ' ')
                sp.space = true;
            else if (*p == '#')
                sp.alt = true;
            else if (*p == '0')
                sp.zero = true;
            else
                break;
        }
        if (*p == '*') {
            int w = va_arg(ap, int);
            if (w < 0) {
                sp.left = true;
                w = -w;
            }
            sp.width = w;
            ++p;
        } else {
            while (*p >= '0' && *p <= '9')
                sp.width = sp.width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            ++p;
            if (*p == '*') {
                int pr = va_arg(ap, int);
                sp.prec = pr < 0 ? -1 : pr;
                ++p;
            } else {
                sp.prec = 0;
                while (*p >= '0' && *p <= '9')
                    sp.prec = sp.prec * 10 + (*p++ - '0');
            }
        }
        enum { L_NONE, L_HH, L_H, L_L, L_LL, L_Z, L_J, L_T, L_BIG } len = L_NONE;
        if (p[0] == 'h' && p[1] == 'h') {
            len = L_HH;
            p += 2;
        } else if (p[0] == 'h') {
            len = L_H;
            ++p;
        } else if (p[0] == 'l' && p[1] == 'l') {
            len = L_LL;
            p += 2;
        } else if (p[0] == 'l') {
            len = L_L;
            ++p;
        } else if (p[0] == 'z') {
            len = L_Z;
            ++p;
        } else if (p[0] == 'j') {
            len = L_J;
            ++p;
        } else if (p[0] == 't') {
            len = L_T;
            ++p;
        } else if (p[0] == 'L') {
            len = L_BIG;
            ++p;
        }
        const char conv = *p;
        if (!conv) {  // truncated conversion: print it as written
            out.append(start);
            return;
        }
        ++p;
        switch (conv) {
        case 'd':
        case 'i': {
            long long v;
            switch (len) {
            case L_HH: v = (signed char)va_arg(ap, int); break;
            case L_H: v = (short)va_arg(ap, int); break;
            case L_L: v = va_arg(ap, long); break;
            case L_LL: v = va_arg(ap, long long); break;
            case L_Z: v = (long long)va_arg(ap, ptrdiff_t); break;
            case L_J: v = (long long)va_arg(ap, intmax_t); break;
            case L_T: v = (long long)va_arg(ap, ptrdiff_t); break;
            default: v = va_arg(ap, int); break;
            }
            const bool neg = v < 0;
            const unsigned long long mag = neg ? 0ull - (unsigned long long)v : (unsigned long long)v;
            emit_integer(out, sp, neg, mag, 10, false, true);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            unsigned long long v;
            switch (len) {
            case L_HH: v = (unsigned char)va_arg(ap, unsigned int); break;
            case L_H: v = (unsigned short)va_arg(ap, unsigned int); break;
            case L_L: v = va_arg(ap, unsigned long); break;
            case L_LL: v = va_arg(ap, unsigned long long); break;
            case L_Z: v = (unsigned long long)va_arg(ap, size_t); break;
            case L_J: v = (unsigned long long)va_arg(ap, uintmax_t); break;
            case L_T: v = (unsigned long long)va_arg(ap, ptrdiff_t); break;
            default: v = va_arg(ap, unsigned int); break;
            }
            const unsigned base = conv == 'u' ? 10u : conv == 'o' ? 8u : 16u;
            emit_integer(out, sp, false, v, base, conv == 'X', false);
            break;
        }
        case 'c': {
            const char c = (char)va_arg(ap, int);
            emit_padded(out, sp, std::string(), std::string(1, c), false);
            break;
        }
        case 's': {
            if (len == L_L) {  // wide strings are not supported
                (void)va_arg(ap, const wchar_t*);
                emit_padded(out, sp, std::string(), "?", false);
                break;
            }
            const char* s = va_arg(ap, const char*);
            std::string body;
            if (!s) {
                body = "(null)";
            } else if (sp.prec >= 0) {
                size_t n = 0;
                while (n < (size_t)sp.prec && s[n])
                    ++n;
                body.assign(s, n);
            } else {
                body = s;
            }
            emit_padded(out, sp, std::string(), body, false);
            break;
        }
        case 'p': {
            const void* v = va_arg(ap, const void*);
            std::string body = v ? "0x" + to_base((unsigned long long)(uintptr_t)v, 16, false) : "(nil)";
            emit_padded(out, sp, std::string(), body, false);
            break;
        }
        case 'n':
            (void)va_arg(ap, void*);  // deliberately not supported: writes nothing
            break;
        case 'f':
        case 'F': {
            const double v = len == L_BIG ? (double)va_arg(ap, long double) : va_arg(ap, double);
            emit_fixed(out, sp, v, conv == 'F');
            break;
        }
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            const double v = len == L_BIG ? (double)va_arg(ap, long double) : va_arg(ap, double);
            emit_libc_float(out, sp, conv, v);
            break;
        }
        default:  // unknown conversion: print it as written
            out.append(start, p);
            break;
        }
    }
}

std::string vformat(const char* fmt, va_list ap)
{
    std::string out;
    vformat_append(out, fmt, ap);
    return out;
}

std::string format(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::string out = vformat(fmt, ap);
    va_end(ap);
    return out;
}

// ---------------------------------------------------------------- TextWriter

TextWriter::~TextWriter() { close(nullptr); }

void TextWriter::discard()
{
    close(nullptr);
    path_.clear();
    discard_ = true;
}

bool TextWriter::open(const std::string& path, std::string* err)
{
    close(nullptr);
    discard_ = false;
    path_ = path;
    failed_ = false;
    buf_.clear();
    f_ = platform::open_write(path);
    if (!f_) {
        failed_ = true;
        if (err)
            *err = path + ": cannot create file: " + platform::errno_text();
        return false;
    }
    return true;
}

void TextWriter::write(const char* data, size_t n)
{
    if (discard_)
        return;
    if (!f_) {
        failed_ = true;
        return;
    }
    const char* p = data;
    const char* end = data + n;
    while (p < end) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', (size_t)(end - p)));
        if (!nl) {
            buf_.append(p, end);
            break;
        }
        buf_.append(p, nl);
        buf_ += "\r\n";
        p = nl + 1;
    }
    if (buf_.size() >= (1u << 16))
        flush_buffer();
}

void TextWriter::vprintf(const char* fmt, va_list ap)
{
    if (discard_)
        return;
    std::string s;
    vformat_append(s, fmt, ap);
    write(s);
}

void TextWriter::printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void TextWriter::flush_buffer()
{
    if (f_ && !buf_.empty()) {
        if (std::fwrite(buf_.data(), 1, buf_.size(), f_) != buf_.size())
            failed_ = true;
    }
    buf_.clear();
}

bool TextWriter::close(std::string* err)
{
    bool ok = !failed_;
    if (f_) {
        flush_buffer();
        ok = !failed_;
        if (std::fclose(f_) != 0)
            ok = false;
        f_ = nullptr;
    }
    if (!ok && err)
        *err = path_ + ": write error";
    failed_ = false;
    buf_.clear();
    return ok;
}

} // namespace tpmx
