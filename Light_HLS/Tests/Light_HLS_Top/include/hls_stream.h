#ifndef LIGHT_HLS_STREAM_H
#define LIGHT_HLS_STREAM_H

namespace hls
{

template <typename T, int Depth = 2>
class stream
{
    static_assert(Depth > 0, "hls::stream depth must be positive");

public:
    stream() = default;
    explicit stream(const char *)
    {
    }

    __attribute__((always_inline)) void write(const T &value)
    {
        storage_ = value;
    }

    __attribute__((always_inline)) T read()
    {
        return storage_;
    }

    __attribute__((always_inline)) void read(T &value)
    {
        value = storage_;
    }

    __attribute__((always_inline)) stream &operator<<(const T &value)
    {
        write(value);
        return *this;
    }

    __attribute__((always_inline)) stream &operator>>(T &value)
    {
        read(value);
        return *this;
    }

private:
    // Synthesis-only event storage, not a software queue. Volatile preserves
    // every read/write through Clang and LLVM, including unused reads and
    // repeated equal writes. Construction must not enqueue an initial token.
    volatile T storage_;
};

} // namespace hls

#endif
