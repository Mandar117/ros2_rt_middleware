# Benchmark summary

## Environment

```
kernel=6.18.33.2-microsoft-standard-WSL2
preempt_rt=no
cpus=4
rmw=rmw_fastrtps_cpp (default)
rlimit_rtprio=0
args=--duration 10 (all executors, modes, rates; separate processes)
```

First 1 s of every run discarded. Latencies in µs.

## One-way message latency

| Scope | Executor | Transport | Rate (Hz) | Message | n | p50 | p99 | p99.9 | max |
|---|---|---|--:|---|--:|--:|--:|--:|--:|
| xproc | rt | Copy | 100 | joint (96 B) | 901 | 194.2 | 301.0 | 417.8 | 444.1 |
| xproc | rt | Copy | 100 | cloud (3840 B) | 901 | 139.7 | 223.4 | 349.4 | 399.3 |
| xproc | rt | Copy | 500 | joint (96 B) | 4,504 | 109.5 | 194.0 | 296.6 | 346.1 |
| xproc | rt | Copy | 500 | cloud (3840 B) | 4,505 | 77.4 | 145.7 | 210.4 | 272.3 |
| xproc | rt | Copy | 1000 | joint (96 B) | 9,009 | 95.9 | 157.8 | 218.7 | 299.1 |
| xproc | rt | Copy | 1000 | cloud (3840 B) | 9,009 | 67.8 | 125.8 | 194.5 | 255.7 |
| xproc | rt | Loaned | 100 | joint (96 B) | 900 | 164.4 | 285.2 | 392.5 | 581.7 |
| xproc | rt | Loaned | 100 | cloud (3840 B) | 901 | 123.0 | 214.9 | 285.2 | 353.9 |
| xproc | rt | Loaned | 500 | joint (96 B) | 4,505 | 108.9 | 194.7 | 299.4 | 416.5 |
| xproc | rt | Loaned | 500 | cloud (3840 B) | 4,506 | 76.5 | 143.3 | 222.5 | 302.8 |
| xproc | rt | Loaned | 1000 | joint (96 B) | 9,004 | 96.7 | 155.1 | 194.0 | 823.9 |
| xproc | rt | Loaned | 1000 | cloud (3840 B) | 9,005 | 67.5 | 125.1 | 163.3 | 809.4 |
| xproc | rt | SHM | 100 | joint (96 B) | 901 | 49.3 | 81.9 | 91.6 | 106.3 |
| xproc | rt | SHM | 100 | cloud (3840 B) | 901 | 24.1 | 54.7 | 58.5 | 59.7 |
| xproc | rt | SHM | 500 | joint (96 B) | 4,506 | 34.1 | 61.3 | 79.6 | 135.5 |
| xproc | rt | SHM | 500 | cloud (3840 B) | 4,506 | 14.5 | 38.6 | 59.1 | 114.1 |
| xproc | rt | SHM | 1000 | joint (96 B) | 9,011 | 32.3 | 58.9 | 95.1 | 117.6 |
| xproc | rt | SHM | 1000 | cloud (3840 B) | 9,011 | 12.8 | 35.4 | 68.8 | 99.2 |
| xproc | stock | Copy | 100 | joint (96 B) | 982 | 145.7 | 276.8 | 505.8 | 1,292.7 |
| xproc | stock | Copy | 100 | cloud (3840 B) | 982 | 110.2 | 200.0 | 333.5 | 1,234.4 |
| xproc | stock | Copy | 500 | joint (96 B) | 4,913 | 105.5 | 176.6 | 240.6 | 270.5 |
| xproc | stock | Copy | 500 | cloud (3840 B) | 4,913 | 75.8 | 143.7 | 185.7 | 242.0 |
| xproc | stock | Copy | 1000 | joint (96 B) | 9,828 | 94.4 | 157.0 | 195.3 | 354.5 |
| xproc | stock | Copy | 1000 | cloud (3840 B) | 9,828 | 68.2 | 127.2 | 170.7 | 327.1 |
| xproc | stock | Loaned | 100 | joint (96 B) | 982 | 167.1 | 332.4 | 418.9 | 603.5 |
| xproc | stock | Loaned | 100 | cloud (3840 B) | 982 | 122.6 | 240.3 | 281.0 | 332.3 |
| xproc | stock | Loaned | 500 | joint (96 B) | 4,879 | 105.6 | 180.7 | 276.5 | 408.6 |
| xproc | stock | Loaned | 500 | cloud (3840 B) | 4,879 | 75.3 | 145.2 | 225.9 | 387.5 |
| xproc | stock | Loaned | 1000 | joint (96 B) | 9,829 | 93.3 | 161.4 | 243.6 | 369.9 |
| xproc | stock | Loaned | 1000 | cloud (3840 B) | 9,829 | 66.2 | 129.5 | 188.9 | 316.4 |
| xproc | stock | SHM | 100 | joint (96 B) | 982 | 50.9 | 92.0 | 173.2 | 241.4 |
| xproc | stock | SHM | 100 | cloud (3840 B) | 982 | 24.4 | 59.7 | 153.5 | 213.9 |
| xproc | stock | SHM | 500 | joint (96 B) | 4,913 | 34.0 | 65.8 | 86.6 | 133.8 |
| xproc | stock | SHM | 500 | cloud (3840 B) | 4,913 | 14.2 | 45.0 | 75.4 | 114.3 |
| xproc | stock | SHM | 1000 | joint (96 B) | 9,828 | 32.3 | 58.1 | 79.9 | 151.5 |
| xproc | stock | SHM | 1000 | cloud (3840 B) | 9,828 | 13.0 | 38.7 | 62.4 | 117.6 |

## Publisher wake-up lateness

| Scope | Executor | Transport | Rate (Hz) | n | p50 | p99 | p99.9 | max |
|---|---|---|--:|--:|--:|--:|--:|--:|
| xproc | rt | Copy | 100 | 901 | 31.8 | 80.8 | 550.1 | 555.0 |
| xproc | rt | Copy | 500 | 4,505 | 16.2 | 46.5 | 135.0 | 246.8 |
| xproc | rt | Copy | 1000 | 9,009 | 22.5 | 47.3 | 95.2 | 210.7 |
| xproc | rt | Loaned | 100 | 901 | 29.4 | 255.7 | 492.6 | 593.5 |
| xproc | rt | Loaned | 500 | 4,506 | 22.2 | 45.3 | 114.7 | 324.2 |
| xproc | rt | Loaned | 1000 | 9,005 | 18.4 | 47.8 | 76.7 | 222.5 |
| xproc | rt | SHM | 100 | 901 | 30.0 | 139.2 | 295.0 | 335.0 |
| xproc | rt | SHM | 500 | 4,506 | 18.2 | 48.2 | 79.1 | 458.7 |
| xproc | rt | SHM | 1000 | 9,013 | 22.7 | 52.3 | 103.2 | 323.8 |
| xproc | stock | Copy | 100 | 992 | 896.1 | 1,026.8 | 1,315.3 | 3,171.2 |
| xproc | stock | Copy | 500 | 4,959 | 239.4 | 278.6 | 315.1 | 520.1 |
| xproc | stock | Copy | 1000 | 9,919 | 154.3 | 185.1 | 226.0 | 303.4 |
| xproc | stock | Loaned | 100 | 992 | 897.7 | 1,072.6 | 1,371.3 | 1,453.8 |
| xproc | stock | Loaned | 500 | 4,960 | 238.9 | 274.0 | 307.5 | 374.3 |
| xproc | stock | Loaned | 1000 | 9,920 | 154.0 | 183.9 | 272.0 | 354.1 |
| xproc | stock | SHM | 100 | 992 | 905.3 | 1,095.0 | 1,297.0 | 1,390.1 |
| xproc | stock | SHM | 500 | 4,961 | 239.9 | 285.6 | 427.8 | 851.7 |
| xproc | stock | SHM | 1000 | 9,924 | 156.4 | 185.1 | 238.9 | 850.7 |
