#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Created on Fri Jan 17 11:36:03 2025

@author: egloffv
"""

import math
from multiprocessing import Pool


class fxpt:
    def __init__(self, value = 0):
        if isinstance(value, fxpt):
            self.m_fixed = value.m_fixed
        else:
            self.m_fixed = int(value*65536)
    def __repr__(self) -> str:
        return "{:x} {:.8f}".format(self.m_fixed, self.m_fixed/65536)
    def __add__(self, other):
        return fxpt.fromRaw(self.m_fixed + fxpt(other).m_fixed)
    def __radd__(self, other):
        return self.__add__(other)
    def __sub__(self, other):
        return fxpt.fromRaw(self.m_fixed - fxpt(other).m_fixed)
    def __rsub__(self, other):
        return fxpt(other).__sub__(self)
    def __mul__(self, other):
        return fxpt.fromRaw((self.m_fixed* fxpt(other).m_fixed) >> 16)
    def __rmul__(self, other):
        return self.__mul__(other)
    def __truediv__(self, other):
        return fxpt.fromRaw((self.m_fixed << 16) / fxpt(other).m_fixed)
    def __rtruediv__(self, other):
        return fxpt(other).__truediv__(self)
    def __floordiv__(self, other):
        return fxpt(self.m_fixed // fxpt(other).m_fixed) 
    def __rfloordiv__(self, other):
        return fxpt(other).__floordiv__(self)
    def __mod__(self, other):
        return fxpt.fromRaw(self.m_fixed % other.m_fixed)
    def __rmod__(self, other):
        return fxpt(other).__mod__(self)
    def __neg__(self):
        return fxpt.fromRaw(-self.m_fixed)
    def __pos__(self):
        return fxpt(self)
    def __abs__(self):
        return fxpt.fromRaw(abs(self.m_fixed))
    def __round__(self, ndigits = None):
        return round(self.m_fixed/65536, ndigits)
    def __trunc__(self):
        return int(self.m_fixed/65536)
    def __floor__(self):
        return math.floor(self.m_fixed/65536)
    def __ceil__(self):
        return math.ceil(self.m_fixed/65536)
    def __lt__(self, other) -> bool:
        return fxpt(self).m_fixed < fxpt(other).m_fixed
    def __le__(self, other) -> bool:
        return fxpt(self).m_fixed <= fxpt(other).m_fixed
    def __eq__(self, other) -> bool:
        return fxpt(self).m_fixed == fxpt(other).m_fixed
    def __ne__(self, other) -> bool:
        return fxpt(self).m_fixed != fxpt(other).m_fixed
    def __gt__(self, other) -> bool:
        return fxpt(self).m_fixed > fxpt(other).m_fixed
    def __ge__(self, other) -> bool:
        return fxpt(self).m_fixed >= fxpt(other).m_fixed
    def __hash__(self):
        return hash(self.m_fixed)
    def __bool__(self):
        return bool(self.m_fixed)
    def fromRaw(value: int):
        res = fxpt()
        res.m_fixed = int(value)
        return res
    def toInt(self) -> int:
        return self.m_fixed >> 16 ## round towards -Inf
    def toFloat(self) -> float:
        return self.m_fixed / 65536 ## round towards 0
    
    def sqrt(self):
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
        ## error with -4096, 1,
        # xn = float(1 << (self.m_fixed.bit_length() >> 3))
        x0_table = [
            float('nan'),
            1./32, 1./32, 1./16, 1./16, 1./16, 1./8, 1./8, 1./8,
            1./4, 1./4, 1./4, 1./2, 1./2, 1./2, 1, 1, 1,
            2, 2, 2, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32
        ]
        xn = fxpt(x0_table[self.m_fixed.bit_length()])
        print(self, xn)
        neg = False
        if self.m_fixed < 0:
            neg = True
            self.m_fixed = - self.m_fixed
        for i in range(6):
            xn3 = xn*xn*xn
            xn = xn*((xn3+2*self)/(2*xn3+self)) ## division first before multiplication to avoid underflow into 0 for small values
            print(xn)
        if neg:
            return -xn
        return xn
    
    def frexp(self):
        if self.m_fixed == 0:
            return 0, 0
        e = self.m_fixed.bit_length()-16
        if e >= 0:
            m = self.m_fixed >> e
        else:
            m = self.m_fixed << -e
        return fxpt.fromRaw(m), e
    
    # def __pow__(self, other[, modulo]):
    # def __rpow__(self, other[, modulo]):

def mycbrt(f):
    if isinstance(f, float | int):
        m, e = math.frexp(abs(f))
    elif isinstance(f, fxpt):
        e = f.m_fixed.bit_length()-16
        m = 1
    u = (0.492659620528969547+(0.697570460207922770-0.191502161678719066*m)*m)

STEP_I = 4096
STEP_J = 128
MIN_I = -32768
MAX_I = +32768
MIN_J = -32768
MAX_J = +32768
DEBUG = STEP_I * STEP_J < 1024

def testAdditionInt():
    FUNC = "testAdditionInt"
    print(FUNC)
    prev = 0
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            assert i+j == fxpt(i)+fxpt(j), "{} {}".format(i, j)
def testSubstractionInt():
    FUNC = "testSubstractionInt"
    print(FUNC)
    prev = 0
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            assert i-j == fxpt(i)-fxpt(j), "{} {}".format(i, j)
def testMultiplicationInt():
    FUNC = "testMultiplicationInt"
    print(FUNC)
    prev = 0
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            assert i*j == fxpt(i)*fxpt(j), "{} {}".format(i, j)
def testDivisionInt():
    FUNC = "testDivisionInt"
    print(FUNC)
    prev = 0
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            if j != 0:
                assert i//j == (fxpt(i)//fxpt(j)).toInt(), "{} {}".format(i, j)
def testModuloInt():
    FUNC = "testModuloInt"
    print(FUNC)
    prev = 0
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            if j != 0:
                assert i%j == (fxpt(i)%fxpt(j)).toInt(), "{} {}".format(i, j)
def testComparisonInt():
    FUNC = "testComparisonInt"
    print(FUNC)
    prev = 0
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            assert (i <  j) == (fxpt(i) <  fxpt(j)), "{} {}".format(i, j)
            assert (i >  j) == (fxpt(i) >  fxpt(j)), "{} {}".format(i, j)
            assert (i <= j) == (fxpt(i) <= fxpt(j)), "{} {}".format(i, j)
            assert (i >= j) == (fxpt(i) >= fxpt(j)), "{} {}".format(i, j)
            assert (i == j) == (fxpt(i) == fxpt(j)), "{} {}".format(i, j)
            assert (i != j) == (fxpt(i) != fxpt(j)), "{} {}".format(i, j)
def testAdditionFloat():
    FUNC = "testAdditionFloat"
    print(FUNC)
    prev = 0
    for i in range(MIN_I, MAX_I, STEP_I):
        fi = fxpt(i/65536)
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            fj = fxpt(j/65536)
            assert fi.toFloat()+fj.toFloat() == fi+fj, "{} {}".format(i, j)
def testSubstractionFloat():
    FUNC = "testSubstractionFloat"
    print(FUNC)
    prev = 0
    for i in range(MIN_I, MAX_I, STEP_I):
        fi = fxpt(i/65536)
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            fj = fxpt(j/65536)
            assert fi.toFloat()-fj.toFloat() == fi-fj, "{} {}".format(i, j)
def testMultiplicationFloat():
    FUNC = "testMultiplicationFloat"
    print(FUNC)
    prev = 0
    for i in range(MIN_I, MAX_I, STEP_I):
        fi = fxpt(i/65536)
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            fj = fxpt(j/65536)
            assert abs(fi.toFloat()*fj.toFloat() - fi*fj) < 1/32768, "{} {}".format(i, j)
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
    for i in range(MIN_I, MAX_I, STEP_I):
        fi = fxpt(i/65536)
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            fj = fxpt(j/65536)
            if j != 0:
                assert abs(fi.toFloat()/fj.toFloat() - fi/fj) < 1/32768, "{} {}".format(i, j)
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
    for i in range(MIN_I, MAX_I, STEP_I):
        fi = fxpt(i/65536)
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            fj = fxpt(j/65536)
            if j != 0:
                assert abs(fi.toFloat()%fj.toFloat() - fi%fj) < 1/32768, "{} {}".format(i, j)
def testComparisonFloat():
    FUNC = "testComparisonFloat"
    print(FUNC)
    prev = 0
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        for j in range(MIN_J, MAX_J, STEP_J):
            assert (i <  j) == (fxpt(i) <  fxpt(j)), "{} {}".format(i, j)
            assert (i >  j) == (fxpt(i) >  fxpt(j)), "{} {}".format(i, j)
            assert (i <= j) == (fxpt(i) <= fxpt(j)), "{} {}".format(i, j)
            assert (i >= j) == (fxpt(i) >= fxpt(j)), "{} {}".format(i, j)
            assert (i == j) == (fxpt(i) == fxpt(j)), "{} {}".format(i, j)
            assert (i != j) == (fxpt(i) != fxpt(j)), "{} {}".format(i, j)
            
def testFrexp():
    FUNC = "testFrexp"
    print(FUNC)
    prev = 0
    for i in range(MIN_I, MAX_I, STEP_I):
        if DEBUG and int(100*(i-MIN_I)/(MAX_I-MIN_I)) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        fi = fxpt.fromRaw(i)
        assert math.frexp(fi.toFloat()) == fxpt.frexp(fi), "{}".format(i)
        
def testSqrt():
    FUNC = "testSqrt"
    print(FUNC)
    prev = 0
    for i in range(0, 2**31+1, STEP_I):
        if DEBUG and int(100*(i)/2**31) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        fi = fxpt.fromRaw(i)
        assert abs(math.sqrt(fi.toFloat()) - fxpt.sqrt(fi)) < 1/32768, "{}".format(i)
def testCbrt():
    FUNC = "testCbrt"
    print(FUNC)
    prev = 0
    # for i in range(-2**31, 2**31+1, STEP_I):
    for i in range(-4095, 2**31+1, STEP_I):
        if DEBUG and int(100*(i+2**31)/2**32) != prev:
            prev += 1
            print("{} {}%".format(FUNC, prev))
        fi = fxpt.fromRaw(i)
        assert abs(math.cbrt(fi.toFloat()) - fxpt.cbrt(fi)) < 1/32768, "{}".format(i)


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

if STEP_I * STEP_J < 16384:
    funcs = [globals()[f] for f in globals() if "test" in f]
    def smap(f, *args):
        return f(*args)
    with Pool() as p:
        p.map(smap, funcs)
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
    
    testFrexp()
    
    # testSqrt()
    testCbrt()
    