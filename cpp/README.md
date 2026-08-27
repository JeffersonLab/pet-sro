# evio_ejfat_replay

Replays recorded FEB SRO capture files through an EJFAT load balancer, forever.

Given N EVIO capture files, the program opens all of them at once, reads one
event from each, checks that the N events share a timestamp, packetizes each
one into EJFAT packets and sends them to the load balancer. When the inputs run
out, all N files are reopened and the replay starts again. Ctrl-C stops it
cleanly.

This is a C++ tree parallel to the project's Java sources. It does not call into
Java: the EVIO reading and timestamp extraction are reimplemented natively.

Two programs are built:

* **`evio_ejfat_replay`** — the reader, synchronizer and packetizer described
  here.
* **`evio_ejfat_recv`** — the other end: reassembles EJFAT packets back into
  EVIO events and reports statistics about them. Built only when E2SAR is
  present.

**[HOWTO.md](HOWTO.md)** is a short end-to-end walkthrough of running both,
loopback, with no load balancer and no control plane.

---

## Design notes

Written before the implementation, from the two reference sources. Nothing in
the protocol handling below is guessed.

### EVIO layout and where the timestamp lives

Reference: `org.jlab.detimg.petiroc.daq.SroWireFormat` and
`org.jlab.detimg.petiroc.replay.ReplayFrameSource` in
`.../PetirocJava/src/main/org/jlab/detimg/petiroc/`. Transcribed into
`include/SroWireFormat.hpp`, which is the authority for this program.

The FEB event socket carries a stream of self-framing, **big-endian** EVIO
blocks. Word 0 is the block length in 32-bit words **including itself**, so a
block occupies `blockLenWords * 4` bytes:

| word | meaning |
|------|---------|
| 0 | EVIO block length in words (counts itself) |
| 1..6 | rest of the EVIO block header |
| 7 | EVIO magic `0xC0DA0100` |
| 8 | event bank length |
| 9 | event bank tag/type — **rocid** in the upper 16 bits |
| 10 | int32 sub-bank length |
| 11 | int32 sub-bank tag/type (`0xFF302011`) |
| 12 | fixed marker `0x31010003` |
| 13 | frame counter, unsigned 32-bit, starts at 0 |
| **14** | **timestamp, low 32 bits** |
| **15** | **timestamp, high 32 bits** |
| 16 | fixed marker `0x41850001` |
| 17 | fixed marker `0x00000081` |
| 18..23 | slow-controls bank (recent firmware only) |
| 24.. | TDC hit words |

**The timestamp is `word14 | (word15 << 32)`**, both read as unsigned
big-endian, in **nanoseconds**. This is exactly what `ReplayFrameSource.index()`
does. Timestamps start at 0 when a run's stream is enabled and advance by one
frame period (1 ms) per frame.

Older captures have no slow-controls bank and their TDC words begin at word 18.
That does not affect this program, which only reads words 0, 7, 9 and 13–15.

### The two capture formats

`ReplayFrameSource` accepts two on-disk formats and tells them apart without a
flag or filename convention, because both place the EVIO magic at **byte offset
28**. The byte order of those four bytes identifies the format:

* **`WIRE_DUMP`** (`evio_<ip>.bin`) — raw big-endian wire bytes as they arrived
  on the socket, block-length word present. Read verbatim.
* **`FEB_STREAM_DUMP`** (`pet_sro_feb_stream<N>.bin`, written by
  `pet_sro_eth.c`) — per frame, a native **little-endian** `int32` word count
  followed by that many little-endian words. This format **drops the EVIO
  block-length word**, because `sock_read_event_socket()` consumed it for
  framing and the C writer only stored what was left.

`EvioFileReader` normalises `FEB_STREAM_DUMP` on read: it byte-swaps every word
back to network order and re-prepends `wordCount + 1` as the block-length word.
After that both formats produce **byte-identical** events, which
`feb_stream_dump_yields_identical_events_to_the_wire_dump` checks.

Unlike the Java class, which maps or normalises a whole capture up front to
share it across thousands of emulated streams, this reader works block by block
and never holds more than one event per file in memory.

### EJFAT packetization

Reference: `/Users/gurjyan/Documents/Devel/e2sar-utils/src/e2sar_root.cpp`,
plus `E2SAR/include/e2sarDPSegmenter.hpp` and `e2sarHeaders.hpp`.

Packetization is done by **E2SAR's `Segmenter`**, the same mechanism
`e2sar_root.cpp` uses. `EjfatSender` configures it and hands it whole events;
the Segmenter fragments each event and writes the headers. For each fragment it
prepends an LB+RE header pair:

* **LB header** (16 bytes) — `'L'`,`'B'`, version 2, next-proto, entropy, and an
  event number that E2SAR derives from a microsecond clock for LB routing.
* **RE header** (20 bytes) — version, `dataId`, `bufferOffset`,
  `bufferLength` and `eventNum`. Note that `bufferLength` is the **whole event
  length**, not the fragment length; `bufferOffset` is the fragment's offset
  within the event. That pair is what lets the receiver reassemble, and it is
  how the beginning and end of an event are indicated.

An event of `B` bytes becomes `ceil(B / maxPayloadLen)` datagrams, where
`maxPayloadLen = MTU - (IP 20 + UDP 8 + LB 16 + RE 20)` = **1436 bytes at MTU
1500**, 8936 at MTU 9000. The program's packet counters use the same arithmetic,
so `--dry-run` reports the packet count a real send would produce.

The EVIO block is the EJFAT payload, unmodified — no re-framing, no re-tagging.

**Sending mode.** By default `EjfatSender` calls `Segmenter::sendEvent()`, which
fragments and writes on the calling thread. That means the reader's buffer can be
reused the moment the call returns, so no copy is made anywhere between the file
and the socket. `--async` switches to `Segmenter::addToSendQueue()` — the mode
`e2sar_root.cpp` uses — which returns immediately and therefore requires a copy
of each event, freed by a callback once E2SAR is done with it. It retries on
`MemoryError` (queue full) exactly as the reference does.

---

## Source-tree layout

```text
cpp/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── SroWireFormat.hpp      EVIO word offsets and byte-order helpers
│   ├── EvioFileReader.hpp     streaming reader + EventSource interface
│   ├── EvioEventView.hpp      validate/decode a reassembled event (receiver)
│   ├── EventSynchronizer.hpp  N-way timestamp merge
│   ├── PacketSink.hpp         transport interface + NullPacketSink
│   ├── EjfatSender.hpp        PacketSink backed by E2SAR (pimpl)
│   ├── EjfatReceiver.hpp      reception + reassembly via E2SAR (pimpl)
│   ├── ReceiveStats.hpp       receiver counters, reports, validation levels
│   ├── TimestampRebaser.hpp   loop-seam timestamp/frame-counter rebasing
│   ├── ReplayLoop.hpp         application control logic
│   ├── ReplayStats.hpp        counters and reports
│   ├── SignalHandler.hpp      Ctrl-C -> atomic flag
│   └── Logging.hpp            level-filtered logger
├── HOWTO.md                   end-to-end run, no load balancer
├── src/                       one .cpp per header, plus main.cpp
│   │                          and recv_main.cpp (evio_ejfat_recv)
│   └── actor/                 the ERSAP plugin
│       ├── CMakeLists.txt
│       ├── EjfatReceiverActor.hpp
│       ├── EjfatReceiverActor.cpp
│       ├── services.yaml      example ERSAP deployment
│       └── README.md          data-format and compatibility analysis
└── tests/
    ├── CMakeLists.txt
    ├── TestHarness.hpp        ~100-line test runner
    ├── EvioFixtures.hpp       synthetic captures, stub source, mock sink
    ├── test_evio_reader.cpp
    ├── test_synchronizer.cpp
    ├── test_packetization.cpp
    ├── test_rebaser.cpp
    ├── test_event_view.cpp
    └── test_real_captures.cpp  (registered only when data/ is present)
```

The four concerns the task calls out are in separate components: the EVIO reader
(`EvioFileReader`), timestamp synchronization (`EventSynchronizer`), EJFAT
packetization (`EjfatSender` behind `PacketSink`) and application control
(`ReplayLoop`). Only two files include `<e2sar.hpp>`: `EjfatSender.cpp` and
`EjfatReceiver.cpp`. Everything else, and every test, builds without E2SAR.

Reception is a component in its own right, for the same reason sending is:
`evio_ejfat_recv` and the ERSAP actor in `src/actor` both drive
`EjfatReceiver`, so neither can develop its own defaults or its own idea of
what a received event is. `EjfatReceiverConfig`'s member initialisers are the
single source of truth for the defaults documented under
[Command-line reference](#command-line-reference).

### What the reassembler returns

`e2sar::Reassembler::recvEvent()` hands back a `new[]`-allocated buffer that the
caller owns and must `delete[]`; `petsro::ReassembledEvent` is the move-only
holder that does exactly that. Every transport header -- UDP, LB and RE -- has
already been stripped, so the buffer holds precisely the payload the segmenter
was given for that event. Because `ReplayLoop::sendGroup()` sends one
`EvioEvent` per EJFAT event, **one reassembled buffer is one complete
big-endian EVIO version-4 block containing exactly one EVIO event**, with its
block-length word present and nothing prepended or appended.

`EvioEventView::inspectEvioEvent()` proves that on every event by comparing the
block-length word against the number of bytes actually delivered -- the one
check the file reader gets for free and the network does not.
`cpp/src/actor/README.md` carries the field-by-field verification against the
captures in `data/`.

---

## Dependencies

| Dependency | Required | How CMake finds it |
|---|---|---|
| C++17 compiler | yes | — |
| CMake ≥ 3.16 | yes | — |
| **Boost.program_options** | yes | `find_package(Boost COMPONENTS program_options)`. Set `BOOST_ROOT` if it is somewhere unusual. Same library `e2sar_root.cpp` uses. |
| **Threads** | yes | `find_package(Threads)` |
| **E2SAR** | optional | `pkg_check_modules(E2SAR IMPORTED_TARGET e2sar)` — the same `e2sar.pc` that `e2sar-utils/meson.build` consumes, so a `PKG_CONFIG_PATH` set up for that project works here unchanged. |

No new third-party dependency was added. There is no EVIO C++ library in this
repository or its environment, so the reader is native — the required behaviour
is small and fully specified by `SroWireFormat`, as described above. The tests
use a ~100-line harness rather than GoogleTest, for the same reason.

**When E2SAR is not found**, CMake prints a warning and builds anyway. The
binary then reads, synchronizes and does packet accounting under `--dry-run`,
the full test suite still runs, and `--uri` fails with an explanatory message.
This keeps the reader and synchronizer testable on a machine with no E2SAR
install.

### Warnings

Built with `-Wall -Wextra -Wpedantic`, plus `-Wshadow -Wconversion
-Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor`. Nothing is suppressed.
`-DPETSRO_WERROR=ON` promotes them to errors.

---

## Build

```bash
cmake -S cpp -B build
cmake --build build -j
```

To include EJFAT sending, put `e2sar.pc` on the search path first:

```bash
export PKG_CONFIG_PATH=/path/to/e2sar/lib/pkgconfig:$PKG_CONFIG_PATH
cmake -S cpp -B build
cmake --build build -j
```

CMake reports which it configured:

```text
-- evio_ejfat_replay configuration:
--   Build type    : RelWithDebInfo
--   EJFAT sending : ON
--   Tests         : ON
```

Run the tests:

```bash
cd build && ctest --output-on-failure
```

---

## Command-line reference

```text
evio_ejfat_replay --file-count N --file f1.evio --file f2.evio ... --uri <ejfat_uri> [OPTIONS]
evio_ejfat_replay --file-count N [OPTIONS] --uri <ejfat_uri> f1.evio f2.evio ...
```

Files may be given with repeated `--file` options or positionally, and the two
forms may be mixed. **The total number of files must equal `--file-count`**, or
the program exits with status 2 before opening anything.

### Input

| Option | Default | Meaning |
|---|---|---|
| `-n, --file-count N` | *required* | Number of EVIO input files. Must equal the number supplied. |
| `-f, --file PATH` | — | An EVIO input file; repeat once per stream. May also be positional. |

### EJFAT / E2SAR

These mirror the options of `e2sar_root.cpp`.

| Option | Default | Meaning |
|---|---|---|
| `-u, --uri URI` | *required unless `--dry-run`* | EJFAT URI of the load balancer. |
| `--dataid-base N` | `1` | EJFAT `dataId` of the first file; file *i* gets `dataid-base + i`. |
| `--eventsrcid N` | `1` | EJFAT event source id, carried in the Sync header. |
| `--mtu N` | `1500` | Segmenter MTU, 576–9000. `0` auto-detects (Linux only). |
| `--rate F` | `1.0` | Send rate in Gbps. Negative means unlimited. |
| `--sockets N` | `4` | UDP send sockets the segmenter opens. |
| `-c, --withcp` | off | Register this sender with the load balancer control plane. |
| `-V, --novalidate` | off | Do not validate the control plane's SSL certificate. |
| `--async` | off | Use `addToSendQueue()` (copies each event) instead of inline `sendEvent()`. |
| `--entropy-per-source` | off | Set LB entropy to `1 + fileIndex` instead of letting E2SAR randomise it per event. |

### Replay

| Option | Default | Meaning |
|---|---|---|
| `--loop-limit N` | `0` | Stop after N complete replay loops. `0` means run until Ctrl-C. |
| `--group-delay-us N` | `0` | Pause between synchronized groups, in microseconds. |
| `--reset-event-numbers` | off | Restart EJFAT event numbers at 1 on each loop. |
| `--no-rebase-timestamps` | off | Replay captured timestamps verbatim. By default each loop adds the replayed span plus one frame period so time never goes backwards. |
| `--dry-run` | off | Read and synchronize but send nothing. No load balancer needed. |
| `--dry-run-mtu N` | `1500` | MTU used for `--dry-run` packet accounting. |

### Reporting

| Option | Default | Meaning |
|---|---|---|
| `-h, --help` | — | Show the full help. |
| `--stats-interval F` | `5.0` | Seconds between progress lines. `0` disables them. |
| `-v, --verbose` | off | Per-event, per-packet and per-mismatch detail. Very noisy. |
| `-q, --quiet` | off | Warnings and errors only. |

### Examples

```bash
# Two streams to a load balancer, forever.
evio_ejfat_replay --file-count 2 --uri ejfat://token@cp.example:18008/lb/1 \
    --file evio_10.0.0.1.bin --file evio_10.0.0.2.bin

# Four streams, jumbo frames, control plane, capped at 5 Gbps.
evio_ejfat_replay --file-count 4 --uri ejfat://... \
    --mtu 9000 --withcp --rate 5.0 \
    s0.bin s1.bin s2.bin s3.bin

# Exercise the reader and synchronizer with no load balancer in reach.
evio_ejfat_replay --file-count 2 --dry-run --loop-limit 1 s0.bin s1.bin
```

---

## Behaviour

### Mapping input files to EJFAT source identifiers

**`dataId(i) = --dataid-base + i`**, where `i` is the zero-based position of the
file on the command line. With the default base of 1, the first file is
`dataId` 1, the second 2, and so on. The mapping is deterministic and stable
across replay loops, so a receiver can attribute every packet to a source by its
RE-header `dataId`. `--dataid-base + N - 1` must fit in 16 bits; the program
refuses to start otherwise.

Entropy is 0 by default, which tells E2SAR to generate a random value per event.
`--entropy-per-source` sets it to `1 + i` instead, pinning each source to one
path through the load balancer.

### Timestamp rebasing across loops

**On by default.** Each replay loop adds the span just replayed **plus one frame
period** to the timestamp (words 14/15) and frame counter (word 13) written into
every outgoing event, so replayed time never goes backwards. The extra frame
period makes the loop seam exactly as long as any other inter-frame gap, so a
consumer cannot tell where the capture wrapped.

This follows `ReplayStream.advance()`, whose comment states the case: replayed
verbatim, time jumps backwards at every loop point, and
`EventTimeSlice.canAccept()`/`isFull()`, every coincidence window, and any
time-slice-based load-balancer routing downstream all break.

**One deliberate difference from the Java code.** `ReplayStream` computes the
offset per stream, from that stream's own `durationNanos()`. It can — its
streams are independent emulated FEBs that are never compared to each other.
Here the streams must keep *exactly* equal timestamps or `EventSynchronizer`
stops emitting groups at all, and captures of unequal length (the normal case —
see below) would get different per-stream offsets and drift apart permanently
after the first loop. So **the timestamp offset is shared by all N streams**,
derived from the span of the synchronized groups actually sent, which is common
to every stream by construction. The frame counter is rebased per stream, since
nothing synchronizes on it.

Rebasing happens at send time, after synchronization, so the merge always
compares the raw timestamps read from the files. Words 13–15 are patched in
place in the reader's buffer, which costs no copy: that buffer is overwritten by
the next read anyway. Every other byte of the event is untouched.

`--no-rebase-timestamps` replays the captured timestamps verbatim.

With the captures in `data/`, whose synchronized span is 121.396 s:

```text
loop 1 ends   ts = 121396000000
loop 2 begins ts = 121397000000   <- one frame period later, no discontinuity
loop 3 begins ts = 242794000000
```

### Event numbering

One monotonically increasing EJFAT event number per **EVIO event sent**, so the
N events of a synchronized group get N consecutive numbers. Numbering **starts
at 1**, because E2SAR treats a passed event number of 0 as "do not override the
internal counter".

By default numbering **keeps increasing across replay loops**, so no two events
in a run share a number. `--reset-event-numbers` restarts at 1 on each loop
instead.

### Timestamp synchronization

For each output cycle the program holds one current event per stream and:

1. compares the N timestamps;
2. if all are equal, sends the group;
3. otherwise finds the largest current timestamp and advances every stream
   below it, discarding the events it passes;
4. repeats until they agree, a stream hits EOF, an error occurs, or shutdown is
   requested.

**A group is never sent unless all N timestamps are exactly equal.** There is no
tolerance window: the frames come from one clock, so a near-miss is a real
misalignment worth seeing rather than papering over.

Skipped events, timestamp mismatches and timestamp regressions are counted and
reported; individual skips are logged at `--verbose`.

**Assumptions about ordering**, since the algorithm depends on them:

* Timestamps are non-decreasing within a file. `SroWireFormat` documents them as
  advancing by exactly one frame period from 0.
* A stream whose timestamp goes backwards is logged as a discontinuity and still
  advanced. It can no longer catch the leader, so the pass ends at EOF rather
  than hanging.
* Progress is guaranteed because every non-matching pass consumes at least one
  event from at least one finite file. A `maxAdvancesPerGroup` guard (100 000)
  is a second belt: pathological input that never converges reports an error
  instead of grinding.

### When one file reaches EOF

The pass ends for **all** streams. Any partially assembled group is discarded
whole — sending some of a cycle's N events would hand the load balancer an event
group that never completes downstream. All files are then closed and the entire
set is reopened from the beginning.

One file is never restarted independently while the others continue: that would
silently pair frames from different points in the capture.

**Consequence for captures of unequal length: the tail of every longer file is
never replayed.** The pass is bounded by the shortest input, and because all
files restart together it is the *same* tail that is dropped on every loop. With
the captures in `data/`, `evio_192.168.0.13.bin` has 125 399 frames against
`evio_192.168.0.16.bin`'s 121 397, so its last **4002 frames (704 328 bytes,
3.2%) are never sent, on any loop**. That is the correct behaviour for
timestamp-synchronized replay — those frames have no partner to be synchronized
with — but if you need the whole of every capture to reach the load balancer,
the inputs have to be trimmed to a common frame count first.

A truncated final block — a capture cut short by a killed writer — is logged,
counted as a `truncatedTail`, and treated as end of file, which is what
`ReplayFrameSource` does. Genuinely bad data (an implausible block length, a bad
magic word, a block too short to hold a timestamp) is `Malformed` and stops the
run with an error naming the file and byte offset.

### Ctrl-C

`SIGINT` and `SIGTERM` are handled. The handler does exactly one thing: store
`true` into a lock-free `std::atomic<bool>`. No logging, no allocation, no
cleanup — all of that happens in normal control flow, which polls the flag
between groups and during a long resynchronization.

Files are closed by RAII, the sink is drained, final statistics are printed, and
the program exits 0. A **second** signal calls `_Exit(128 + signal)` immediately,
which is async-signal-safe and gives an operator a way out if a send is wedged
in the kernel.

### Logging and statistics

Logged at the default level: files opened and reopened with the detected byte
order, replay-loop number, per-pass skip counts, malformed records, send
failures naming the file and destination, and a progress line every
`--stats-interval` seconds. Per-event and per-packet lines are `--verbose` only.

Printed at shutdown: replay loops completed, synchronized groups sent, EVIO
events sent, EJFAT packets sent, payload bytes and average rate, timestamp
mismatches, timestamp regressions, incomplete groups dropped, read errors, send
errors, and — per input file — events read, sent, skipped, bytes, read errors,
send errors and truncated tails. With E2SAR, the Segmenter's own fragment, error
and sync counters follow.

---

## Tests

```bash
cd build && ctest --output-on-failure
```

87 tests across six executables. No live load balancer is needed: the
transport is the injectable `PacketSink`, and the tests use
`MockPacketSink`, which records every event and its fragmentation and can be
told to fail on demand.

The unit tests build their own capture files from the layout `SroWireFormat`
documents (`EvioFixtures.hpp`), in both on-disk formats, so they run anywhere.
The synchronizer tests use `VectorEventSource`, a stub that replays a canned
list of timestamps, so alignment logic is tested with no file system involved.

**`test_real_captures`** additionally checks the reader against genuine FEB
captures in `<repo>/data`. Those files are tens of megabytes and are not tracked
in git, so CMake registers this test only when they are present; point it
elsewhere with `-DPETSRO_CAPTURE_DIR=/path/to/captures`. Its expectations were
established by scanning the files with an independent decoder written from
`SroWireFormat`, not by recording whatever this reader happened to produce.

| Coverage | Where |
|---|---|
| Opening a valid EVIO file (both formats) | `test_evio_reader` |
| Reading one complete event, payload unaltered | `test_evio_reader` |
| Extracting a known timestamp spanning both words | `test_evio_reader` |
| Detecting EOF, and distinguishing it from malformed input | `test_evio_reader` |
| Reopening and reading from the beginning, repeatedly | `test_evio_reader` |
| Synchronizing already aligned streams | `test_synchronizer` |
| Advancing one, and several, lagging streams | `test_synchronizer` |
| Unsynchronizable, malformed and I/O-failing streams | `test_synchronizer` |
| Packetizing events smaller than one packet | `test_packetization` |
| Packetizing events requiring multiple packets | `test_packetization` |
| Graceful shutdown through the shared flag | `test_synchronizer`, `test_packetization` |
| Loop-seam rebasing: verbatim first pass, one-frame-period seam, shared offset, 64-bit carry, monotonic over many loops | `test_rebaser` |
| Reassembled-event validation: magic, short/long delivery, round trip with the reader | `test_event_view` |
| Real captures: format, clean EOF, full-file coverage, timestamps, rocid, reopen, frame-for-frame sync, rebased monotonicity across seams | `test_real_captures` |

### Verified against the captures in `data/`

`evio_192.168.0.13.bin` (22 827 680 bytes) and `evio_192.168.0.16.bin`
(21 677 720 bytes), cross-checked against an independent decoder:

| | `…0.13` | `…0.16` |
|---|---|---|
| Format | `WIRE_DUMP` | `WIRE_DUMP` |
| Blocks | 125 399 | 121 397 |
| rocid | 2 | 3 |
| Timestamps | 0 → 125.398 s | 0 → 121.396 s |
| Timestamp step | 1 ms, **every** frame | 1 ms, **every** frame |
| Frame-counter gaps | none | none |
| Block sizes | 168–216 bytes | 168–216 bytes |
| Malformed / truncated | none | none |

Every byte of both files is consumed, and an FNV-1a hash over all event bytes
produced by `EvioFileReader` matches a hash computed independently in Python —
the events are returned unaltered. Packet accounting was checked by forcing
fragmentation with `--dry-run-mtu 100`: 1 311 228 packets, matching
`Σ ceil(blockBytes / 36)` exactly. Eight replay loops and a mid-loop `SIGINT`
completed with zero read or send errors.

---

## Known assumptions and limitations

* **Timestamp ordering** — as described above: non-decreasing within a file,
  exact equality across files.
* **Capture formats** — only the two `ReplayFrameSource` accepts. A file whose
  byte 28 is not the EVIO magic in either byte order is rejected at open. Both
  captures in `data/` are `WIRE_DUMP`; the `FEB_STREAM_DUMP` normalisation path
  is covered by unit tests but has not been exercised against a real
  `pet_sro_feb_stream*.bin`.
* **These captures never fragment.** Blocks in `data/` are 168–216 bytes, well
  under the 1436-byte payload of a 1500-byte MTU, so every event is exactly one
  packet in normal operation. The multi-packet path is covered by unit tests and
  was verified on real data only by forcing a small MTU.
* **Unequal capture lengths** — the tail of every longer file is never replayed;
  see "When one file reaches EOF" above.
* **Block size ceiling** — blocks above `MAX_BLOCK_WORDS` (100 000 words,
  400 KB) are rejected as implausible, matching the Java reader.
* **Rebasing rewrites three words of every event.** Words 13, 14 and 15 differ
  from the captured bytes; everything else is byte-identical. Turn it off with
  `--no-rebase-timestamps` if you need the payload untouched, at the cost of
  time jumping backwards at each loop seam.
* **The rebased frame counter wraps at 32 bits**, as it does in `ReplayStream`,
  which casts to `int` for the same reason. At the 1 ms frame period these
  captures use, that is about 50 days of continuous replay.
* **Reading is single-threaded.** N files are read in lock step on one thread,
  which is what synchronization requires. At the ~200-byte frames and 1 ms
  cadence these captures hold, the bottleneck is the network, not the reader.
* **`--async` copies every event.** Unavoidable: `addToSendQueue()` returns
  before the bytes are sent. The default synchronous path does not copy.
* **No receiver.** This program only sends. Use `e2sar_root.cpp --recv` or
  `ersap_et_receiver` on the other end.
* **`EjfatSender`'s E2SAR code path is compiled only when E2SAR is found.** It
  was verified by syntax-checking against the API signatures in
  `E2SAR/include/e2sarDPSegmenter.hpp`; the E2SAR checkout at
  `~/Documents/Devel/E2SAR` does not itself compile against Boost 1.89, because
  `e2sarUtil.hpp` uses `boost::asio::io_service`, removed in Boost 1.87. Build
  against the E2SAR install that `e2sar_root.cpp` uses.
