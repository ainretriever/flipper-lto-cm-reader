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

## `lto5_uninitialized.bin`

- Synthetic 8160-byte CM with a factory-style directory modeled on the supplied blank tape layout
- Uses the synthetic LTO-5 identity and manufacture date from `lto5_health.bin`
- Application page at `0x1A0`, version-3 status page at `0x5C0`, zero loads
- No initialisation or usage pages; unused directory slots point to `0x600`
- Contains no cartridge or drive identifiers copied from physical media
