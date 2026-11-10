unsigned binding_affinity(unsigned a, unsigned b, unsigned x, unsigned y)
{
    unsigned p = a * x;
    unsigned q = b * y;
    unsigned r = b * (p ^ 7u);
    unsigned s = a * (q ^ 9u);
    return r ^ s;
}
