# HowTo: replay EVIO files through EJFAT, with no load balancer

End-to-end loopback test of the whole chain — file reader → packetizer → UDP →
reassembler → EVIO statistics — with **no control plane and no load balancer**.

Everything below runs on one host. Two terminals.

---

## 1. Build

##########  For MacOS. This will automatically export PKG_CONFIG_PATH
- conda create -n e2sar \
  --override-channels \
  -c ibaldin \
  -c conda-forge \
  --strict-channel-priority \
  python=3.11 \
  e2sar

- conda env config vars set \
  PKG_CONFIG_PATH="$CONDA_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"


- conda deactivate
- conda activate e2sar

- conda install \
  --override-channels \
  -c ibaldin \
  -c conda-forge \
  --strict-channel-priority \
  "libboost-devel=1.89"

- pkg-config --modversion e2sar
- conda list | grep -E 'boost|e2sar'

#####################


Both programs need E2SAR. Put its `e2sar.pc` on the pkg-config path first:

```bash
export PKG_CONFIG_PATH=/path/to/e2sar/lib/pkgconfig:$PKG_CONFIG_PATH

cmake -S cpp -B build
cmake --build build -j
```

Confirm the configure summary says `EJFAT sending : ON`. If it says `OFF`,
pkg-config did not find E2SAR and `build/evio_ejfat_recv` will not exist.

---

## 2. How bypassing the load balancer works

Omitting `--withcp` on both sides is all it takes, but two details follow from
it and you have to get both right or nothing arrives:

1. **The sender writes straight to the URI's `data=` address.** No control
   plane, no sync packets, no LB in the path. So the sender's URI needs
   `data=<ip>:<port>` — **with** a port. The receiver's URI needs `data=<ip>`
   — **without** one; its port comes from `--recv-port`.

2. **The LB header stays on the wire.** Normally the load balancer consumes the
   16-byte LB header the segmenter prepends. With no LB, nothing strips it, so
   the receiver must be told to expect it. `evio_ejfat_recv` does this for you:
   it sets `withLBHeader = !useCP`, the same rule `ejfat_receiver_actor.cpp`
   uses.

The rest of the URI (`useless@…`, `/lb/1`, `sync=…`) is required by the parser
but unused when the control plane is off. The values below are the ones from
E2SAR's own no-CP test.

---

## 3. Start the receiver

**Terminal 1**, first — so it is listening before the sender starts:

```bash
./build/evio_ejfat_recv \
    --uri 'ejfat://useless@127.0.0.1:9876/lb/1?sync=127.0.0.1:12345&data=127.0.0.1' \
    --recv-ip 127.0.0.1 \
    --recv-port 10000 \
    --recv-threads 1 \
    --event-timeout 500 \
    --stats-interval 5
```

It prints its listening ports and then waits:

```text
[10:02:11.004] INFO  listening on 127.0.0.1 ports 10000:10000 with 1 thread(s),
                     control plane off (expecting the LB header on the wire)
[10:02:11.004] INFO  waiting for events; press Ctrl-C to stop
```

---

## 4. Start the sender

**Terminal 2**. Note `data=127.0.0.1:10000` — the port matches `--recv-port`
above:

```bash
./build/evio_ejfat_replay \
    --file-count 2 \
    --file data/evio_192.168.0.13.bin \
    --file data/evio_192.168.0.16.bin \
    --uri 'ejfat://useless@127.0.0.1:9876/lb/1?sync=127.0.0.1:12345&data=127.0.0.1:10000' \
    --mtu 1500 \
    --rate 1.0 \
    --loop-limit 2 \
    --stats-interval 5
```

No `--withcp`, so the sender neither registers with a load balancer nor sends
sync packets.

`--rate 1.0` caps the send at 1 Gbps. Over loopback with no shaping the sender
will happily outrun a single-threaded reassembler and you will see loss that
tells you nothing about the code; leave the cap on for a first run.

`--loop-limit 2` makes the run finite. Drop it to replay forever and stop both
sides with Ctrl-C.

---

## 5. Read the output

The sender finishes with:

```text
========== Replay Summary ==========
  Replay loops completed    : 2
  Synchronized groups sent  : 242794
  EVIO events sent          : 485588
  EJFAT packets sent        : 485588
  Payload bytes sent        : 87602144 (83.54 MiB)
  Send errors               : 0
```

Stop the receiver with **Ctrl-C**. It reports on the EVIO events themselves:

```text
========== Reception Summary ==========
  EVIO events received      : 485588
  Payload bytes received    : 87602144 (83.54 MiB)
  Malformed EVIO events     : 0

  Per EJFAT source (dataId):
    dataId=1 rocid=2
         events=242794 bytes=44246704 malformed=0
         event size min=176 max=216 bytes
         timestamp first=0 last=242793000000 span=242.793 s
         timestamp regressions=0 frame-counter gaps=0
    dataId=2 rocid=3
         events=242794 bytes=43355440 malformed=0
         event size min=168 max=216 bytes
         timestamp first=0 last=242793000000 span=242.793 s
         timestamp regressions=0 frame-counter gaps=0

  E2SAR reassembler:
    Packets received        : 485588
    Events reassembled      : 485588
    Reassembly loss         : 0
    Enqueue loss            : 0
    Lost events             : 0
```

### What to check

| Line | Meaning |
|---|---|
| `EVIO events received` == sender's `EVIO events sent` | Nothing was dropped. |
| `Malformed EVIO events : 0` | Every event still has its EVIO magic **and** its block-length word matches the bytes delivered — i.e. reassembly returned the whole block and nothing more. |
| `dataId=1 rocid=2`, `dataId=2 rocid=3` | The file→source mapping survived: file 0 → `dataId` 1 → rocid 2, file 1 → `dataId` 2 → rocid 3. |
| `timestamp regressions=0` | Rebasing worked. Across the loop seam time kept moving forward; without it you would see one regression per seam. |
| `frame-counter gaps=0` | No event was silently lost between two that arrived. |
| `Reassembly loss` / `Enqueue loss` | E2SAR's own view of what never got assembled. Non-zero means the receiver could not keep up — lower `--rate` or raise `--recv-threads`. |

The two sources report the same 242.793 s span because they replayed in
timestamp lockstep, which is the property the whole program exists to provide.

---

## 6. If nothing arrives

| Symptom | Cause |
|---|---|
| Receiver sits at 0 events forever | Port mismatch. The sender's `data=<ip>:<port>` must name the receiver's `--recv-port`. The sender reports no error — it is writing into the void. |
| Events arrive but all are malformed | LB-header mismatch: one side thinks the control plane is on. Either pass `--withcp` to both or to neither. |
| `Reassembly loss` climbing | Receiver is behind. Lower `--rate`, raise `--recv-threads`, or add `--group-delay-us` on the sender. |
| Sender exits immediately, "cannot parse EJFAT URI" | Quote the URI. `&` in `?sync=…&data=…` is a shell background operator. |
| `evio_ejfat_recv: No such file` | Built without E2SAR. Check `EJFAT sending : ON` in the CMake summary. |

---

## 7. Smaller smoke test

To check the plumbing in a few seconds rather than minutes, replay one loop and
stop the receiver after a fixed count:

```bash
# Terminal 1
./build/evio_ejfat_recv \
    --uri 'ejfat://useless@127.0.0.1:9876/lb/1?sync=127.0.0.1:12345&data=127.0.0.1' \
    --recv-ip 127.0.0.1 --recv-port 10000 --max-events 2000

# Terminal 2
./build/evio_ejfat_replay --file-count 2 \
    --uri 'ejfat://useless@127.0.0.1:9876/lb/1?sync=127.0.0.1:12345&data=127.0.0.1:10000' \
    --rate 0.5 --loop-limit 1 \
    data/evio_192.168.0.13.bin data/evio_192.168.0.16.bin
```

`--max-events` makes the receiver exit on its own, so the run needs no Ctrl-C.

To exercise the reader and synchronizer with no networking at all, the sender
alone will do:

```bash
./build/evio_ejfat_replay --file-count 2 --dry-run --loop-limit 1 \
    data/evio_192.168.0.13.bin data/evio_192.168.0.16.bin
```

---

## 8. Two hosts

Same commands; replace the loopback addresses. On the receiver host, `--recv-ip`
must be an address that host actually holds, and the sender's `data=` must name
that address:

```bash
# receiver, on 10.0.0.5
./build/evio_ejfat_recv \
    --uri 'ejfat://useless@127.0.0.1:9876/lb/1?sync=127.0.0.1:12345&data=10.0.0.5' \
    --recv-ip 10.0.0.5 --recv-port 10000

# sender, elsewhere
./build/evio_ejfat_replay --file-count 2 \
    --uri 'ejfat://useless@127.0.0.1:9876/lb/1?sync=127.0.0.1:12345&data=10.0.0.5:10000' \
    --mtu 9000 --rate 5.0 \
    data/evio_192.168.0.13.bin data/evio_192.168.0.16.bin
```

Use `--mtu 9000` only if jumbo frames are enabled end to end; otherwise the
packets are silently dropped by the first hop that cannot carry them.

---

## 9. With a real load balancer

Add `--withcp` to **both** commands and use a real instance URI with a valid
token. The sender then registers itself with the control plane and emits sync
packets; the receiver registers as a worker and expects the LB to have stripped
the LB header. Everything else stays the same.
