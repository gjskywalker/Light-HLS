extern "C" float backend_floating(float a, float b, int operation)
{
    switch (operation) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a / b;
    case 4: return -a;
    case 5: return a < b ? a : b;
    case 6: return static_cast<float>(static_cast<int>(a));
    case 7: return static_cast<float>(static_cast<unsigned>(a));
    case 9: return __builtin_fmaf(a, b, 0.25f);
    default: return a;
    }
}
