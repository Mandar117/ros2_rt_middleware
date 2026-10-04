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
args=--duration 10 --load 4 --results-dir /home/mandy/bench/wsl2_load4
```

First 1 s of every run discarded. Latencies in µs.

## One-way message latency

| Scope | Executor | Transport | Rate (Hz) | Message | n | p50 | p99 | p99.9 | max |
|---|---|---|--:|---|--:|--:|--:|--:|--:|
| inproc | rt | Copy | 100 | joint (96 B) | 900 | 323.4 | 6,050.1 | 6,226.2 | 6,227.5 |
| inproc | rt | Copy | 100 | cloud (3840 B) | 900 | 158.3 | 5,486.0 | 5,841.6 | 5,842.3 |
| inproc | rt | Copy | 500 | joint (96 B) | 4,457 | 109.0 | 4,070.4 | 9,047.1 | 10,000.7 |
| inproc | rt | Copy | 500 | cloud (3840 B) | 4,457 | 67.9 | 4,036.2 | 9,016.1 | 10,026.1 |
| inproc | rt | Copy | 1000 | joint (96 B) | 8,804 | 76.3 | 3,932.1 | 5,275.6 | 6,480.0 |
| inproc | rt | Copy | 1000 | cloud (3840 B) | 8,805 | 49.0 | 3,920.8 | 5,887.2 | 6,466.3 |
| inproc | rt | Loaned | 100 | joint (96 B) | 901 | 308.5 | 6,086.6 | 6,162.0 | 6,174.3 |
| inproc | rt | Loaned | 100 | cloud (3840 B) | 901 | 132.7 | 3,923.7 | 5,778.8 | 5,846.0 |
| inproc | rt | Loaned | 500 | joint (96 B) | 4,315 | 101.1 | 4,250.7 | 6,261.7 | 9,919.5 |
| inproc | rt | Loaned | 500 | cloud (3840 B) | 4,315 | 47.3 | 2,143.3 | 6,035.8 | 9,899.5 |
| inproc | rt | Loaned | 1000 | joint (96 B) | 8,820 | 73.3 | 3,204.4 | 5,018.9 | 6,314.3 |
| inproc | rt | Loaned | 1000 | cloud (3840 B) | 8,820 | 46.6 | 3,893.3 | 5,069.4 | 6,341.8 |
| inproc | rt | SHM | 100 | joint (96 B) | 898 | 34.8 | 92.0 | 167.5 | 168.1 |
| inproc | rt | SHM | 100 | cloud (3840 B) | 898 | 9.4 | 100.5 | 170.4 | 212.8 |
| inproc | rt | SHM | 500 | joint (96 B) | 4,449 | 14.9 | 61.7 | 142.2 | 356.5 |
| inproc | rt | SHM | 500 | cloud (3840 B) | 4,449 | 8.0 | 52.9 | 123.6 | 365.3 |
| inproc | rt | SHM | 1000 | joint (96 B) | 8,595 | 8.3 | 41.8 | 96.1 | 399.8 |
| inproc | rt | SHM | 1000 | cloud (3840 B) | 8,595 | 7.2 | 48.0 | 111.1 | 264.2 |
| inproc | stock | Copy | 100 | joint (96 B) | 982 | 258.6 | 6,177.2 | 6,648.6 | 8,694.6 |
| inproc | stock | Copy | 100 | cloud (3840 B) | 982 | 112.6 | 4,444.5 | 6,050.4 | 8,616.8 |
| inproc | stock | Copy | 500 | joint (96 B) | 4,835 | 95.8 | 3,822.2 | 6,154.3 | 14,406.9 |
| inproc | stock | Copy | 500 | cloud (3840 B) | 4,835 | 61.7 | 3,656.2 | 5,845.6 | 13,803.4 |
| inproc | stock | Copy | 1000 | joint (96 B) | 9,551 | 69.3 | 3,645.3 | 5,472.2 | 6,574.4 |
| inproc | stock | Copy | 1000 | cloud (3840 B) | 9,551 | 45.6 | 3,692.9 | 5,508.1 | 6,554.1 |
| inproc | stock | Loaned | 100 | joint (96 B) | 981 | 262.3 | 5,462.9 | 6,092.1 | 6,311.4 |
| inproc | stock | Loaned | 100 | cloud (3840 B) | 981 | 129.1 | 4,386.7 | 5,149.1 | 5,818.0 |
| inproc | stock | Loaned | 500 | joint (96 B) | 4,848 | 90.9 | 2,951.8 | 5,944.9 | 7,575.2 |
| inproc | stock | Loaned | 500 | cloud (3840 B) | 4,848 | 58.1 | 3,071.8 | 5,913.0 | 7,521.9 |
| inproc | stock | Loaned | 1000 | joint (96 B) | 9,522 | 68.7 | 3,472.9 | 5,345.5 | 6,905.4 |
| inproc | stock | Loaned | 1000 | cloud (3840 B) | 9,522 | 46.1 | 3,523.0 | 5,315.9 | 6,892.6 |
| inproc | stock | SHM | 100 | joint (96 B) | 982 | 28.1 | 103.3 | 171.6 | 194.3 |
| inproc | stock | SHM | 100 | cloud (3840 B) | 982 | 8.5 | 230.4 | 328.1 | 370.2 |
| inproc | stock | SHM | 500 | joint (96 B) | 4,830 | 28.0 | 130.0 | 3,074.1 | 3,883.0 |
| inproc | stock | SHM | 500 | cloud (3840 B) | 4,830 | 18.8 | 189.0 | 3,290.1 | 5,346.6 |
| inproc | stock | SHM | 1000 | joint (96 B) | 9,574 | 22.4 | 201.8 | 3,833.5 | 5,554.7 |
| inproc | stock | SHM | 1000 | cloud (3840 B) | 9,574 | 14.4 | 224.4 | 3,836.6 | 5,555.5 |

## Publisher wake-up lateness

| Scope | Executor | Transport | Rate (Hz) | n | p50 | p99 | p99.9 | max |
|---|---|---|--:|--:|--:|--:|--:|--:|
| inproc | rt | Copy | 100 | 900 | 36.6 | 3,796.8 | 3,831.3 | 3,865.8 |
| inproc | rt | Copy | 500 | 4,453 | 17.9 | 1,962.6 | 3,997.7 | 4,029.6 |
| inproc | rt | Copy | 1000 | 8,784 | 14.4 | 632.4 | 3,972.0 | 5,238.8 |
| inproc | rt | Loaned | 100 | 901 | 34.9 | 3,811.0 | 3,854.5 | 4,020.7 |
| inproc | rt | Loaned | 500 | 4,310 | 18.9 | 3,994.1 | 7,835.6 | 7,868.3 |
| inproc | rt | Loaned | 1000 | 8,803 | 14.3 | 238.6 | 3,958.4 | 5,133.9 |
| inproc | rt | SHM | 100 | 899 | 31.0 | 517.5 | 996.9 | 1,004.6 |
| inproc | rt | SHM | 500 | 4,447 | 21.1 | 3,174.5 | 3,223.7 | 3,342.1 |
| inproc | rt | SHM | 1000 | 8,567 | 15.3 | 2,656.4 | 4,658.5 | 4,738.1 |
| inproc | stock | Copy | 100 | 992 | 976.3 | 4,897.6 | 5,258.5 | 5,272.1 |
| inproc | stock | Copy | 500 | 4,873 | 243.5 | 3,172.0 | 4,836.5 | 10,216.8 |
| inproc | stock | Copy | 1000 | 9,602 | 154.0 | 503.8 | 4,478.3 | 5,663.6 |
| inproc | stock | Loaned | 100 | 991 | 985.3 | 5,189.4 | 6,766.9 | 7,198.5 |
| inproc | stock | Loaned | 500 | 4,886 | 243.9 | 1,979.9 | 4,726.7 | 6,284.2 |
| inproc | stock | Loaned | 1000 | 9,578 | 155.1 | 700.3 | 4,611.9 | 5,684.3 |
| inproc | stock | SHM | 100 | 992 | 996.3 | 4,269.0 | 5,129.4 | 5,134.4 |
| inproc | stock | SHM | 500 | 4,861 | 250.8 | 2,839.1 | 4,716.6 | 6,437.2 |
| inproc | stock | SHM | 1000 | 9,608 | 154.8 | 437.5 | 4,782.7 | 6,131.5 |

## Run health

| Executor | Transport | Rate (Hz) | Achieved (Hz) | Published | Received | Lost | Logger drops | Loans (pub/sub) | RT sched applied | Missed releases |
|---|---|--:|--:|--:|--:|--:|--:|---|---|--:|
| stock | Copy | 100 | 109.1 | 1,092 | 2,184 | 0 | 0 | no/no | n/a | 0 |
| stock | Copy | 500 | 536.7 | 5,373 | 10,746 | 0 | 0 | no/no | n/a | 157 |
| stock | Copy | 1000 | 1,058.3 | 10,602 | 21,204 | 0 | 0 | no/no | n/a | 430 |
| stock | Loaned | 100 | 108.9 | 1,091 | 2,182 | 0 | 0 | no/no | n/a | 0 |
| stock | Loaned | 500 | 538.5 | 5,386 | 10,772 | 0 | 0 | no/no | n/a | 125 |
| stock | Loaned | 1000 | 1,055.9 | 10,578 | 21,156 | 0 | 0 | no/no | n/a | 455 |
| stock | SHM | 100 | 109.0 | 1,092 | 2,184 | 0 | 0 | no/no | n/a | 0 |
| stock | SHM | 500 | 535.9 | 5,361 | 10,718 | 0 | 0 | no/no | n/a | 164 |
| stock | SHM | 1000 | 1,058.9 | 10,608 | 21,214 | 0 | 0 | no/no | n/a | 430 |
| rt | Copy | 100 | 99.9 | 1,000 | 2,000 | 0 | 0 | no/no | no | 0 |
| rt | Copy | 500 | 494.5 | 4,953 | 9,906 | 0 | 0 | no/no | no | 98 |
| rt | Copy | 1000 | 976.9 | 9,784 | 19,568 | 0 | 0 | no/no | no | 319 |
| rt | Loaned | 100 | 99.9 | 1,001 | 2,002 | 0 | 0 | no/no | no | 0 |
| rt | Loaned | 500 | 480.5 | 4,810 | 9,620 | 0 | 0 | no/no | no | 333 |
| rt | Loaned | 1000 | 978.3 | 9,803 | 19,606 | 0 | 0 | no/no | no | 293 |
| rt | SHM | 100 | 99.8 | 999 | 1,998 | 0 | 0 | no/no | no | 0 |
| rt | SHM | 500 | 494.4 | 4,947 | 9,894 | 0 | 0 | no/no | no | 110 |
| rt | SHM | 1000 | 956.5 | 9,567 | 19,134 | 0 | 0 | no/no | no | 624 |
