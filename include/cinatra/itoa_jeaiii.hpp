#pragma once
/*
MIT License

Copyright (c) 2017 James Edward Anhalt III - https://github.com/jeaiii/itoa

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stdint.h>

// form a 4.32 fixed point number: t = u * 2^32 / 10^log10(u)
// use as much precision as possible when needed (log10(u) >= 5) 
// so shift up then down afterwards by log10(u) * log2(10) ~= 53/16
// need to round up before and or after in some cases
// once we have the fixed point number we can read off the digit in the upper 32 bits
// and multiply the lower 32 bits by 10 to get the next digit and so on
// we can do 2 digits at a time by multiplying by 100 each time

// TODO:
// x64 optimized verison (no need to line up on 32bit boundary, so can multiply by 5 instead of 10 using lea instruction)
// full 64 bit LG()
// try splitting the number into chucks that can be processed independently
// try odd digit first
// try writing 4 chars at a time

namespace cinatra {
#if 0
    // 1 char at a time

#define W(N, I) b[N] = char(I) + '0'
#define A(N) t = (uint64_t(1) << (32 + N / 5 * N * 53 / 16)) / uint32_t(1e##N) + 1 - N / 9, t *= u, t >>= N / 5 * N * 53 / 16, t += N / 5 * 4, W(0, t >> 32)
#define D(N) t = uint64_t(10) * uint32_t(t), W(N, t >> 32)

#define L0 W(0, u)
#define L1 A(1), D(1)
#define L2 A(2), D(1), D(2)
#define L3 A(3), D(1), D(2), D(3)
#define L4 A(4), D(1), D(2), D(3), D(4)
#define L5 A(5), D(1), D(2), D(3), D(4), D(5)
#define L6 A(6), D(1), D(2), D(3), D(4), D(5), D(6)
#define L7 A(7), D(1), D(2), D(3), D(4), D(5), D(6), D(7)
#define L8 A(8), D(1), D(2), D(3), D(4), D(5), D(6), D(7), D(8)
#define L9 A(9), D(1), D(2), D(3), D(4), D(5), D(6), D(7), D(8), D(9)

#else
    // 2 chars at a time

    struct pair { char t, o; };
#define CINATRAP(T) T, '0',  T, '1', T, '2', T, '3', T, '4', T, '5', T, '6', T, '7', T, '8', T, '9'
    static const pair s_pairs[] = { CINATRAP('0'), CINATRAP('1'), CINATRAP('2'), CINATRAP('3'), CINATRAP('4'), CINATRAP('5'), CINATRAP('6'), CINATRAP('7'), CINATRAP('8'), CINATRAP('9') };

#define CINATRAW(N, I) *(pair*)&b[N] = s_pairs[I]
#define CINATRAA(N) t = (uint64_t(1) << (32 + N / 5 * N * 53 / 16)) / uint32_t(1e##N) + 1 + N/6 - N/8, t *= u, t >>= N / 5 * N * 53 / 16, t += N / 6 * 4, CINATRAW(0, t >> 32)
#define CINATRAS(N) b[N] = char(uint64_t(10) * uint32_t(t) >> 32) + '0'
#define CINATRAD(N) t = uint64_t(100) * uint32_t(t), CINATRAW(N, t >> 32)

#define CINATRAL0 b[0] = char(u) + '0'
#define CINATRAL1 CINATRAW(0, u)
#define CINATRAL2 CINATRAA(1), CINATRAS(2)
#define CINATRAL3 CINATRAA(2), CINATRAD(2)
#define CINATRAL4 CINATRAA(3), CINATRAD(2), CINATRAS(4)
#define CINATRAL5 CINATRAA(4), CINATRAD(2), CINATRAD(4)
#define CINATRAL6 CINATRAA(5), CINATRAD(2), CINATRAD(4), CINATRAS(6)
#define CINATRAL7 CINATRAA(6), CINATRAD(2), CINATRAD(4), CINATRAD(6)
#define CINATRAL8 CINATRAA(7), CINATRAD(2), CINATRAD(4), CINATRAD(6), CINATRAS(8)
#define CINATRAL9 CINATRAA(8), CINATRAD(2), CINATRAD(4), CINATRAD(6), CINATRAD(8)

#endif

#define LN(N) (CINATRAL##N, b += N + 1)
#define LZ LN
    // if you want to '\0' terminate
    //#define LZ(N) &(L##N, b[N + 1] = '\0')

#define LG(F) (u<100 ? u<10 ? F(0) : F(1) : u<1000000 ? u<10000 ? u<1000 ? F(2) : F(3) : u<100000 ? F(4) : F(5) : u<100000000 ? u<10000000 ? F(6) : F(7) : u<1000000000 ? F(8) : F(9))

    inline char* u32toa_jeaiii(uint32_t u, char* b) {
        uint64_t t;
        return LG(LZ);
    }

    inline char* i32toa_jeaiii(int32_t i, char* b) {
        uint32_t u = i < 0 ? *b++ = '-', 0 - uint32_t(i) : i;
        uint64_t t;
        return LG(LZ);
    }

    inline char* u64toa_jeaiii(uint64_t n, char* b) {
        uint32_t u;
        uint64_t t;

        if (uint32_t(n >> 32) == 0)
            return u = uint32_t(n), LG(LZ);

        uint64_t a = n / 100000000;

        if (uint32_t(a >> 32) == 0) {
            u = uint32_t(a);
            LG(LN);
        }
        else {
            u = uint32_t(a / 100000000);
            LG(LN);
            u = a % 100000000;
            LN(7);
        }

        u = n % 100000000;
        return LZ(7);
    }

    inline char* i64toa_jeaiii(int64_t i, char* b) {
        uint64_t n = i < 0 ? *b++ = '-', 0 - uint64_t(i) : i;
        return u64toa_jeaiii(n, b);
    }
}
