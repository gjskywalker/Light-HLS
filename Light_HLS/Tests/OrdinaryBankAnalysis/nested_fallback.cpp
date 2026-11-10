// Exercise ordinary-loop bank coverage without pipeline directives.
void nested_fallback(unsigned input[256], unsigned output[8], unsigned count)
{
#pragma HLS ARRAY_PARTITION variable=input type=cyclic factor=4 dim=1
  for (unsigned row = 0; row < 2; ++row)
    for (unsigned col = 0; col < 4; ++col) {
      unsigned i = row*4 + col;
      output[i] = input[4*i] + input[4*i+1];
    }
}
