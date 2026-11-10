static void private_buffer(const unsigned input[32], unsigned computed[16], unsigned seed)
{
  unsigned buffer[32];
  for (unsigned i = 0; i < 32; ++i) {
#pragma HLS PIPELINE II=1
    buffer[i] = input[i] + seed;
  }
  for (unsigned i = 0; i < 16; ++i)
    computed[i] = buffer[i] + buffer[i+16];
}

static void store_stage(const unsigned computed[16], unsigned output[16])
{
  for (unsigned i = 0; i < 16; ++i)
    output[i] = computed[i];
}

void local_dataflow(unsigned input[32], unsigned output[16], unsigned seed)
{
  unsigned computed[16];
#pragma HLS DATAFLOW
  private_buffer(input, computed, seed);
  store_stage(computed, output);
}
