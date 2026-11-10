#include "HI_BackendContext.h"
#include <algorithm>

namespace light_hls::backend
{
bool memoryRequestsConflict(const MemoryAccessInfo &left, int leftStart,
                            const MemoryAccessInfo &right, int rightStart,
                            unsigned initiationInterval)
{
    if (left.memory != right.memory)
        return false;
    const long long delta = static_cast<long long>(leftStart) - rightStart;
    if (!initiationInterval ? delta != 0 : delta % initiationInterval != 0)
        return false;
    if (initiationInterval && !left.iterationBanks.empty() &&
        left.iterationBanks.size() == right.iterationBanks.size())
    {
        const long long offset = delta / initiationInterval;
        const long long count = left.iterationBanks.size();
        for (long long i = std::max(0LL, -offset); i < std::min(count, count - offset); ++i)
            if (left.iterationBanks[i] == right.iterationBanks[i + offset])
                return true;
        return false;
    }
    if (left.possibleBanks.empty() || right.possibleBanks.empty())
        return true;
    return std::any_of(left.possibleBanks.begin(), left.possibleBanks.end(),
        [&](unsigned bank) {
            return std::find(right.possibleBanks.begin(), right.possibleBanks.end(), bank) !=
                   right.possibleBanks.end();
        });
}
} // namespace light_hls::backend
