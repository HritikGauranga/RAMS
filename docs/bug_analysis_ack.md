# ACK Bug Analysis

## Root Cause

The root cause is in `CallManager_handleSmsAck()` in `CallManager.cpp`.

### Bug: Empty input name matches all inputs

When bare `ACK` is sent (without an input identifier), `inputName` is empty string `""`.
Since all unconfigured inputs have empty name fields (zero-initialized to `""`), the condition:

```cpp
if (String(cfg.name).equalsIgnoreCase(inputName))
```

evaluates to `"".equalsIgnoreCase("")` which is `true` for EVERY input, causing ALL active alarms to be acknowledged.

### Contributing Issue: No input identifier matching

The function only matches by configured `name` field (e.g., "Main Door"), NOT by input identifier (e.g., `DI1`, `AI1`). The `parseInputIdentifier()` function exists in Modem.cpp but is not used in CallManager.

### Contributing Issue: SMS parser expects space, not %

The SMS parser checks `startsWith("ACK ")` but the command format uses `ACK%<Input>`.

## Fix Plan

1. In `CallManager_handleSmsAck()`:
   - Reject empty `inputName` (fixes the bare `ACK` → all-acknowledged bug)
   - First try to match by input identifier (`DI1`, `AI1`, etc.)
   - Then fall back to matching by configured name

