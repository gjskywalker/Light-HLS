/* Same unsigned 32-bit, eight-word, single-port interface as the pilot. */
#include "flow_pilot.c"

unsigned dot_product(const unsigned memory[8], unsigned key)
{
    unsigned bias = memory[0] + key;
    return bias + memory[1] * memory[2] + memory[3] * memory[4];
}

unsigned bit_extract(const unsigned memory[8], unsigned key)
{
    unsigned low = memory[0] & 255u;
    unsigned shift = memory[1] & 15u;
    return low | (((memory[2] ^ key) >> shift) << 8);
}

unsigned crc_step(const unsigned memory[8], unsigned key)
{
    unsigned state = key ^ memory[2];
    unsigned mask = 0u - (state & 1u);
    return (state >> 1) ^ (mask & memory[1]) ^ memory[0];
}

unsigned wavelet_lift(const unsigned memory[8], unsigned key)
{
    unsigned detail = memory[0] - ((memory[1] + memory[3]) >> 1);
    return memory[2] + ((detail + key + 2u) >> 2);
}

unsigned hamming_distance(const unsigned memory[8], unsigned key)
{
    unsigned base = memory[0], mask = memory[1];
    unsigned x = memory[2] ^ key;
    x -= (x >> 1) & 0x55555555u;
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0f0f0f0fu;
    return base + (((x * 0x01010101u) >> 24) ^ mask);
}

unsigned majority_vote(const unsigned memory[8], unsigned key)
{
    unsigned a = memory[0], b = memory[1], c = memory[2];
    return ((a & b) | ((a ^ b) & c)) ^ key;
}

unsigned linear_interp(const unsigned memory[8], unsigned key)
{
    unsigned offset = memory[2];
    unsigned fraction = key & 255u;
    unsigned a = memory[0], b = memory[1];
    return ((a * (256u - fraction) + b * fraction) >> 8) + offset;
}

unsigned horner_eval(const unsigned memory[8], unsigned key)
{
    unsigned constant = memory[0];
    return ((memory[3] * key + memory[2]) * key + memory[1]) * key + constant;
}

unsigned table_blend(const unsigned memory[8], unsigned key)
{
    unsigned bias = memory[0], x = memory[1] ^ key, y = memory[2] + key;
    return bias + memory[x & 7u] * memory[y & 7u];
}

unsigned sum_squares(const unsigned memory[8], unsigned key)
{
    unsigned bias = memory[0], a = memory[1] - key, b = memory[2] - key;
    unsigned c = memory[3] - key;
    return bias + a * a + b * b + c * c;
}
