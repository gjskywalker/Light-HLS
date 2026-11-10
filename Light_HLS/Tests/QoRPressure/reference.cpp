#include <cstdio>

void qor_pressure(unsigned input[256], unsigned output[256], unsigned coefficient);

int main()
{
    unsigned input[256], output[256];
    for (unsigned trial = 0; trial < 8; ++trial)
    {
        for (unsigned i = 0; i < 256; ++i)
            input[i] = (i * 131u + trial * 977u) ^ 0xa531e297u;
        qor_pressure(input, output, trial * 29u + 7u);
        for (unsigned i = 0; i < 256; ++i)
            std::printf("RESULT %u %u %u\n", trial, i, output[i]);
    }
}
