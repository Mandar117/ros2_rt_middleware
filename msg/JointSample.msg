# Fixed-size joint-state sample (12-DoF arm).
#
# Every field is fixed-size, so the generated C++ type is trivially copyable
# ("plain"). That is the precondition for zero-copy loaned messages: RMWs can
# only loan types whose size is known at compile time.

int64 publish_ns        # publisher steady_clock (CLOCK_MONOTONIC) at publish
uint64 seq              # publisher sequence number (shared with CloudSample)
float64[12] positions   # 96-byte payload
