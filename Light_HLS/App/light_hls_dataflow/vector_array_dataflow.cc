static const int N = 64;

static void load_stage(const int input[N], int values[N])
{
    for (int i = 0; i < N; ++i)
    {
        values[i] = input[i];
    }
}

static void compute_stage(const int input[N], int output[N])
{
    for (int i = 0; i < N; ++i)
    {
        output[i] = input[i] * 3 + 1;
    }
}

static void store_stage(const int values[N], int output[N])
{
    for (int i = 0; i < N; ++i)
    {
        output[i] = values[i];
    }
}

void vector_array_dataflow(const int input[N], int output[N])
{
    int loaded[N];
    int computed[N];

#pragma HLS DATAFLOW
    load_stage(input, loaded);
    compute_stage(loaded, computed);
    store_stage(computed, output);
}
