#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Created on Fri Jan 17 11:36:03 2025

@author: egloffv
"""

import math
from time import process_time_ns
from multiprocessing import Pool
from struct import pack, unpack
from collections import Counter
maxi = Counter()

class fxpt:
    def __init__(self, value = 0, frac = 16):
        self.m_frac = int(frac)
        if isinstance(value, fxpt):
            if value.m_frac < frac:
                self.m_fixed = value.m_fixed << (self.m_frac-value.m_frac)
            else:
                self.m_fixed = value.m_fixed >> (value.m_frac-self.m_frac)
        else:
            self.m_fixed = int(value*2**self.m_frac)
    def __repr__(self) -> str:
        return "{:x} {:.8f}".format(self.m_fixed, self.m_fixed/2**self.m_frac)
    def __add__(self, other):
        if isinstance(other, fxpt):
            if other.m_frac < self.m_frac:
                return fxpt.fromRaw(self.m_fixed + (other.m_fixed << (self.m_frac-other.m_frac)), self.m_frac)
            else:
                return fxpt.fromRaw((self.m_fixed << (other.m_frac-self.m_frac)) + other.m_fixed, other.m_frac)
        return fxpt.fromRaw(self.m_fixed + fxpt(other, self.m_frac).m_fixed, self.m_frac)
    def __radd__(self, other):
        return self.__add__(other)
    def __sub__(self, other):
        if isinstance(other, fxpt):
            if other.m_frac < self.m_frac:
                return fxpt.fromRaw(self.m_fixed - (other.m_fixed << (self.m_frac-other.m_frac)), self.m_frac)
            else:
                return fxpt.fromRaw((self.m_fixed << (other.m_frac-self.m_frac)) - other.m_fixed, other.m_frac)
        return fxpt.fromRaw(self.m_fixed - fxpt(other, self.m_frac).m_fixed, self.m_frac)
    def __rsub__(self, other):
        return fxpt(other, self.m_frac).__sub__(self)
    def __mul__(self, other):
        if isinstance(other, fxpt):
            if other.m_frac < self.m_frac:
                return fxpt.fromRaw((self.m_fixed * (other.m_fixed << (self.m_frac-other.m_frac))) >> self.m_frac, self.m_frac)
            else:
                return fxpt.fromRaw(((self.m_fixed << (other.m_frac-self.m_frac)) * other.m_fixed) >> other.m_frac, other.m_frac)
        return fxpt.fromRaw((self.m_fixed * fxpt(other, self.m_frac).m_fixed) >> self.m_frac, self.m_frac)
    def __rmul__(self, other):
        return self.__mul__(other)
    def __truediv__(self, other):
        if isinstance(other, fxpt):
            if other.m_frac < self.m_frac:
                return fxpt.fromRaw((self.m_fixed << self.m_frac) / (other.m_fixed << (self.m_frac-other.m_frac)), self.m_frac)
            else:
                return fxpt.fromRaw((self.m_fixed << (self.m_frac+other.m_frac)) / other.m_fixed, other.m_frac)
        return fxpt.fromRaw((self.m_fixed << self.m_frac) / fxpt(other, self.m_frac).m_fixed, self.m_frac)
    def __rtruediv__(self, other):
        return fxpt(other, self.m_frac).__truediv__(self)
    def __floordiv__(self, other):
        if isinstance(other, fxpt):
            if other.m_frac < self.m_frac:
                return fxpt.fromRaw((self.m_fixed // (other.m_fixed << (self.m_frac-other.m_frac))) << self.m_frac, self.m_frac)
            else:
                return fxpt.fromRaw(((self.m_fixed << (other.m_frac-self.m_frac)) // other.m_fixed) << other.m_frac, other.m_frac)
        return fxpt(self.m_fixed // fxpt(other, self.m_frac).m_fixed, self.m_frac)
    def __rfloordiv__(self, other):
        return fxpt(other, self.m_frac).__floordiv__(self)
    def __mod__(self, other):
        if isinstance(other, fxpt):
            if other.m_frac < self.m_frac:
                return fxpt.fromRaw(self.m_fixed % (other.m_fixed << (self.m_frac-other.m_frac)), self.m_frac)
            else:
                return fxpt.fromRaw((self.m_fixed << (other.m_frac-self.m_frac)) % other.m_fixed, other.m_frac)
        return fxpt.fromRaw(self.m_fixed % other.m_fixed, self.m_frac)
    def __rmod__(self, other):
        return fxpt(other, self.m_frac).__mod__(self)
    def __neg__(self):
        return fxpt.fromRaw(-self.m_fixed, self.m_frac)
    def __pos__(self):
        return fxpt(self)
    def __abs__(self):
        return fxpt.fromRaw(abs(self.m_fixed), self.m_frac)
    def __round__(self, ndigits = None):
        return round(self.m_fixed/2**self.m_frac, ndigits)
    def __trunc__(self):
        return int(self.m_fixed/2**self.m_frac)
    def __floor__(self):
        return math.floor(self.m_fixed/2**self.m_frac)
    def __ceil__(self):
        return math.ceil(self.m_fixed/2**self.m_frac)
    def __lt__(self, other) -> bool:
        if isinstance(other, fxpt):
            ## scale to the most precise one
            if self.m_frac >= other.m_frac:
                return self.m_fixed < (other.m_fixed << (self.m_frac-other.m_frac))
            else:
                return (self.m_fixed << (other.m_frac-self.m_frac)) < other.m_fixed
        return self.m_fixed < fxpt(other, self.m_frac).m_fixed
    def __le__(self, other) -> bool:
        if isinstance(other, fxpt):
            ## scale to the most precise one
            if self.m_frac >= other.m_frac:
                return self.m_fixed <= (other.m_fixed << (self.m_frac-other.m_frac))
            else:
                return (self.m_fixed << (other.m_frac-self.m_frac)) <= other.m_fixed
        return self.m_fixed <= fxpt(other, self.m_frac).m_fixed
    def __eq__(self, other) -> bool:
        if isinstance(other, fxpt):
            ## scale to the most precise one
            if self.m_frac >= other.m_frac:
                return self.m_fixed == (other.m_fixed << (self.m_frac-other.m_frac))
            else:
                return (self.m_fixed << (other.m_frac-self.m_frac)) == other.m_fixed
        return self.m_fixed == fxpt(other, self.m_frac).m_fixed
    def __ne__(self, other) -> bool:
        if isinstance(other, fxpt):
            ## scale to the most precise one
            if self.m_frac >= other.m_frac:
                return self.m_fixed != (other.m_fixed << (self.m_frac-other.m_frac))
            else:
                return (self.m_fixed << (other.m_frac-self.m_frac)) != other.m_fixed
        return self.m_fixed != fxpt(other, self.m_frac).m_fixed
    def __gt__(self, other) -> bool:
        if isinstance(other, fxpt):
            ## scale to the most precise one
            if self.m_frac >= other.m_frac:
                return self.m_fixed > (other.m_fixed << (self.m_frac-other.m_frac))
            else:
                return (self.m_fixed << (other.m_frac-self.m_frac)) > other.m_fixed
        return self.m_fixed > fxpt(other, self.m_frac).m_fixed
    def __ge__(self, other) -> bool:
        if isinstance(other, fxpt):
            ## scale to the most precise one
            if self.m_frac >= other.m_frac:
                return self.m_fixed >= (other.m_fixed << (self.m_frac-other.m_frac))
            else:
                return (self.m_fixed << (other.m_frac-self.m_frac)) >= other.m_fixed
        return self.m_fixed >= fxpt(other, self.m_frac).m_fixed
    def __hash__(self):
        return hash(self.m_fixed)
    def __bool__(self):
        return bool(self.m_fixed)
    def fromRaw(value: int, frac = 16):
        res = fxpt(0, frac)
        res.m_fixed = int(value)
        return res
    def toInt(self) -> int:
        return self.m_fixed >> self.m_frac ## round towards -Inf
    def toFloat(self) -> float:
        return self.m_fixed / 2**self.m_frac ## round towards 0
    
    def sqrt(self):
        if self.m_fixed < 0:
            raise ValueError("math domain error")
        t = 0
        q = 0
        b = 0x40000000
        r = self.m_fixed
        if r < 0x40000200:
            while b != 0x40:
                t = q + b;
                if r >= t:
                    r -= t;
                    q = t + b ## equivalent to q += 2*b
                r <<= 1
                b >>= 1
            q >>= 8
            return fxpt.fromRaw(q)
        while b > 0x40:
            t = q + b
            if r >= t:
                r -= t
                q = t + b ## equivalent to q += 2*b
            if (r & 0x80000000) != 0:
                q >>= 1
                b >>= 1
                r >>= 1
                while b > 0x20:
                    t = q + b
                    if r >= t:
                        r -= t
                        q = t + b
                    r <<= 1
                    b >>= 1
                q >>= 7
                return fxpt.fromRaw(q)
            r <<= 1
            b >>= 1
        q >>= 8
        return fxpt.fromRaw(q)
    def cbrt(self):
        # global maxi
        if self.m_fixed == 0:
            return fxpt(0)
        x0_table = [
            float('nan'),
            1./32, 1./32, 1./16, 1./16, 1./16, 1./8, 1./8, 1./8,
            1./4, 1./4, 1./4, 1./2, 1./2, 1./2, 1, 1, 1,
            2, 2, 2, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32
        ]
        xn = fxpt(x0_table[self.m_fixed.bit_length()], 24)
        f = abs(fxpt(self, 24))
        # print(f, xn)
        for i in range(3): ## range(2) has more than 1ulp error
            xn3 = xn*xn*xn
            # newxn = xn*((xn3+2*f)/(2*xn3+f))
            # if newxn == xn:
            #     maxi[i] += 1
            #     break
            xn = xn*((xn3+2*f)/(2*xn3+f)) ## division first before multiplication to avoid underflow into 0 for small values
        
        if self.m_fixed < 0:
            return fxpt(-xn, 16)
        return fxpt(xn, 16)
    
    def frexp(self):
        if self.m_fixed == 0:
            return fxpt(0, 32), 0
        e = self.m_fixed.bit_length()-self.m_frac
        m = self.m_fixed << (32-self.m_frac-e)
        # if e >= 0:
        #     m = m >> e
        # else:
        #     m = m << -e
        return fxpt.fromRaw(m, 32), e
    
    def log2(self):
        f = fxpt(self, 24)
        b = 1 << (f.m_frac - 1)
        y = 0
        x = f.m_fixed

        if x <= 0:
            raise ValueError("math domain error")

        while x < (1 << f.m_frac):
            x <<= 1
            y -= 1 << f.m_frac

        while x >= (2 << f.m_frac):
            x >>= 1
            y += 1 << f.m_frac

        z = x

        for i in range(f.m_frac):
            z = (z * z) >> f.m_frac
            if z >= (2 << f.m_frac):
                z >>= 1
                y += b
            b >>= 1
            # print("{:016x} {:08x} {:08x}".format(z, y, b))

        return fxpt.fromRaw(y, 24)
    def log(self, base=2.718281828459045):
        # return self.log2()*0.6931471805599453   ## log2(x)/log2(e)
        return self.log2()/fxpt(base, self.m_frac).log2()
    def log10(self):
        return self.log2()*0.3010299956639812   ## log2(x)/log2(10)
    def exp(self):
        if self.m_fixed < -726817:
            return fxpt(0)
        res = 1+self
        prod = self*self
        fact = 2
        for i in range(2, 8):
            res += prod/fact
            fact *= i+1
            prod *= self
            print(i, prod, fact, res)
        return res
    
    # def __pow__(self, other[, modulo]):
    # def __rpow__(self, other[, modulo]):

def cbrt_glibc(f):
    if isinstance(f, float | int):
        m, e = math.frexp(f)
    elif isinstance(f, fxpt):
        m, e = f.frexp()
    if f < 0:
        m = -m
    SQR_CBRT2 = 1.5874010519681994748
    CBRT2 = 1.2599210498948731648
    factor = [1/SQR_CBRT2, 1/CBRT2, 1.0, CBRT2, SQR_CBRT2]
    ## u € [0.49265962052896956; 0.9987279190581733]
    ## u  is fx32, not fx16
    u = (0.492659620528969547+(0.697570460207922770-0.191502161678719066*m)*m)
    t2 = u*u*u
    if e < 0:
        ## mimic c modulo which is negative for negative numbers
        em = e%3-3
        if em == -3:
            em = 0
        ym = u * (t2+2*m)/(2*t2+m) * factor[2+em]
    else:
        ym = u * (t2+2*m)/(2*t2+m) * factor[2+e%3]
    # print(ym, e//3)
    if f < 0:
        return -ym*2**(e//3)
    else:
        return ym*2**(e//3)

STEP_I = 256
STEP_J = 256
MIN_I = -32768
MAX_I = +32768+1
MIN_J = -32768
MAX_J = +32768+1
DEBUG_IJ = (MAX_I-MIN_I)//STEP_I * (MAX_J-MIN_J)//STEP_J > 4194304
MIN = -2**31
MAX = 2**31+1
STEP = 65536
DEBUG = (MAX-MIN)//STEP > 4194304

def testAdditionInt():
    FUNC = "testAdditionInt"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            assert i+j == fxpt(i)+fxpt(j), "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
def testSubstractionInt():
    FUNC = "testSubstractionInt"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            assert i-j == fxpt(i)-fxpt(j), "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
def testMultiplicationInt():
    FUNC = "testMultiplicationInt"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            assert i*j == fxpt(i)*fxpt(j), "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
def testDivisionInt():
    FUNC = "testDivisionInt"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            if j != 0:
                assert i//j == (fxpt(i)//fxpt(j)).toInt(), "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
def testModuloInt():
    FUNC = "testModuloInt"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            if j != 0:
                assert i%j == (fxpt(i)%fxpt(j)).toInt(), "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
def testComparisonInt():
    FUNC = "testComparisonInt"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            assert (i <  j) == (fxpt(i) <  fxpt(j)), "{} {}".format(i, j)
            assert (i >  j) == (fxpt(i) >  fxpt(j)), "{} {}".format(i, j)
            assert (i <= j) == (fxpt(i) <= fxpt(j)), "{} {}".format(i, j)
            assert (i >= j) == (fxpt(i) >= fxpt(j)), "{} {}".format(i, j)
            assert (i == j) == (fxpt(i) == fxpt(j)), "{} {}".format(i, j)
            assert (i != j) == (fxpt(i) != fxpt(j)), "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
def testAdditionFloat():
    FUNC = "testAdditionFloat"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        fi = fxpt(i/65536)
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            fj = fxpt(j/65536)
            assert fi.toFloat()+fj.toFloat() == fi+fj, "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
def testSubstractionFloat():
    FUNC = "testSubstractionFloat"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        fi = fxpt(i/65536)
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            fj = fxpt(j/65536)
            assert fi.toFloat()-fj.toFloat() == fi-fj, "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
def testMultiplicationFloat():
    FUNC = "testMultiplicationFloat"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        fi = fxpt(i/65536)
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            fj = fxpt(j/65536)
            assert abs(fi.toFloat()*fj.toFloat() - fi*fj) < 1/32768, "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
def testDivisionFloat():
    FUNC = "testDivisionFloat"
    print(FUNC)
    for i in range(-128, 128):
        fi = fxpt(i/65536)
        for j in range(-128, 128):
            fj = fxpt(j/65536)
            if j != 0:
                assert fxpt(fi.toFloat()/fj.toFloat()) == fi/fj, "{} {}".format(i, j)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        fi = fxpt(i/65536)
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            fj = fxpt(j/65536)
            if j != 0:
                assert abs(fi.toFloat()/fj.toFloat() - fi/fj) < 1/32768, "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
def testModuloFloat():
    FUNC = "testComparisonFloat"
    print(FUNC)
    for i in range(-128, 128):
        fi = fxpt(i/65536)
        for j in range(-128, 128):
            fj = fxpt(j/65536)
            if j != 0:
                assert fxpt(fi.toFloat()%fj.toFloat()) == fi%fj, "{} {}".format(i, j)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        fi = fxpt(i/65536)
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            fj = fxpt(j/65536)
            if j != 0:
                assert abs(fi.toFloat()%fj.toFloat() - fi%fj) < 1/32768, "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
def testComparisonFloat():
    FUNC = "testComparisonFloat"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG_IJ and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            assert (i <  j) == (fxpt(i) <  fxpt(j)), "{} {}".format(i, j)
            assert (i >  j) == (fxpt(i) >  fxpt(j)), "{} {}".format(i, j)
            assert (i <= j) == (fxpt(i) <= fxpt(j)), "{} {}".format(i, j)
            assert (i >= j) == (fxpt(i) >= fxpt(j)), "{} {}".format(i, j)
            assert (i == j) == (fxpt(i) == fxpt(j)), "{} {}".format(i, j)
            assert (i != j) == (fxpt(i) != fxpt(j)), "{} {}".format(i, j)
    end = process_time_ns()
    return (end-start)/1e9
            
def testFrexp():
    FUNC = "testFrexp"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN, MAX, STEP):
        if DEBUG and int(100*(i-MIN)/(MAX-MIN)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        fi = fxpt.fromRaw(i)
        m, e = fxpt.frexp(fi)
        assert math.frexp(fi.toFloat()) == (m.toFloat(), e), "{}".format(i)
    end = process_time_ns()
    return (end-start)/1e9
        
def testSqrt():
    FUNC = "testSqrt"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(0, 2*MAX, STEP):
        if DEBUG and int(100*(i)/(2*MAX)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        fi = fxpt.fromRaw(i)
        assert abs(math.sqrt(fi.toFloat()) - fxpt.sqrt(fi).toFloat()) < 1/32768, "{}".format(i)
    end = process_time_ns()
    return (end-start)/1e9
def testCbrt():
    FUNC = "testCbrt"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN, MAX, STEP):
        if DEBUG and int(100*(i-MIN)/(MAX-MIN)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        fi = fxpt.fromRaw(i)
        # assert math.cbrt(fi.toFloat()) == fi.cbrt(), "{}".format(i)
        assert abs(math.cbrt(fi.toFloat()) - fxpt.cbrt(fi).toFloat()) < 1/32768, "{}".format(i)
    end = process_time_ns()
    return (end-start)/1e9
def testlog2():
    FUNC = "testlog2"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(STEP, 2*MAX, STEP):
        if DEBUG and int(100*(i-STEP)/(2*MAX)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        fi = fxpt.fromRaw(i)
        # assert math.log2(fi.toFloat()) == fi.log2(), "{}".format(i)
        assert abs(math.log2(fi.toFloat()) - fxpt.log2(fi).toFloat()) < 1/32768, "{}".format(i)
    end = process_time_ns()
    return (end-start)/1e9
def testlog(base = 2.718281828459045):
    FUNC = "testlog {}".format(base)
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(STEP, 2*MAX, STEP):
        if DEBUG and int(100*(i-STEP)/(2*MAX)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        fi = fxpt.fromRaw(i)
        # assert math.log(fi.toFloat(), base) == fi.log(base), "{}".format(i)
        assert abs(math.log(fi.toFloat(), base) - fxpt.log(fi, base).toFloat()) < 1/32768, "{}".format(i)
    end = process_time_ns()
    return (end-start)/1e9
def testlog10():
    FUNC = "testlog10"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(STEP, 2*MAX, STEP):
        if DEBUG and int(100*(i-STEP)/(2*MAX)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        fi = fxpt.fromRaw(i)
        # assert math.log10(fi.toFloat()) == fi.log10(), "{}".format(i)
        assert abs(math.log10(fi.toFloat()) - fxpt.log10(fi).toFloat()) < 1/32768, "{}".format(i)
    end = process_time_ns()
    return (end-start)/1e9
def testexp():
    global fi, i
    FUNC = "testexp"
    print(FUNC)
    prev = 0
    start = process_time_ns()
    for i in range(MIN, MAX, STEP):
        if DEBUG and int(100*(i-MIN)/(MAX-MIN)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        fi = fxpt.fromRaw(i)
        # assert math.exp(fi.toFloat()) == fi.exp(), "{}".format(i)
        assert abs(math.exp(fi.toFloat()) - fxpt.exp(fi).toFloat()) < 1/32768, "{}".format(i)
    end = process_time_ns()
    return (end-start)/1e9

a = fxpt(1)
print(a)
a = fxpt(1/65536)
print(a)
a = fxpt(-1/65536)
print(a)
a = fxpt(3.1415)
print(a)
a = fxpt(1e9)
print(a)
print(a+1)
a = fxpt(1)
b = fxpt(2)
c = fxpt(3)

a = fxpt(32767+1/65536)

if STEP_I * STEP_J < 16384:
    funcs = [globals()[f] for f in globals() if "test" in f]
    def smap(f, *args):
        return f(*args)
    with Pool() as p:
        res = p.map(smap, funcs)
else:
    # testAdditionInt()
    # testSubstractionInt()
    # testMultiplicationInt()
    # testDivisionInt()
    # testModuloInt()
    # testAdditionFloat()
    # testSubstractionFloat()
    # testMultiplicationFloat()
    # testDivisionFloat()
    # testModuloFloat()
    
    # testFrexp()
    
    # testSqrt()
    # testCbrt()
    testlog2()
    # testlog()
    # testlog(3)
    # testlog10()
    # testexp()
    

def exp(f, deg):
    global m, e
    m, e = np.frexp(f)
    res = 1 + m
    prod = m*m
    fact = 2
    for i in range(2, deg):
        res += prod/fact
        fact *= i+1
        prod *= m
    # return np.ldexp(res, e)*2.**e
    return res
def exp2(f, deg):
    global m, e
    m, e = np.frexp(f)
    res = 1 + m
    prod = m*m
    fact = 2
    for i in range(2, deg):
        res += prod/fact
        fact *= i+1
        prod *= m
    return np.ldexp(res, e)
def exp3(f, deg):
    global m, e
    m, e = np.frexp(f)
    res = 1 + m
    prod = m*m
    fact = 2
    for i in range(2, deg):
        res += prod/fact
        fact *= i+1
        prod *= m
    return np.ldexp(res, e)*2.**e
def exp4(f, deg):
    return (1+f/deg)**deg
def exp5(f, deg):
    # ln2 = fxpt(0.6931471805599453, 32)
    ln2 = math.log(2)
    z = f % ln2
    E = f // ln2
    return exp(z, deg)*2**E
def exp6(f, deg):
    # ln2 = fxpt(0.6931471805599453, 32)
    ln2 = math.log(2)
    z = f % ln2
    E = f // ln2
    return exp2(z, deg)*2**E
def exp7(f, deg):
    # ln2 = fxpt(0.6931471805599453, 32)
    ln2 = math.log(2)
    z = f % ln2
    E = f // ln2
    return exp3(z, deg)*2**E
def exp8(f, deg):
    # ln2 = fxpt(0.6931471805599453, 32)
    ln2 = math.log(2)
    z = f % ln2
    E = f // ln2
    return exp4(z, deg)*2**E
def exp9(f, deg):
    x = f
    f = f/2
    x2 = x*x/9
    x3 = x*x2/8
    x4 = x*x3/14
    x5 = x*x4/30
    return (1+f+x2+x3+x4+x5)/(1-f+x2-x3+x4-x5)

import matplotlib.pyplot as plt
import numpy as np
from IPython import get_ipython
ipython = get_ipython()
ipython.run_line_magic("matplotlib", "qt")
# %matplotlib qt
x = np.logspace(-6, 2, 10**4)
x = np.concatenate((-np.flip(x), x))
real = np.exp(x)
for i in range(3, 20):
    plt.figure()
    legend = []
    plot = plt.semilogy
    plot(x, abs(exp(x, i)-real)/real)
    legend.append("DL Taylor")
    plot(x, abs(exp2(x, i)-real)/real)
    legend.append("DL + ldexp")
    plot(x, abs(exp3(x, i)-real)/real)
    legend.append("DL + ldexp + 2**e")
    plot(x, abs(exp4(x, i)-real)/real)
    legend.append("1+x/n")
    plot(x, abs(exp5(x, i)-real)/real)
    legend.append("red+DL")
    plot(x, abs(exp6(x, i)-real)/real)
    legend.append("red+DL+ldexp")
    plot(x, abs(exp7(x, i)-real)/real)
    legend.append("red+DL+ldexp+2**e")
    plot(x, abs(exp8(x, i)-real)/real)
    legend.append("red+1+x/n")
    plot(x, abs(exp9(x, i)-real)/real)
    legend.append("pade5/5")
    # plot(x, np.exp(x))
    # legend.append("real")
    # plot(x, np.exp(x))
    # legend.append("inf")
    plt.legend(legend)
    plt.title(str(i))
    plt.show()



