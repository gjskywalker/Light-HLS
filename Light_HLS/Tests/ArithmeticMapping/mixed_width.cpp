unsigned arithmetic_mapping(unsigned a, unsigned b, int c, unsigned mode)
{
    if (mode & 1u)
        return (a & 0x1ffffu) * (b & 0x1ffffu) + a;
    // The usual unsigned conversion preserves all two's-complement bits.
    return static_cast<unsigned>(c) * b + a;
}
