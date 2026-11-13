#include <cstdint>

#define BITS(To, value) __builtin_bit_cast(To, (value))

extern "C" uint64_t backend_conversion(uint64_t value_bits, int operation)
{
    switch (operation)
    {
    case 0: return BITS(uint32_t, static_cast<float>(BITS(_Float16, uint16_t(value_bits))));
    case 1: return BITS(uint64_t, static_cast<double>(BITS(_Float16, uint16_t(value_bits))));
    case 2: return BITS(uint16_t, static_cast<_Float16>(BITS(float, uint32_t(value_bits))));
    case 3: return BITS(uint64_t, static_cast<double>(BITS(float, uint32_t(value_bits))));
    case 4: return BITS(uint16_t, static_cast<_Float16>(BITS(double, value_bits)));
    case 5: return BITS(uint32_t, static_cast<float>(BITS(double, value_bits)));
    default: return value_bits;
    }
}
