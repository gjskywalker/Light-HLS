# Dual-port ap_memory backend test

The top function performs two independent reads from one external array. The
single-port run must serialize the requests and report a port conflict. The
configured two-port run binds both requests to the same cycle on ports zero and
one and is checked with a synchronous dual-port Icarus memory model.
