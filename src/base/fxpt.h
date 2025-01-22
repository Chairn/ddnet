#ifndef BASE_FXPT_H
#define BASE_FXPT_H

#include <cstdint>
#include <limits>
#include <cmath>

template<int E, int F>
class fxpt
{
    static_assert(E >= 0);
    static_assert(F >= 0);
    static_assert(E+F > 0);
public:
    constexpr fxpt<E, F>(float f)
    : m_fixed(f*(1 << F))
    {}
    constexpr fxpt<E, F>(double d)
    : m_fixed(d*(1 << F))
    {}
    constexpr fxpt<E, F>(int i)
    : m_fixed(i << F)
    {}

    constexpr fxpt<E,F>()
    : m_fixed()
    {}

    constexpr static fxpt<E, F> fromRaw(int i)
    {
        fxpt<E, F> res;
        res.m_fixed = i;
        return res;
    }

    constexpr float toFloat() const
    {
        //float res = m_fixed/float(1 << F);
        return m_fixed/float(1 << F);
    }
    constexpr int toInt() const
    {   // division rather than shift for correct rounding towards 0 of negative numbers
        return m_fixed/(1 << F);
    }

    constexpr fxpt<E, F>& operator+=(const fxpt<E, F>& rhs)
    {
        m_fixed += rhs.m_fixed;
        return *this;
    }
    template<typename T>
    constexpr fxpt<E, F>& operator+=(const T& rhs)
    {
        m_fixed += fxpt<E, F>(rhs).m_fixed;
        return *this;
    }
    constexpr fxpt<E, F>& operator-=(const fxpt<E, F>& rhs)
    {
        m_fixed -= rhs.m_fixed;
        return *this;
    }
    template<typename T>
    constexpr fxpt<E, F>& operator-=(const T& rhs)
    {
        m_fixed -= fxpt<E, F>(rhs).m_fixed;
        return *this;
    }
    constexpr fxpt<E, F> operator-() const
    {
        fxpt<E, F> a = *this;
        a.m_fixed = -a.m_fixed;
        return a;
    }
    constexpr fxpt<E, F>& operator*=(const fxpt<E, F>& rhs)
    {
        //printf("%d ", m_fixed);
        int64_t buf = (int64_t)m_fixed * rhs.m_fixed;
        m_fixed = buf >> F;
        //printf("%d %llx %d %f\n", rhs.m_fixed, buf, m_fixed, m_fixed/65536.);
        //m_fixed = buf/(1 << F);
        /*if(buf & ((1 << (F-1)) - 1))
            m_fixed++;*/
        //m_fixed += (buf & (1 << (F-1))) >> (F-1);
        return *this;
    }
    template<typename T>
    constexpr fxpt<E, F>& operator*=(const T& rhs)
    {
        int64_t buf = (int64_t)m_fixed * fxpt<E, F>(rhs).m_fixed;
        buf >>= F;
        m_fixed = buf;
        return *this;
    }
    constexpr fxpt<E, F>& operator/=(const fxpt<E, F>& rhs)
    {
        //printf("%d ", m_fixed);
        int64_t buf = ((int64_t)m_fixed << F) / rhs.m_fixed;
        m_fixed = buf;
        //printf("%d %llx %d %f\n", rhs.m_fixed, buf, m_fixed, m_fixed/65536.);
        return *this;
    }
    template<typename T>
    constexpr fxpt<E, F>& operator/=(const T& rhs)
    {
        int64_t buf = ((int64_t)m_fixed << F) / fxpt<E, F>(rhs).m_fixed;
        m_fixed = buf;
        return *this;
    }
    constexpr fxpt<E, F>& operator%=(const fxpt<E, F>& rhs)
    {
        int64_t buf = (int64_t)(m_fixed % rhs.m_fixed);
        //printf("%d %d %llx ", m_fixed, rhs.m_fixed, buf);
        m_fixed = buf;// >> F;
        //printf("%d %g\n", m_fixed, m_fixed/65536.f);
        return *this;
    }
    template<typename T>
    constexpr fxpt<E, F>& operator%=(const T& rhs)
    {
        int64_t buf = ((int64_t)m_fixed) % fxpt<E, F>(rhs).m_fixed;
        m_fixed = buf;
        return *this;
    }

    constexpr friend bool operator==(const fxpt<E, F>& lhs, const fxpt<E, F>& rhs)
    {
        return lhs.m_fixed == rhs.m_fixed;
    }
    template<typename T>
    constexpr friend bool operator==(const fxpt<E, F>& lhs, const T& rhs)
    {
        return lhs.m_fixed == fxpt<E, F>(rhs).m_fixed;
    }
    constexpr friend bool operator!=(const fxpt<E, F>& lhs, const fxpt<E, F>& rhs)
    {
        return lhs.m_fixed != rhs.m_fixed;
    }
    template<typename T>
    constexpr friend bool operator!=(const fxpt<E, F>& lhs, const T& rhs)
    {
        return lhs.m_fixed != fxpt<E, F>(rhs).m_fixed;
    }
    constexpr friend bool operator<=(const fxpt<E, F>& lhs, const fxpt<E, F>& rhs)
    {
        return lhs.m_fixed <= rhs.m_fixed;
    }
    template<typename T>
    constexpr friend bool operator<=(const fxpt<E, F>& lhs, const T& rhs)
    {
        return lhs.m_fixed <= fxpt<E, F>(rhs).m_fixed;
    }
    constexpr friend bool operator<(const fxpt<E, F>& lhs, const fxpt<E, F>& rhs)
    {
        return lhs.m_fixed < rhs.m_fixed;
    }
    template<typename T>
    constexpr friend bool operator<(const fxpt<E, F>& lhs, const T& rhs)
    {
        return lhs.m_fixed < fxpt<E, F>(rhs).m_fixed;
    }
    constexpr friend bool operator>=(const fxpt<E, F>& lhs, const fxpt<E, F>& rhs)
    {
        return lhs.m_fixed >= rhs.m_fixed;
    }
    template<typename T>
    constexpr friend bool operator>=(const fxpt<E, F>& lhs, const T& rhs)
    {
        return lhs.m_fixed >= fxpt<E, F>(rhs).m_fixed;
    }
    constexpr friend bool operator>(const fxpt<E, F>& lhs, const fxpt<E, F>& rhs)
    {
        return lhs.m_fixed > rhs.m_fixed;
    }
    template<typename T>
    constexpr friend bool operator>(const fxpt<E, F>& lhs, const T& rhs)
    {
        return lhs.m_fixed > fxpt<E, F>(rhs).m_fixed;
    }

    int Raw() const
    {
        return m_fixed;
    }

private:
    int m_fixed : E+F;
};

template<int E, int F>
constexpr inline fxpt<E, F> operator+(const fxpt<E, F>& lhs, const fxpt<E, F>& rhs)
{
    fxpt<E, F> result = lhs;
    result += rhs;
    return result;
}
template<typename T, int E, int F>
constexpr inline fxpt<E, F> operator+(const fxpt<E, F>& lhs, const T& rhs)
{
    fxpt<E, F> result = lhs;
    result += rhs;
    return result;
}
template<typename T, int E, int F>
constexpr inline fxpt<E, F> operator+(const T& lhs, const fxpt<E, F>& rhs)
{
    fxpt<E, F> result = lhs;
    result += rhs;
    return result;
}
template<int E, int F>
constexpr inline fxpt<E, F> operator-(const fxpt<E, F>& lhs, const fxpt<E, F>& rhs)
{
    fxpt<E, F> result = lhs;
    result -= rhs;
    return result;
}
template<typename T, int E, int F>
constexpr inline fxpt<E, F> operator-(const fxpt<E, F>& lhs, const T& rhs)
{
    fxpt<E, F> result = lhs;
    result -= rhs;
    return result;
}
template<typename T, int E, int F>
constexpr inline fxpt<E, F> operator-(const T& lhs, const fxpt<E, F>& rhs)
{
    fxpt<E, F> result = lhs;
    result -= rhs;
    return result;
}
template<int E, int F>
constexpr inline fxpt<E, F> operator*(const fxpt<E, F>& lhs, const fxpt<E, F>& rhs)
{
    fxpt<E, F> result = lhs;
    result *= rhs;
    return result;
}
template<typename T, int E, int F>
constexpr inline fxpt<E, F> operator*(const fxpt<E, F>& lhs, const T& rhs)
{
    fxpt<E, F> result = lhs;
    result *= rhs;
    return result;
}
template<typename T, int E, int F>
constexpr inline fxpt<E, F> operator*(const T& lhs, const fxpt<E, F>& rhs)
{
    fxpt<E, F> result = lhs;
    result *= rhs;
    return result;
}
template<int E, int F>
constexpr inline fxpt<E, F> operator/(const fxpt<E, F>& lhs, const fxpt<E, F>& rhs)
{
    fxpt<E, F> result = lhs;
    result /= rhs;
    return result;
}
template<typename T, int E, int F>
constexpr inline fxpt<E, F> operator/(const fxpt<E, F>& lhs, const T& rhs)
{
    fxpt<E, F> result = lhs;
    result /= rhs;
    return result;
}
template<typename T, int E, int F>
constexpr inline fxpt<E, F> operator/(const T& lhs, const fxpt<E, F>& rhs)
{
    fxpt<E, F> result = lhs;
    result /= rhs;
    return result;
}
template<int E, int F>
constexpr inline fxpt<E, F> operator%(const fxpt<E, F>& lhs, const fxpt<E, F>& rhs)
{
    fxpt<E, F> result = lhs;
    result %= rhs;
    return result;
}
template<typename T, int E, int F>
constexpr inline fxpt<E, F> operator%(const fxpt<E, F>& lhs, const T& rhs)
{
    fxpt<E, F> result = lhs;
    result %= rhs;
    return result;
}
template<typename T, int E, int F>
constexpr inline fxpt<E, F> operator%(const T& lhs, const fxpt<E, F>& rhs)
{
    fxpt<E, F> result = lhs;
    result %= rhs;
    return result;
}

template<int E, int F>
std::ostream& operator<<(std::ostream& os, const fxpt<E, F>& obj)
{
    os << std::hex << obj.Raw() << std::dec << " " << obj.toFloat();
    return os;
}

namespace std
{
    // explicitly allowed, based on https://stackoverflow.com/a/16519653
    template<int E, int F> class numeric_limits<fxpt<E, F>>
    {
    public:
        constexpr static fxpt<E, F> lowest() {return fxpt<E, F>::fromRaw(1);}
        constexpr static fxpt<E, F> max() {return fxpt<E, F>::fromRaw(std::numeric_limits<int>::max());}
        constexpr static fxpt<E, F> min() {return fxpt<E, F>::fromRaw(std::numeric_limits<int>::min());}
    };

    template<int E, int F> fxpt<E, F> nextafter(fxpt<E, F> from, fxpt<E, F> to)
    {
        if(to == from)
            return to;
        else if(to > from)
            return fxpt<E, F>::fromRaw(from.Raw()+1);
        else // to < from
            return fxpt<E, F>::fromRaw(from.Raw()-1);
    }
}

namespace fx
{
    // those overloads are forbidden in std::...
    template<int E, int F>
    constexpr fxpt<E, F> sqrt(fxpt<E, F> f)
    {
        // taken from https://github.com/chmike/fpsqrt/blob/df099181030e95d663d89e87d4bf2d36534776a5/fpsqrt.c#L113
        // other implementation available in https://stackoverflow.com/a/30962495
        // https://en.wikipedia.org/wiki/Methods_of_computing_square_roots (looks to be the same)
        uint32_t t, q, b, r;
        r = (int32_t)f.Raw();
        q = 0;
        b = 0x40000000UL;
        if( r < 0x40000200 )
        {
            while( b != 0x40 )
            {
                t = q + b;
                if( r >= t )
                {
                    r -= t;
                    q = t + b; // equivalent to q += 2*b
                }
                r <<= 1;
                b >>= 1;
            }
            q >>= 8;
            return fxpt<E, F>::fromRaw(q);
        }
        while( b > 0x40 )
        {
            t = q + b;
            if( r >= t )
            {
                r -= t;
                q = t + b; // equivalent to q += 2*b
            }
            if( (r & 0x80000000) != 0 )
            {
                q >>= 1;
                b >>= 1;
                r >>= 1;
                while( b > 0x20 )
                {
                    t = q + b;
                    if( r >= t )
                    {
                        r -= t;
                        q = t + b;
                    }
                    r <<= 1;
                    b >>= 1;
                }
                q >>= 7;
                return fxpt<E, F>::fromRaw(q);
            }
            r <<= 1;
            b >>= 1;
        }
        q >>= 8;
        return fxpt<E, F>::fromRaw(q);
    }

    template<int E, int F>
    constexpr fxpt<E, F> cbrt(fxpt<E, F> num)
    {
        /* Halley's Method: (a is num), x0 should be close to cbrt of num
        x_(n+1) = x_n (x_n^3+2a)/(2x_n^3+a)
        */
        if(num == 0)
            return 0;
        
        constexpr fxpt<E,F> x0_table[] = {
            1./32, 1./32, 1./16, 1./16, 1./16, 1./8, 1./8, 1./8,
            1./4, 1./4, 1./4, 1./2, 1./2, 1./2, 1, 1, 1,
            2, 2, 2, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32
        };

        fxpt<E, F> xn;
        if(num > 0)
            xn = x0_table[30-__builtin_clrsb(num.Raw())];
        else
            xn = -x0_table[31-__builtin_clrsb(num.Raw())];
        fxpt<E, F> numtwice = 2*num;
        std::cout << num << " " << __builtin_clrsb(num.Raw()) << " " << xn;
        for(int i = 0; i < 6; ++i)
        {
            fxpt<E, F> xn3 = xn*xn*xn;
            xn = xn * (xn3+numtwice)/(2*xn3+num);
            printf("%d %g\n", i, xn.toFloat());
        }
        printf("%g\n", std::cbrt(num.toFloat()));
        return xn;
    }

    template<int E, int F>
    constexpr fxpt<E, F> exp(fxpt<E, F> num)
    {
        if(num.Raw() > 681389) // overflow, (exp(10.397) = 32767)
        {
            return std::numeric_limits<fxpt<E, F>>::max();
        }
        else if(num.Raw() < -726817) // unferflow, (exp(-11.09) = 1/65536)
        {
            return 0;
        }
        fxpt<E, F> res = 1+num;
        fxpt<E, F> prod = num*num;
        fxpt<E, F> fact = 2;
        for(int i = 2; i < 8; )
        {
            res += prod/fact;
            i++;
            fact *= i;
            prod *= num;
        }

        return res;
    }

    template<int E, int F>
    constexpr fxpt<E, F> pow(fxpt<E, F> base, fxpt<E, F> num)
    {
        return exp(num*log(base));
    }

    // log2 function taken from https://github.com/dmoulding/log2fix/blob/master/log2fix.c
    // found in https://stackoverflow.com/a/14884853
    // other implementation available in https://stackoverflow.com/a/30962495
    // not tested yet
    template<int E, int F>
    constexpr fxpt<E, F> log2(fxpt<E, F> num)
    {
        // This implementation is based on Clay. S. Turner's fast binary logarithm
        // algorithm[1].

        int32_t b = 1U << (F - 1);
        int32_t y = 0;
        int32_t x = num.Raw();

        if (x == 0) {
            return std::numeric_limits<fxpt<E, F>>::min(); // represents negative infinity
        }

        while (x < 1U << F) {
            x <<= 1;
            y -= 1U << F;
        }

        while (x >= 2U << F) {
            x >>= 1;
            y += 1U << F;
        }

        uint64_t z = x;

        for (size_t i = 0; i < F; i++) {
            z = z * z >> F;
            if (z >= 2U << F) {
                z >>= 1;
                y += b;
            }
            b >>= 1;
        }

        return fxpt<E, F>::fromRaw(y);
    }

    template<int E, int F>
    constexpr fxpt<E, F> log(fxpt<E, F> x)
    {
        uint64_t t;

        t = log2(x) * fxpt<E, F>(1./log2(fxpt<E, F>(2.718281828)));

        return fxpt<E, F>::fromRaw(t >> 31);
    }

    template<int E, int F>
    constexpr fxpt<E, F> log10(fxpt<E, F> x)
    {
        uint64_t t;

        t = log2(x) * fxpt<E, F>(1./log2(fxpt<E, F>(10)));

        return fxpt<E, F>::fromRaw(t >> 31);
    }

    template<int E, int F>
    constexpr fxpt<E, F> acos(fxpt<E, F> num)
    {
        return 0;
    }

    template<int E, int F>
    constexpr fxpt<E, F> asin(fxpt<E, F> num)
    {
        return 0;
    }

    template<int E, int F>
    constexpr fxpt<E, F> atan(fxpt<E, F> num)
    {
        return 0;
    }

    template<int E, int F>
    constexpr fxpt<E, F> cos(fxpt<E, F> num)
    {
        // https://en.wikipedia.org/wiki/CORDIC#Software_Example_(Python)
        /*from math import atan2, sqrt, sin, cos, radians

ITERS = 16
theta_table = [atan2(1, 2**i) for i in range(ITERS)]

def compute_K(n):
    """
    Compute K(n) for n = ITERS. This could also be
    stored as an explicit constant if ITERS above is fixed.
    """
    k = 1.0
    for i in range(n):
        k *= 1 / sqrt(1 + 2 ** (-2 * i))
    return k

def CORDIC(alpha, n):
    K_n = compute_K(n)
    theta = 0.0
    x = 1.0
    y = 0.0
    P2i = 1  # This will be 2**(-i) in the loop below
    for arc_tangent in theta_table:
        sigma = +1 if theta < alpha else -1
        theta += sigma * arc_tangent
        x, y = x - sigma * y * P2i, sigma * P2i * x + y
        P2i /= 2
    return x * K_n, y * K_n

if __name__ == "__main__":
# Print a table of computed sines and cosines, from -90° to +90°, in steps of 15°,
# comparing against the available math routines.
print("  x       sin(x)     diff. sine     cos(x)    diff. cosine ")
for x in range(-90, 91, 15):
    cos_x, sin_x = CORDIC(radians(x), ITERS)
    print(
        f"{x:+05.1f}°  {cos_x:+.8f} ({cos_x-cos(radians(x)):+.8f})  {sin_x:+.8f} ({sin_x-sin(radians(x)):+.8f})"
    )*/
        // cos(x) = somme (-1)^n*x^2n/(2n)!

        return 0;
    }

    template<int E, int F>
    constexpr fxpt<E, F> sin(fxpt<E, F> num)
    {
        return 0;
    }

    template<int E, int F>
    constexpr fxpt<E, F> tan(fxpt<E, F> num)
    {
        return 0;
    }
}

typedef fxpt<16, 16> fx16_16_t;

void test()
{
    fxpt<16,16> a, b, c;

    c += b;
    c = a+b;
    c = a*b;
    c *= a;
    a = b/c;
    c /= a;
}

#endif // BASE_FXPT_H
