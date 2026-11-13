static void producer(float a[16], float stage[16], float scale)
{
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        stage[i] = a[i] * scale;
    }
}

static void consumer(float stage[16], float out[16])
{
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        out[i] = __builtin_sqrtf(stage[i]) / 3.0f;
    }
}

void dut(float a[16], float out[16], float scale)
{
    float stage[16];
#pragma HLS DATAFLOW
    producer(a, stage, scale);
    consumer(stage, out);
}
