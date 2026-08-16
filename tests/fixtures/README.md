# Synthetic CM fixtures

These fixtures were constructed from scratch for automated tests. They do not contain data copied from a physical cartridge.

## `lto5_health.bin`

- 4096-byte CM
- Synthetic serial: `12 34 56 78`, BCC `08`
- LTO-5 rewritable cartridge type
- Manufacture date: `2024-01-02`
- Load count: `42`
- Four 96-byte usage pages with thread counts 1 through 4
- The newest page contains deliberately recognizable health counters

## `cleaning_32byte.bin`

- 4096-byte CM
- Synthetic serial: `A1 B2 C3 D4`, BCC `04`
- Universal cleaning cartridge type `0x8001`
- Manufacture date: `2024-02-03`
- 32-byte cartridge status page
- Clean count: `10`
- Tape length: `300 m`
- Used position: `55 m`
- Estimated remaining cleanings under the application heuristic: `42`
