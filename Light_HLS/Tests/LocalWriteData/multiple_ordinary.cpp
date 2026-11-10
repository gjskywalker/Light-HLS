// Check single-writer payload binding without changing memory events.
void multiple_ordinary(unsigned input[32], unsigned output[16], unsigned seed)
{
  unsigned buffer[32];
#pragma HLS ARRAY_PARTITION variable=buffer type=cyclic factor=4 dim=1
#pragma HLS BIND_STORAGE variable=buffer type=ram_1p impl=lutram latency=1
  for (unsigned i = 0; i < 16; ++i) {
    unsigned x = input[i];
    buffer[i] = x + seed;
    buffer[i+16] = x ^ seed;
  }
  for (unsigned i = 0; i < 16; ++i) {
    output[i] = buffer[i] + buffer[i+16];
  }
}
