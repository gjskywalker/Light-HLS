// Exercise ordinary-loop bank coverage without pipeline directives.
void mixed_hazards(unsigned input[256], unsigned output[8], unsigned count)
{
#pragma HLS ARRAY_PARTITION variable=input type=cyclic factor=4 dim=1
  unsigned lane = count & 3;
  for (unsigned i = 0; i < 8; ++i) {
    unsigned x = input[4*i + lane];
    input[4*i] = x + 5;
    unsigned y = input[4*i + lane];
    input[4*i+1] = y + 7;
    output[i] = x + y;
  }
}
