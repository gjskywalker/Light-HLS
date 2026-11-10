#include "HI_BackendContext.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/KnownBits.h"
#include <algorithm>

namespace light_hls::backend
{
unsigned unsignedArithmeticWidth(const llvm::Value *value, const llvm::DataLayout &layout)
{
    const unsigned width = value->getType()->getIntegerBitWidth();
    const llvm::KnownBits known = llvm::computeKnownBits(value, layout);
    return std::max(1U, width - known.countMinLeadingZeros());
}
} // namespace light_hls::backend
