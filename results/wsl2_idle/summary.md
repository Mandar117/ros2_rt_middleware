# Benchmark summary

## Environment

```
kernel=6.18.33.2-microsoft-standard-WSL2
kernel_version=#1 SMP PREEMPT_DYNAMIC Thu Jun 18 21:54:43 UTC 2026
machine=x86_64
preempt_rt=no
cpus=4
rmw=rmw_fastrtps_cpp
rlimit_rtprio=0
rlimit_memlock=67108864
cpu_governor=
isolcpus=no
clock_raw_vs_mono_ppm=90906
args=--duration 10 --results-dir /home/mandy/bench/wsl2_idle
```

First 1 s of every run discarded. Latencies in µs.

## One-way message latency

| Scope | Executor | Transport | Rate (Hz) | Message | n | p50 | p99 | p99.9 | max |
|---|---|---|--:|---|--:|--:|--:|--:|--:|
| inproc | rt | Copy | 100 | joint (96 B) | 901 | 117.6 | 225.9 | 292.0 | 329.2 |
| inproc | rt | Copy | 100 | cloud (3840 B) | 901 | 67.9 | 143.7 | 183.2 | 198.1 |
| inproc | rt | Copy | 500 | joint (96 B) | 4,507 | 55.0 | 114.1 | 145.5 | 267.9 |
| inproc | rt | Copy | 500 | cloud (3840 B) | 4,508 | 27.6 | 76.0 | 117.4 | 238.1 |
| inproc | rt | Copy | 1000 | joint (96 B) | 9,013 | 52.2 | 103.9 | 133.2 | 169.8 |
| inproc | rt | Copy | 1000 | cloud (3840 B) | 9,014 | 26.0 | 66.8 | 98.7 | 123.2 |
| inproc | rt | Loaned | 100 | joint (96 B) | 900 | 103.5 | 210.5 | 258.4 | 275.1 |
| inproc | rt | Loaned | 100 | cloud (3840 B) | 900 | 62.3 | 138.7 | 157.8 | 159.8 |
| inproc | rt | Loaned | 500 | joint (96 B) | 4,506 | 55.2 | 121.2 | 153.5 | 225.9 |
| inproc | rt | Loaned | 500 | cloud (3840 B) | 4,506 | 26.4 | 86.7 | 106.5 | 201.3 |
| inproc | rt | Loaned | 1000 | joint (96 B) | 9,013 | 52.7 | 126.0 | 170.8 | 518.6 |
| inproc | rt | Loaned | 1000 | cloud (3840 B) | 9,014 | 26.1 | 92.2 | 118.8 | 509.3 |
| inproc | rt | SHM | 100 | joint (96 B) | 899 | 51.8 | 87.9 | 130.7 | 386.3 |
| inproc | rt | SHM | 100 | cloud (3840 B) | 900 | 26.5 | 53.4 | 106.8 | 365.3 |
| inproc | rt | SHM | 500 | joint (96 B) | 4,506 | 34.0 | 60.1 | 77.2 | 139.0 |
| inproc | rt | SHM | 500 | cloud (3840 B) | 4,507 | 14.2 | 38.1 | 55.4 | 64.7 |
| inproc | rt | SHM | 1000 | joint (96 B) | 9,013 | 32.2 | 54.9 | 85.6 | 136.2 |
| inproc | rt | SHM | 1000 | cloud (3840 B) | 9,014 | 12.8 | 32.4 | 66.5 | 141.6 |
| inproc | stock | Copy | 100 | joint (96 B) | 982 | 81.8 | 374.3 | 1,291.2 | 1,487.8 |
| inproc | stock | Copy | 100 | cloud (3840 B) | 982 | 49.1 | 195.3 | 582.4 | 1,342.4 |
| inproc | stock | Copy | 500 | joint (96 B) | 4,917 | 54.8 | 117.1 | 172.8 | 438.3 |
| inproc | stock | Copy | 500 | cloud (3840 B) | 4,917 | 28.0 | 84.0 | 134.7 | 400.0 |
| inproc | stock | Copy | 1000 | joint (96 B) | 9,832 | 51.0 | 105.2 | 142.8 | 298.0 |
| inproc | stock | Copy | 1000 | cloud (3840 B) | 9,832 | 26.1 | 77.3 | 115.1 | 272.4 |
| inproc | stock | Loaned | 100 | joint (96 B) | 981 | 95.3 | 176.1 | 254.6 | 294.5 |
| inproc | stock | Loaned | 100 | cloud (3840 B) | 981 | 52.0 | 113.8 | 157.5 | 176.7 |
| inproc | stock | Loaned | 500 | joint (96 B) | 4,917 | 53.8 | 129.8 | 177.4 | 265.5 |
| inproc | stock | Loaned | 500 | cloud (3840 B) | 4,917 | 26.8 | 94.4 | 136.8 | 165.2 |
| inproc | stock | Loaned | 1000 | joint (96 B) | 9,835 | 50.8 | 109.3 | 175.3 | 331.9 |
| inproc | stock | Loaned | 1000 | cloud (3840 B) | 9,835 | 25.4 | 80.1 | 128.1 | 300.6 |
| inproc | stock | SHM | 100 | joint (96 B) | 982 | 50.6 | 81.7 | 122.6 | 297.3 |
| inproc | stock | SHM | 100 | cloud (3840 B) | 982 | 27.4 | 58.3 | 100.1 | 269.3 |
| inproc | stock | SHM | 500 | joint (96 B) | 4,916 | 33.7 | 58.6 | 77.6 | 106.4 |
| inproc | stock | SHM | 500 | cloud (3840 B) | 4,916 | 14.0 | 35.9 | 56.5 | 87.4 |
| inproc | stock | SHM | 1000 | joint (96 B) | 9,832 | 32.1 | 58.6 | 75.4 | 196.3 |
| inproc | stock | SHM | 1000 | cloud (3840 B) | 9,832 | 12.6 | 37.3 | 55.5 | 180.1 |

## Publisher wake-up lateness

| Scope | Executor | Transport | Rate (Hz) | n | p50 | p99 | p99.9 | max |
|---|---|---|--:|--:|--:|--:|--:|--:|
| inproc | rt | Copy | 100 | 901 | 31.6 | 76.9 | 290.9 | 497.5 |
| inproc | rt | Copy | 500 | 4,508 | 20.4 | 50.2 | 301.0 | 588.9 |
| inproc | rt | Copy | 1000 | 9,014 | 19.4 | 49.2 | 111.1 | 218.9 |
| inproc | rt | Loaned | 100 | 900 | 30.9 | 71.3 | 370.5 | 695.7 |
| inproc | rt | Loaned | 500 | 4,506 | 18.6 | 112.1 | 469.9 | 672.3 |
| inproc | rt | Loaned | 1000 | 9,014 | 22.5 | 52.6 | 100.2 | 140.0 |
| inproc | rt | SHM | 100 | 900 | 31.9 | 115.5 | 360.5 | 411.5 |
| inproc | rt | SHM | 500 | 4,508 | 18.1 | 52.7 | 253.8 | 555.6 |
| inproc | rt | SHM | 1000 | 9,014 | 22.2 | 46.6 | 96.7 | 187.3 |
| inproc | stock | Copy | 100 | 992 | 897.8 | 1,185.8 | 1,446.9 | 1,471.4 |
| inproc | stock | Copy | 500 | 4,963 | 238.2 | 288.1 | 455.4 | 704.2 |
| inproc | stock | Copy | 1000 | 9,923 | 153.5 | 182.5 | 215.3 | 305.0 |
| inproc | stock | Loaned | 100 | 991 | 892.1 | 978.7 | 1,277.4 | 1,387.1 |
| inproc | stock | Loaned | 500 | 4,963 | 237.3 | 276.5 | 307.0 | 493.2 |
| inproc | stock | Loaned | 1000 | 9,926 | 153.2 | 183.2 | 277.6 | 677.7 |
| inproc | stock | SHM | 100 | 992 | 901.1 | 1,003.8 | 1,603.7 | 1,653.3 |
| inproc | stock | SHM | 500 | 4,963 | 239.5 | 293.8 | 476.0 | 850.6 |
| inproc | stock | SHM | 1000 | 9,926 | 155.4 | 185.9 | 261.9 | 442.0 |

## Run health

| Executor | Transport | Rate (Hz) | Achieved (Hz) | Published | Received | Lost | Logger drops | Loans (pub/sub) | RT sched applied | Missed releases |
|---|---|--:|--:|--:|--:|--:|--:|---|---|--:|
| stock | Copy | 100 | 109.0 | 1,092 | 2,184 | 0 | 0 | no/no | n/a | 0 |
| stock | Copy | 500 | 545.4 | 5,463 | 10,926 | 0 | 0 | no/no | n/a | 0 |
| stock | Copy | 1000 | 1,090.9 | 10,923 | 21,846 | 0 | 0 | no/no | n/a | 0 |
| stock | Loaned | 100 | 109.0 | 1,091 | 2,182 | 0 | 0 | no/no | n/a | 0 |
| stock | Loaned | 500 | 545.4 | 5,463 | 10,926 | 0 | 0 | no/no | n/a | 0 |
| stock | Loaned | 1000 | 1,090.9 | 10,926 | 21,852 | 0 | 0 | no/no | n/a | 0 |
| stock | SHM | 100 | 109.0 | 1,092 | 2,184 | 0 | 0 | no/no | n/a | 0 |
| stock | SHM | 500 | 545.4 | 5,463 | 10,924 | 0 | 0 | no/no | n/a | 0 |
| stock | SHM | 1000 | 1,090.8 | 10,926 | 21,846 | 0 | 0 | no/no | n/a | 0 |
| rt | Copy | 100 | 99.9 | 1,001 | 2,002 | 0 | 0 | no/no | no | 0 |
| rt | Copy | 500 | 499.9 | 5,008 | 10,016 | 0 | 0 | no/no | no | 0 |
| rt | Copy | 1000 | 999.9 | 10,014 | 20,028 | 0 | 0 | no/no | no | 0 |
| rt | Loaned | 100 | 99.9 | 1,000 | 2,000 | 0 | 0 | no/no | no | 0 |
| rt | Loaned | 500 | 499.9 | 5,006 | 10,012 | 0 | 0 | no/no | no | 0 |
| rt | Loaned | 1000 | 999.9 | 10,014 | 20,028 | 0 | 0 | no/no | no | 0 |
| rt | SHM | 100 | 99.9 | 1,000 | 2,000 | 0 | 0 | no/no | no | 0 |
| rt | SHM | 500 | 499.9 | 5,008 | 10,014 | 0 | 0 | no/no | no | 0 |
| rt | SHM | 1000 | 999.7 | 10,014 | 20,024 | 0 | 0 | no/no | no | 3 |
