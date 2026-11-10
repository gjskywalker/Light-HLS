// Check single-writer payload binding without changing memory events.
void multiple_dual(unsigned input[32], unsigned output[16], unsigned seed)
{
  unsigned buffer[32];
#pragma HLS BIND_STORAGE variable=buffer type=ram_2p impl=lutram latency=2
  for (unsigned i = 0; i < 16; ++i) {
#pragma HLS PIPELINE II=1
    unsigned x = input[i];
    buffer[i] = x + seed;
    buffer[i+16] = x ^ seed;
  }
  for (unsigned i = 0; i < 16; ++i) {
    output[i] = buffer[i] + buffer[i+16];
  }
}
