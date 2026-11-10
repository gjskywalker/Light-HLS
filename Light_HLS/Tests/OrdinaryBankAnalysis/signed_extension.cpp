// Exercise ordinary-loop bank coverage without pipeline directives.
void signed_extension(unsigned input[256], unsigned output[8], unsigned count)
{
#pragma HLS ARRAY_PARTITION variable=input type=cyclic factor=3 dim=1
  for (unsigned i = 0; i < 8; ++i) {
    signed char index = (signed char)(124 + i);
    output[i] = input[(int)index + 128];
  }
}
