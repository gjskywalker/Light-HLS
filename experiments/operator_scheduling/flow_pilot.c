/* Straight-line kernels with a shared read-only, single-port memory interface. */

unsigned lookup_xor(const unsigned memory[8], unsigned key)
{
    unsigned mask = memory[0];
    unsigned index = (memory[1] ^ key) & 7u;
    unsigned value = memory[index];
    return mask ^ value;
}

unsigned mix_words(const unsigned memory[8], unsigned key)
{
    unsigned tail = memory[0];
    unsigned value = memory[1] ^ key;
    value = value * 0x85ebca6bu;
    value ^= value >> 13;
    value *= 0xc2b2ae35u;
    return tail ^ value;
}

unsigned weighted_sum(const unsigned memory[8], unsigned key)
{
    unsigned bias = memory[0];
    unsigned left = memory[1] * key;
    unsigned right = memory[2] * (key ^ 17u);
    right ^= right >> 16;
    right *= key | 1u;
    return bias + (left ^ right);
}

unsigned pack_channels(const unsigned memory[8], unsigned key)
{
    unsigned alpha = memory[0] & 255u;
    unsigned red = (memory[1] + key) & 255u;
    unsigned green = (memory[2] ^ (key >> 8)) & 255u;
    unsigned blue = memory[3] & 255u;
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}
