// Check single-writer payload binding without changing memory events.
void external_only(unsigned input[32], unsigned output[16], unsigned seed)
{
  for (unsigned i = 0; i < 16; ++i)
    output[i] = input[i] + seed;
}
