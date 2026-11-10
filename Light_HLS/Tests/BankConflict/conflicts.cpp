#include "HI_BackendContext.h"
#include <cassert>
#include <random>
using namespace light_hls::backend;
int main()
{
    MemoryAccessInfo a, b;
    a.memory = b.memory = "tile";
    a.possibleBanks = b.possibleBanks = {0, 1};
    a.iterationBanks = {0, 1, 0, 1};
    b.iterationBanks = {1, 0, 1, 0};
    assert(!memoryRequestsConflict(a, 0, b, 0, 1));
    assert(memoryRequestsConflict(a, 0, b, 1, 1));
    assert(memoryRequestsConflict(a, 1, b, 0, 1));
    assert(!memoryRequestsConflict(a, 0, b, 1, 2));
    assert(!memoryRequestsConflict(a, 0, b, 8, 1));
    b.iterationBanks.clear();
    assert(memoryRequestsConflict(a, 0, b, 0, 1));
    assert(memoryRequestsConflict(a, 0, b, 0, 0));
    assert(!memoryRequestsConflict(a, 0, b, 1, 0));
    b.possibleBanks.clear();
    assert(memoryRequestsConflict(a, 0, b, 0, 1));
    std::mt19937 random(0xBADC0DE);
    for (unsigned trial = 0; trial < 20000; ++trial)
    {
        const unsigned count = 1 + random() % 32, ii = 1 + random() % 7;
        const int x = random() % 60, y = random() % 60;
        a.iterationBanks.resize(count);
        b.iterationBanks.resize(count);
        for (unsigned i = 0; i < count; ++i)
        {
            a.iterationBanks[i] = random() % 4;
            b.iterationBanks[i] = random() % 4;
        }
        bool collision = false;
        for (unsigned i = 0; i < count; ++i)
            for (unsigned j = 0; j < count; ++j)
                collision |= i * ii + x == j * ii + y && a.iterationBanks[i] == b.iterationBanks[j];
        assert(memoryRequestsConflict(a, x, b, y, ii) == collision);
        assert(memoryRequestsConflict(b, y, a, x, ii) == collision);
    }
}
