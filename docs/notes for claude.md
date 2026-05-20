Use `BLE_CHUNKED_TRACE_PROTOCOL.md` as the source of truth for the current firmware protocol.

Update the Flutter BLE layer so that it:
1. Treats the phone as BLE Central / GATT Client.
2. Connects to the firmware device and discovers Nordic UART Service.
3. Subscribes to NUS TX notifications before sending any command.
4. Writes command fragments sequentially to NUS RX.
5. Waits for `FRAG_OK` after each non-final fragment.
6. Sends the final fragment with `#` as the command terminator.
7. Parses notifications as a stream:
   - `T_BEGIN` clears the current trace buffer.
   - `T=...` appends trace values.
   - `T_END` marks the trace complete.
8. Does not use BLE characteristic read as the trace source.
9. Adds timeouts for `FRAG_OK`, `T_BEGIN`, and `T_END`.
10. Keeps the command format:
    `START=<startVoltageMv>;END=<endVoltageMv>;FREQ=<frequencyHz>;RANGE=<currentRangeNa>`