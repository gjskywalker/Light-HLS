extern "C" double backend_double(double a, double b, int operation)
{
    switch (operation) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a / b;
    case 4: return -a;
    case 5: return a < b ? a : b;
    case 6: return static_cast<double>(static_cast<int>(a));
    case 7: return static_cast<double>(static_cast<unsigned>(a));
    case 8: return static_cast<double>(static_cast<float>(a));
    case 9: return __builtin_fma(a, b, 0.25);
    default: return a;
    }
}
