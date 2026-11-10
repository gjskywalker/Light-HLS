// Reproducer: the current stream stub loses repeated FIFO events before RTL.
#include <hls_stream.h>

static void load_slow(const unsigned input[32], hls::stream<unsigned> &loaded)
{
  for (unsigned i = 0; i < 32; ++i) {
    unsigned x = input[i];
    for (unsigned j = 0; j < 4; ++j)
      x ^= x << 1;
    loaded.write(x);
  }
}

static void private_buffer(hls::stream<unsigned> &loaded,
                           hls::stream<unsigned> &computed, unsigned seed)
{
  unsigned buffer[32];
  for (unsigned i = 0; i < 32; ++i) {
#pragma HLS PIPELINE II=1
    buffer[i] = loaded.read() + seed;
  }
  for (unsigned i = 0; i < 16; ++i) {
    computed.write(buffer[i] + buffer[i+16]);
  }
}

static void store_slow(hls::stream<unsigned> &computed, unsigned output[16])
{
  for (unsigned i = 0; i < 16; ++i) {
    unsigned x = computed.read();
    for (unsigned j = 0; j < 8; ++j)
      x = (x << 1) ^ (x >> 1) ^ 0x9e3779b9U;
    output[i] = x;
  }
}

void local_dataflow(unsigned input[32], unsigned output[16], unsigned seed)
{
#pragma HLS DATAFLOW
  hls::stream<unsigned> loaded;
  hls::stream<unsigned> computed;
#pragma HLS STREAM variable=loaded depth=1
#pragma HLS STREAM variable=computed depth=1
  load_slow(input, loaded);
  private_buffer(loaded, computed, seed);
  store_slow(computed, output);
}
