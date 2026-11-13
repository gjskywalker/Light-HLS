#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"
#include <cstdint>

extern "C" uint64_t convert_bits(uint64_t value, unsigned inputWidth, unsigned outputWidth)
{
    using namespace llvm;
    auto semantics = [](unsigned width) -> const fltSemantics & {
        if (width == 16) return APFloat::IEEEhalf();
        if (width == 32) return APFloat::IEEEsingle();
        return APFloat::IEEEdouble();
    };
    APFloat result(semantics(inputWidth), APInt(inputWidth, value));
    bool loses;
    result.convert(semantics(outputWidth), APFloat::rmNearestTiesToEven, &loses);
    return result.bitcastToAPInt().getZExtValue();
}

// The oracle uses software IEEE arithmetic, not host half instructions or RTL.
extern "C" uint64_t evaluate_half(uint64_t av, uint64_t bv, int operation, int native)
{
    using namespace llvm;
    const auto rounding = APFloat::rmNearestTiesToEven;
    APFloat a(APFloat::IEEEhalf(), APInt(16, av));
    APFloat b(APFloat::IEEEhalf(), APInt(16, bv));
    APFloat c(APFloat::IEEEhalf(), APInt(16, 0x3400));
    bool loses = false;
    if ((!native && operation <= 3) || operation == 9)
    {
        a.convert(APFloat::IEEEsingle(), rounding, &loses);
        b.convert(APFloat::IEEEsingle(), rounding, &loses);
        c.convert(APFloat::IEEEsingle(), rounding, &loses);
    }
    switch (operation) {
    case 0: a.add(b, rounding); break;
    case 1: a.subtract(b, rounding); break;
    case 2: a.multiply(b, rounding); break;
    case 3: a.divide(b, rounding); break;
    case 4: a.changeSign(); break;
    case 5: if (a.compare(b) != APFloat::cmpLessThan) a = b; break;
    case 6:
    case 7: {
        APSInt integer(32, operation == 7);
        bool exact;
        a.convertToInteger(integer, APFloat::rmTowardZero, &exact);
        a.convertFromAPInt(integer, operation == 6, rounding);
        break;
    }
    case 9:
    case 10: a.fusedMultiplyAdd(b, c, rounding); break;
    default: break;
    }
    a.convert(APFloat::IEEEhalf(), rounding, &loses);
    return a.bitcastToAPInt().getZExtValue();
}
