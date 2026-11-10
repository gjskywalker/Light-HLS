// Check single-writer payload binding without changing memory events.
void single_latency2(unsigned input[32], unsigned output[16], unsigned seed)
{
  unsigned buffer[32];
#pragma HLS ARRAY_PARTITION variable=buffer type=cyclic factor=4 dim=1
#pragma HLS BIND_STORAGE variable=buffer type=ram_1p impl=lutram latency=2
  for (unsigned i = 0; i < 32; ++i) {
#pragma HLS PIPELINE II=1
    buffer[i] = input[i] + seed;
  }
  for (unsigned i = 0; i < 16; ++i) {
    output[i] = buffer[i] + buffer[i+16];
  }
}
