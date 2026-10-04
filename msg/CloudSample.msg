# Fixed-size point-cloud sample: 320 unorganised XYZ float32 points.
#
# Fixed-size so it is loanable (see JointSample.msg). Layout of `data` matches
# a PointCloud2 with fields x, y, z (FLOAT32) and point_step = 12.

int64 publish_ns        # publisher steady_clock (CLOCK_MONOTONIC) at publish
uint64 seq              # publisher sequence number (shared with JointSample)
uint32 width            # points in the cloud (320)
uint32 point_step       # bytes per point (12)
uint8[3840] data        # 320 * 12-byte payload
