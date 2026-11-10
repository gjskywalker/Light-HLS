// Exercise ordinary-loop bank coverage without pipeline directives.
void descending(unsigned input[256], unsigned output[8], unsigned count)
{
#pragma HLS ARRAY_PARTITION variable=input type=cyclic factor=4 dim=1
  for (unsigned i = 0; i < 8; ++i)
    output[i] = input[28 - 4*i];
}
