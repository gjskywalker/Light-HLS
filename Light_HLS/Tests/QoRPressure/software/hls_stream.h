#ifndef LIGHT_HLS_TEST_SOFTWARE_STREAM_H
#define LIGHT_HLS_TEST_SOFTWARE_STREAM_H

#include <cassert>
#include <queue>

// Native-reference queue only. Never include this header in synthesis runs.
namespace hls {
template <typename T> class stream {
    std::queue<T> values;
public:
    void write(T value) { values.push(value); }
    T read() {
        assert(!values.empty());
        T value = values.front();
        values.pop();
        return value;
    }
};
}
#endif
