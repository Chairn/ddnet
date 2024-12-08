#ifndef BASE_FXPT_H
#define BASE_FXPT_H

#include <cstdint>
#include <limits>

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
    constexpr fxpt<E, F>(int i)
    : m_fixed(i << F)
    {}

    fxpt<E,F>()
    {}

    constexpr static fxpt<E, F> fromRaw(int i)
    {
        fxpt<E, F> res;
        res.m_fixed = i ;
        return res;
    }

    constexpr float toFloat() const
    {
        return m_fixed/float(1 << F);
    }
    constexpr int toInt() const
    {
        return m_fixed >> F;
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
        int64_t buf = (int64_t)m_fixed * rhs.m_fixed;
        buf >>= F;
        m_fixed = buf;
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
        int64_t buf = ((int64_t)m_fixed << F) / rhs.m_fixed;
        m_fixed = buf;
        return *this;
    }
    template<typename T>
    constexpr fxpt<E, F>& operator/=(const T& rhs)
    {
        int64_t buf = ((int64_t)m_fixed << F) / fxpt<E, F>(rhs).m_fixed;
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
    
protected:
    void Scale();
    void Unscale();

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
/*}

namespace ns
{*/
    // those overloads are forbidden in std::...
    template<int E, int F> 
    constexpr fxpt<E, F> sqrt(fxpt<E, F> f)
    {
        // taken from https://github.com/chmike/fpsqrt/blob/df099181030e95d663d89e87d4bf2d36534776a5/fpsqrt.c#L113
        // other implementation available in https://stackoverflow.com/a/30962495
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
    constexpr fxpt<E, F> exp(fxpt<E, F> num)
    {

    }

    template<int E, int F>
    constexpr fxpt<E, F> pow(fxpt<E, F> base, fxpt<E, F> exp)
    {

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
    constexpr fxpt<E, F> atan(fxpt<E, F> num)
    {

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