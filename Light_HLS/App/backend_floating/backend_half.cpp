extern "C" _Float16 backend_half(_Float16 a, _Float16 b, int operation)
{
    switch (operation) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a / b;
    case 4: return -a;
    case 5: return a < b ? a : b;
    case 6: return static_cast<_Float16>(static_cast<int>(a));
    case 7: return static_cast<_Float16>(static_cast<unsigned>(a));
    case 8: return static_cast<_Float16>(static_cast<double>(a));
    // This source spelling explicitly requests FP32 FMA followed by narrowing.
    case 9: return static_cast<_Float16>(__builtin_fmaf(a, b, 0.25f));
    case 10: return __builtin_elementwise_fma(a, b, static_cast<_Float16>(0.25f));
    default: return a;
    }
}
