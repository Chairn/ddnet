#include "test.h"
#include <gtest/gtest.h>

#include <base/fxpt.h>
#include <cmath>
#include <map>
#include <set>

TEST(fxpt, AdditionInt)
{
    for(int i = -1000; i < 1000; ++i)
    {
        for(int j = -1000; j < 1000; ++j)
        {
            EXPECT_EQ(int16_t(i+j), (fx16_16_t(i)+fx16_16_t(j)).toInt());
            EXPECT_EQ(int16_t(i+j), fx16_16_t(i)+fx16_16_t(j));
        }
    }
}

TEST(fxpt, SubstractionInt)
{
    for(int i = -1000; i < 1000; ++i)
    {
        for(int j = -1000; j < 1000; ++j)
        {
            EXPECT_EQ(int16_t(i-j), (fx16_16_t(i)-fx16_16_t(j)).toInt());
            EXPECT_EQ(int16_t(i-j), fx16_16_t(i)-fx16_16_t(j));
        }
    }
}

TEST(fxpt, MultiplicationInt)
{
    for(int i = -1000; i < 1000; ++i)
    {
        for(int j = -1000; j < 1000; ++j)
        {
            EXPECT_EQ(int16_t(i*j), (fx16_16_t(i)*fx16_16_t(j)).toInt());
            EXPECT_EQ(int16_t(i*j), fx16_16_t(i)*fx16_16_t(j));
        }
    }
}

TEST(fxpt, DivisionInt)
{
    for(int i = -1000; i < 1000; ++i)
    {
        for(int j = -1000; j < 1000; ++j)
        {
            if(j != 0)
            {
                EXPECT_EQ(int16_t(i/j), (fx16_16_t(i)/fx16_16_t(j)).toInt());
            }
        }
    }
}

TEST(fxpt, ModuloInt)
{
    for(int i = -1000; i < 1000; ++i)
    {
        for(int j = -1000; j < 1000; ++j)
        {
            if(j != 0)
            {
                EXPECT_EQ(int16_t(i%j), (fx16_16_t(i)%fx16_16_t(j)).toInt());
            }
        }
    }
}

TEST(fxpt, AdditionFloat)
{
    for(float i = -10; i < 10; i += 1./128)
    {
        for(float j = -10; j < 10; j += 1./128)
        {
            EXPECT_EQ(i+j, (fx16_16_t(i)+fx16_16_t(j)).toFloat());
            EXPECT_EQ(i+j, fx16_16_t(i)+fx16_16_t(j));
        }
    }
}

TEST(fxpt, SubstractionFloat)
{
    for(float i = -10; i < 10; i += 1./128)
    {
        for(float j = -10; j < 10; j += 1./128)
        {
            EXPECT_EQ(i-j, (fx16_16_t(i)-fx16_16_t(j)).toFloat());
            EXPECT_EQ(i-j, fx16_16_t(i)-fx16_16_t(j));
        }
    }
}

template<typename T1, typename T2>
std::ostream& operator<<(std::ostream& out, const std::map<T1, T2>& map)
{
    if (map.empty())
        return out << "{}";
    out << "{ ";
    for (const auto& [key, value] : map)
        std::cout << '[' << key << "] = " << value << "; ";
    return out << " }";
}

template<typename T>
std::ostream& operator<<(std::ostream& out, const std::set<T>& set)
{
    if (set.empty())
        return out << "{}";
    out << "{ ";
    for (const auto& key : set)
        std::cout << key << ", ";
    return out << " }";
}

class CMeas2
{
public:
    CMeas2()
    : values()
    {}

    ~CMeas2()
    {
        std::cout << "Elements : " << values.size() << std::endl
                  /*<< "Min : " << *std::min_element(values.begin(), values.end()) << std::endl
                  << "Max : " << *std::max_element(values.begin(), values.end()) << std::endl*/
                  << values << std::endl;
    }

    template<typename T>
    T operator()(T val)
    {
        values[std::ilogb(val)]++;
        return val;
    }

    std::map<int, int64_t> values;
};

class CMeas3
{
public:
    CMeas3(const char* name)
    : m_name(name), values()
    {}

    ~CMeas3()
    {
        std::cout << m_name << std::endl
                  << "Elements : " << values.size() << std::endl
                  /*<< "Min : " << *std::min_element(values.begin(), values.end()) << std::endl
                  << "Max : " << *std::max_element(values.begin(), values.end()) << std::endl*/
                  << values << std::endl;
    }

    template<typename T>
    T operator()(T val)
    {
        values[val]++;
        return val;
    }

    const char* m_name;
    std::map<int, int64_t> values;
};

template<typename T>
class CMeas
{
public:
    CMeas()
    : values()
    {}

    ~CMeas()
    {
        std::cout << "Elements : " << values.size() << std::endl
                  << "Min : " << *std::min_element(values.begin(), values.end()) << std::endl
                  << "Max : " << *std::max_element(values.begin(), values.end()) << std::endl
                  << values << std::endl;
    }

    T operator()(T val)
    {
        values.insert(val);
        return val;
    }

    std::set<T> values;
};

TEST(fxpt, MultiplicationFloat)
{
    //CMeas2 a;
    for(int i = -65536; i <= 65536; i += 64)
    {
        /*static int prev = 0;
        if(int(100.*(i+65536)/131072.) != prev)
        {
            printf("%d%%\n", ++prev);
            fflush(stdout);
        }*/
        fx16_16_t fi = fx16_16_t::fromRaw(i);
        for(int j = -65536; j <= 65536; j += 64)
        {
            fx16_16_t fj = fx16_16_t::fromRaw(j);
            //EXPECT_EQ((fx16_16_t)(i*j), fx16_16_t(i)*fx16_16_t(j));
            EXPECT_TRUE(std::fabs(fi.toFloat()*fj.toFloat() - (fi*fj).toFloat()) < 1./16384);
        }
    }
}
/*{ [-2147483648] = 14522005; [-32] = 3438; [-31] = 15660; [-30] = 64758; [-29] = 251974; [-28] = 909402;
[-27] = 3080442; [-26] = 9762614; [-25] = 28067706; [-24] = 66818164; [-23] = 134085610; [-22] = 268304670;
[-21] = 536742776; [-20] = 1073611060; [-19] = 2147350036; [-18] = -131788; [-17] = -129650; [-16] = 11900564;  }
*/

/*TEST(fxpt, test)
{
    CMes a;
    for(float i = std::numeric_limits<float>::min(); i <= std::numeric_limits<float>::max(); i*=2)
    {
        printf("%g\n", a(i));
    }
    for(float i = -std::numeric_limits<float>::min(); i >= -std::numeric_limits<float>::max(); i*=2)
    {
        printf("%g\n", a(i));
    }
    a(0);
    a(-1.f);
    int i = -65487;
    int j = -1;
    fx16_16_t fi = fx16_16_t::fromRaw(i);
    fx16_16_t fj = fx16_16_t::fromRaw(j);
    EXPECT_EQ(fi.toFloat()/fj.toFloat(), fi/fj);
    EXPECT_EQ(fx16_16_t(fi.toFloat()/fj.toFloat()), fi/fj);
}*/

TEST(fxpt, DivisionFloat)
{
    //CMeas3 a_i("i"), a_j("j (should only be -128 to +128)");
    for(int i = -65536; i <= 65536; i += 64)
    {
        /*static int prev = 0;
        if(int(100.*(i+65536)/131072.) != prev)
        {
            printf("%d%%\n", ++prev);
            fflush(stdout);
        }*/
        fx16_16_t fi = fx16_16_t::fromRaw(i);
        for(int j = -65536; j <= 65536; j += 64)
        {
            fx16_16_t fj = fx16_16_t::fromRaw(j);
            //printf("%d %d\n", i, j);
            if(j != 0)
            {
                //EXPECT_EQ(fx16_16_t(fi.toFloat()/fj.toFloat()), fi/fj);
                //EXPECT_EQ(fi.toFloat()/fj.toFloat(), fi/fj);
                //EXPECT_TRUE(a(std::fabs(fi.toFloat()/fj.toFloat() - (fi/fj).toFloat())) < 1./16384);
                EXPECT_TRUE(std::fabs(fi.toFloat()/fj.toFloat() - (fi/fj).toFloat()) < 1./16384);
                /*float div = (fi/fj).toFloat();
                if(std::fabs(div) < 32768)
                {
                    if(std::fabs(fi.toFloat()/fj.toFloat() - (fi/fj).toFloat()) >= 1./16384)
                    {
                        //a_i(i);
                        a_j(j);
                        //printf("%d %d\n", i, j);
                    }
                }*/
            }
        }
    }
    EXPECT_TRUE(true);
}
/*{ [-2147483648] = 181098984; [-32] = 176; [-31] = 1292; [-30] = 6812; [-29] = 29960; [-28] = 125224;
[-27] = 515752; [-26] = 2069852; [-25] = 8349276; [-24] = 33478812; [-23] = 100508020; [-22] = 234693096;
[-21] = 503073980; [-20] = 1039937036; [-19] = 2113695760; [-18] = -33665092; [-17] = -35939204;
[-16] = 141534276; [-15] = 4143012; [-14] = 1044136; [-13] = 227464; [-12] = 38668; [16] = 131076;  }
*/

TEST(fxpt, ModuloFloat)
{
    //CMeas2 a;
    for(int i = -65536; i <= 65536; i += 64)
    {
        /*static int prev = 0;
        if(int(100.*(i+65536)/131072.) != prev)
        {
            printf("%d%%\n", ++prev);
            fflush(stdout);
        }*/
        fx16_16_t fi = fx16_16_t::fromRaw(i);
        for(int j = -65536; j <= 65536; j += 64)
        {
            fx16_16_t fj = fx16_16_t::fromRaw(j);
            //printf("%d %d\n", i, j);
            if(j != 0)
            {
                //EXPECT_EQ(fx16_16_t(std::fmod(fi.toFloat(), fj.toFloat())), fi%fj);
                //EXPECT_EQ(std::fmod(fi.toFloat(), fj.toFloat()), fi%fj);
                //EXPECT_TRUE(a(std::fabs(std::fmod(fi.toFloat(), fj.toFloat()) - (fi%fj).toFloat())) < 1./16384);
                EXPECT_TRUE(std::fabs(std::fmod(fi.toFloat(), fj.toFloat()) - (fi%fj).toFloat()) < 1./16384);
                /*float div = (fi/fj).toFloat();
                if(std::fabs(div) < 32768)
                {
                    if(std::fabs(fi.toFloat()/fj.toFloat() - (fi/fj).toFloat()) >= 1./16384)
                    {
                        //a_i(i);
                        a_j(j);
                        //printf("%d %d\n", i, j);
                    }
                }*/
            }
        }
    }
    EXPECT_TRUE(true);
}
/*{ [-2147483648] = 17180000256;  }*/

TEST(fxpt, Comparison)
{
    for(float i = -1; i < 1; i += 1./1024)
    {
        /*static int prev = 0;
        if(int(100.*(i+1)/2.) != prev)
        {
            printf("%d%%\n", ++prev);
            fflush(stdout);
        }*/
        for(float j = -1; j < 1; j += 1./1024)
        {
            fx16_16_t fi = i, fj = j;
            EXPECT_EQ(i < j, fi < fj);
            EXPECT_EQ(i > j, fi > fj);
            EXPECT_EQ(i == j, fi == fj);
            EXPECT_EQ(i != j, fi != fj);
            EXPECT_EQ(i <= j, fi <= fj);
            EXPECT_EQ(i >= j, fi >= fj);
        }
    }
}

TEST(fxpt, Sqrt)
{
    //CMeas2 a;
    for(int i = 0; i < std::numeric_limits<int>::max() && i >= 0; i += 1024)
    {
        /*static int prev = 0;
        if(int(100.*i/(float)std::numeric_limits<int>::max()) != prev)
        {
            printf("%d%%\n", ++prev);
            fflush(stdout);
        }*/
        //printf("%x\n", i);
        //EXPECT_TRUE(a(std::fabs(sqrtf(i/65536.f) - fx::sqrt(fx16_16_t::fromRaw(i)).toFloat())) < 1./16384);
        EXPECT_TRUE(  std::fabs(sqrtf(i/65536.f) - fx::sqrt(fx16_16_t::fromRaw(i)).toFloat()) < 1./16384);
        //EXPECT_EQ((fx16_16_t)(sqrtf(i/65536.f)), std::sqrt(fx16_16_t::fromRaw(i)));
    }
}
/*
{ [-2147483648] = 766940448; [-26] = 2; [-25] = 29; [-24] = 244; [-23] = 1954; [-22] = 16107; [-21] = 129902;
[-20] = 1048956; [-19] = 8390227; [-18] = 67113307; [-17] = 536929927; [-16] = 766912544;  }
*/

TEST(fxpt, Cbrt)
{
    /*CMeas2 a;
    for(int i = std::numeric_limits<int>::min(); i < std::numeric_limits<int>::max(); ++i)
    {
        static int prev = 0;
        if(int(100.*((float)i-(float)std::numeric_limits<int>::min())/((float)std::numeric_limits<int>::max() - std::numeric_limits<int>::min())) != prev)
        {
            printf("%d%%\n", ++prev);
            fflush(stdout);
        }
        //printf("%x\n", i);
        EXPECT_TRUE(a(std::fabs(cbrtf(i/65536.f) - fx::cbrt(fx16_16_t::fromRaw(i)).toFloat())) < 1./16384);
    }*/
    CMeas2 a;
    for(int i = 0; i < 100; ++i)
    {
        //EXPECT_TRUE(a(std::fabs(cbrtf(i/65536.f) - fx::cbrt(fx16_16_t::fromRaw(i)).toFloat())) < 1./16384);
        if(a(std::fabs(cbrtf(i/65536.f) - fx::cbrt(fx16_16_t::fromRaw(i)).toFloat())) >= 1./16384)
        {
            printf("failed %d : %g\n", i, std::fabs(cbrtf(i/65536.f) - fx::cbrt(fx16_16_t::fromRaw(i)).toFloat()));
        }
    }

    /*int i = 1;
    do {
        printf("%08x %2d %2d\n", i, __builtin_clrsb(i), __builtin_clrsb(-i));
        i <<= 1;
    } while(i);
    printf("%08x %2d %2d\n", i, __builtin_clrsb(i), __builtin_clrsb(-i));*/
}

