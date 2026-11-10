// Check single-writer payload binding without changing memory events.
void partial_banks(unsigned input[32], unsigned output[16], unsigned seed)
{
  unsigned buffer[32];
#pragma HLS ARRAY_PARTITION variable=buffer type=cyclic factor=4 dim=1
#pragma HLS BIND_STORAGE variable=buffer type=ram_1p impl=lutram latency=1
  for (unsigned i = 0; i < 16; ++i)
    buffer[i] = input[i] + seed;
  for (unsigned i = 0; i < 4; ++i)
    buffer[4*i] += 7;
  for (unsigned i = 0; i < 16; ++i)
    output[i] = buffer[i] ^ (seed + 1);
}
