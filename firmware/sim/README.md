# Puck simulator

The firmware's real recording store, crypto and protocol dispatcher, built for `native_sim` (Zephyr as a Linux program). It serves the control protocol on a pseudo-terminal, so `micnote-cli` can sync from it exactly as it would from a puck over USB. No hardware needed.

What it does at boot:
1. Mounts the recording store on the simulated flash. The flash is the file `flash.bin` in the directory you run it from, so recordings survive restarts. Pass `--flash_erase` to start empty.
2. Recovers any recording a previous run left half-written, as the puck does after a power cut.
3. If the store is empty, records two demo memos (5 s and 12 s, with item markers): a synthetic voice through the firmware's real encoder thread (`app/src/audio/`), so they are real Opus, encrypted to the test-vector host key `host_a`.
4. Serves the protocol on `uart_1`, and prints which pseudo-terminal that is.

Until pairing exists (stage 1 PR 5), the only paired host is `host_a`, so the CLI uses that key.

## Run it

From the repo root, in the firmware environment:

```sh
west build -b native_sim firmware/sim -d build/sim
cmake --preset dev && cmake --build --preset dev     # for micnote-cli

mkdir -p /tmp/puck && cd /tmp/puck
# host_a's private key, from the test vectors:
python3 -c "import json; open('host_a.key','wb').write(bytes.fromhex(json.load(open('$OLDPWD/proto/testvectors/vectors.json'))['keys']['host_a']['priv']))"

$OLDPWD/build/sim/sim/zephyr/zephyr.exe
#   uart_1 connected to pseudotty: /dev/pts/4      <- this one
#   sim: serving the Micnote protocol on uart_1
```

In another terminal (the dev preset puts `micnote-cli` at `build/dev/core/bin/Debug/micnote-cli`):

```sh
micnote-cli status --usb /dev/pts/4 --key /tmp/puck/host_a.key
micnote-cli list   --usb /dev/pts/4 --key /tmp/puck/host_a.key
micnote-cli sync   --usb /dev/pts/4 --key /tmp/puck/host_a.key --out /tmp/puck/out
```

`sync` fetches each recording, checks it decrypts, writes `<id>.mnr` and a playable `<id>.opus`, and acks it; the simulator then deletes it, so a second `list` shows nothing. Restart with `--flash_erase` to get the demo memos back.

## Automated check

`e2e.py` does all of the above and more without a second terminal: it boots the simulator on an empty flash, runs `status`, `list` and `sync --no-ack`, restarts the simulator on the same flash to check the memos survived, then syncs with ack and checks the puck is empty. CI runs it as the firmware workflow's `sim-e2e` job.

```sh
python3 firmware/sim/e2e.py --sim build/sim/sim/zephyr/zephyr.exe \
    --cli build/dev/core/bin/Debug/micnote-cli [--keep DIR]
```

`--keep DIR` copies the synced `.mnr` and `.opus` files out, to listen to.

## Notes

- The link uses `uart_fifo_fill()`, which on native_sim reports how many bytes the pseudo-terminal accepted. `poll_out` would silently drop bytes whenever the host reads slowly.
- `list` shows no durations or markers: they're in each recording's encrypted trailer, which the puck can't read. `sync` shows them after decrypting.
- The board is `native_sim` only. On the puck, the same `dispatch.c` will sit behind USB CDC and BLE (stage 1 PR 5).
