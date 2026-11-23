# OTNS Thread timing (generated)

`tools/otns/thread_latency.py`, 5 seeded runs, OpenThread in OTNS (simulated radio). **Simulation, not a measurement:** real radios, walls and the ESP32-H2's own boot time add to it.

| Quantity | seed 1 | seed 2 | seed 3 | seed 4 | seed 5 | worst |
|---|---:|---:|---:|---:|---:|---:|
| cold_attach_s | 0.950 | 1.400 | 1.000 | 1.400 | 1.400 | 1.400 |
| wake_attach_s | 0.150 | 0.150 | 0.150 | 0.150 | 0.150 | 0.150 |
| wake_first_reply_s | 0.330 | 0.230 | 0.230 | 0.230 | 0.230 | 0.330 |
| rtt_sleepy_child_s | 0.159 | 0.107 | 0.012 | 0.011 | 0.109 | 0.159 |
| rtt_always_on_s | 0.008 | 0.018 | 0.008 | 0.009 | 0.018 | 0.018 |
| parent_loss_reattach_s | 4.000 | 1.750 | - | - | 1.500 | 4.000 |

- `-` for parent loss: in that run the SED's parent was the border router itself, which the harness never removes (it stands in for the hub).
- Deep-sleep budget (wake to first reply) 2.0 s: **PASS**
- Wall time 4 s.
