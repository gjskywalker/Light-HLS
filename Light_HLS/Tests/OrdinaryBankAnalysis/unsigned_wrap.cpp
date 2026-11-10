// Exercise ordinary-loop bank coverage without pipeline directives.
void unsigned_wrap(unsigned input[256], unsigned output[8], unsigned count)
{
#pragma HLS ARRAY_PARTITION variable=input type=cyclic factor=3 dim=1
  for (unsigned i = 0; i < 8; ++i) {
    unsigned char index = (unsigned char)(252 + 4*i);
    output[i] = input[index];
  }
}
