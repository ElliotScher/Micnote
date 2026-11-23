# BLE in BabbleSim

The puck's Bluetooth side, run with no hardware: BabbleSim simulates the radio channel and Nordic's hardware models simulate the SoC, so the real Zephyr Bluetooth stack and Nordic's SoftDevice Controller run as ordinary Linux processes. `link/` is one test image with three roles:

| Role | Device | What it does |
|---|---|---|
| `puck` | 0 | The firmware's BLE module (`app/src/ble/`), dispatcher, host slots, fallback key and recording store on LittleFS. Two memos wait on the fallback key. Stand-ins for the hardware: the pairing window is open while no host is paired, and the button is pressed 300 ms after a host confirms. |
| `host` | 1 | Finds the puck by its advertising (`sync_pending` set), bonds (LE Secure Connections), reads the PSM, opens the L2CAP channel, pairs at the app layer, checks the memos were moved to it, fetches, decrypts and acks both. Checks the 15 ms transfer interval. Reconnects with the bond and authenticates with the link key, checks the status characteristic and the idle interval, then does the same over the GATT fallback. |
| `stranger` | 2 | Turns up after the pairing: must be refused a bond and every characteristic. |

It runs on `nrf52_bsim`, the single-core simulated nRF52833: the puck code only uses Zephyr's host API, which is the same on the nRF5340, and the simulated nRF5340 needs a second (network core) image per device. Throughput printed here is simulated, not a measurement (that's stage 1 PR 7, on the boards).

## Setup (once)

`firmware/west.yml` fetches BabbleSim and the nRF hardware models. Build BabbleSim:

```sh
export BSIM_OUT_PATH=$PWD/external/tools/bsim
export BSIM_COMPONENTS_PATH=$BSIM_OUT_PATH/components
make -C "$BSIM_OUT_PATH" everything -j"$(nproc)"
```

## Run

```sh
python3 firmware/bsim/run.py              # build for nrf52_bsim, then run all three
python3 firmware/bsim/run.py --no-build   # run the last build
python3 firmware/bsim/run.py --keep DIR   # keep the device logs
```

Each role ends with `Test end: ...`; `run.py` fails unless all three exit 0, and prints the last lines of any that didn't. CI runs it as the firmware workflow's `bsim` job.

## Findings it turned up

- **Mbed TLS heap.** With Bluetooth enabled, Mbed TLS allocates from its own 2 KB heap, too small for X25519 (`PSA_ERROR_INSUFFICIENT_MEMORY`). The test sets `CONFIG_MBEDTLS_HEAP_SIZE=8192`; the board builds need the same once BLE joins the app. native_sim uses the host's `malloc` and never showed it.
- **Connection-parameter hold.** Zephyr holds a peripheral's parameter requests back for `CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT` (5 s by default). The puck uses 1 s, so a sync switches to 15 ms almost at once.
- **The receiver sets the pace.** Zephyr's default L2CAP receive path caps the MTU at one PDU (245 bytes) and grants one credit at a time. The test host uses `seg_recv` with a 2048-byte MTU and 32 credits, as a phone would. With those, the puck sustains about 100 KB/s simulated, against §4.7's 50 KB/s target.
