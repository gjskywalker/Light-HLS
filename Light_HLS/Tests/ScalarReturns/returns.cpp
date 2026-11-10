#define CASE_KIND 0

unsigned scalar_returns(unsigned a, unsigned b, unsigned mode)
{
    unsigned accumulator = (mode & 1) ? a : b;
    unsigned prior = accumulator;
    unsigned trips = CASE_KIND == 1 ? mode & 15 : 4;
    for (unsigned index = 0; index < trips; ++index) {
        prior = accumulator;
        accumulator = accumulator * 3 + (a ^ index);
    }
#if CASE_KIND == 2
    return prior ^ (accumulator + b);
#else
    return (mode & 2) ? accumulator - b : accumulator + b;
#endif
}
