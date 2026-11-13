void dut(_Float16 a[16], _Float16 b[16], _Float16 c[16])
{
    _Float16 scratch[16];
    for (int i = 0; i < 16; ++i)
        scratch[i] = a[i] * b[i];
    for (int i = 0; i < 16; ++i)
        c[i] = scratch[i] + static_cast<_Float16>(0.5f);
}
