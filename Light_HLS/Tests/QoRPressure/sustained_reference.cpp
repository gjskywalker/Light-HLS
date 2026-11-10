#include <cstdio>
void qor_pressure(unsigned input[256], unsigned output[256], unsigned coefficient);
int main()
{
    unsigned input[256], output[256];
    for (unsigned i = 0; i < 256; ++i)
        input[i] = (i * 131u) ^ 0xa531e297u;
    for (unsigned trial = 0; trial < 32; ++trial)
    {
        qor_pressure(input, output, trial * 29u + 7u);
        for (unsigned i = 0; i < 256; ++i)
            std::printf("RESULT %u %u %u\n", trial, i, output[i]);
    }
}
