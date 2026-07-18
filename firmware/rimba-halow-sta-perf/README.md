# rimba-halow-sta-perf

HaLow **station** node for measuring 802.11ah throughput. It associates to the
`rimba-halow-ap-perf` SoftAP over SAE, takes an IP by DHCP, then opens an
`esp_console` REPL on the serial port so you can drive `iperf` (and `ping`) over
the HaLow link and read the numbers back.

This is the **client** half of the throughput pair — flash `rimba-halow-ap-perf`
on the other board to get the `iperf` server.

## Build + flash

```sh
make build APP=rimba-halow-sta-perf BOARD=proto1-fgh100m
make flash APP=rimba-halow-sta-perf BOARD=proto1-fgh100m PORT=/dev/ttyACM0
```

(Use the `PORT=/dev/ttyACMx` that this board enumerates as.)

## Running a throughput test

1. Flash `rimba-halow-ap-perf` on board A and `rimba-halow-sta-perf` on board B.
   Both must use the **same** `LINK_SSID` / `LINK_PSK`.
2. On the AP's serial console, start the server: `iperf -s`.
3. Wait for this STA to associate and log its `DHCP lease ...` line, then on its
   serial console start the client against the AP:

   ```
   iperf> iperf -c 192.168.12.1
   ```

   (`192.168.12.1` is the AP address — the `AP_IP` default below.) Add `-u` for a
   UDP test; the `iperf` command help lists the rest of the flags.

## What to change for your own network

Edit the `#define`s at the top of `main/app_main.c`:

- `LINK_SSID` / `LINK_PSK` — the HaLow network name and passphrase. **These must
  match `rimba-halow-ap-perf`'s** or association will fail (the PSK must be ≥ 8
  characters for SAE).
- `AP_IP` — the iperf server address (the AP). Only used in the console banner;
  change it to whatever address the AP hands out / listens on.

The STA takes its address by DHCP, so no client IP is hard-coded here.
