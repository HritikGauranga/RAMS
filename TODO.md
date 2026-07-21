# ACK Bug Fix - Progress

## Steps
- [x] Investigate root cause
- [x] Get plan approved
- [x] Edit `src/Modem.cpp` - Change ACK detection from `"ACK "` to `"ACK%"`
- [x] Edit `src/CallManager.cpp` - Fix `CallManager_handleSmsAck()`:
  - [x] Reject empty inputName
  - [x] Parse `DI1`/`AI1` identifiers from `ACK%DI1` format
  - [x] Fall back to name matching
- [ ] Compile and flash
- [ ] Test

