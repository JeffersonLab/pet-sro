# EjfatReceiverActor

An ERSAP C++ source actor that receives EJFAT packets, reassembles them with
the E2SAR `Reassembler`, **aggregates the N per-stream events of one
synchronized group by their common LB tick**, and publishes **one combined
EVIO v6 record per group** to the next actor in the chain — normally a Java
ERSAP processing actor.

It reuses `evio_ejfat_recv`'s reception and reassembly code rather than
duplicating it: both drive `petsro::EjfatReceiver` (`cpp/include/EjfatReceiver.hpp`,
`cpp/src/EjfatReceiver.cpp`) and both account with `petsro::ReceiveStats`
(`cpp/include/ReceiveStats.hpp`). The aggregation stage on top of that is
`petsro::EvioAggregator` (`cpp/include/EvioAggregator.hpp`,
`cpp/src/EvioAggregator.cpp`), also independent of ERSAP and unit-tested in
`cpp/tests/test_aggregator.cpp`.

---

## 1. Overall data flow

```
UDP socket(s)
     │
     ▼
e2sar::Reassembler ─────►  ReassembledEvent            (per-stream EVIO v4 block)
                            │
                            ▼
                     EvioAggregator::add()             (group table, keyed by LB tick)
                            │  when N members with the same tick have arrived
                            ▼
                     buildEvioV6Record()               (14-word v6 header + index + N banks)
                            │
                            ▼
                     bounded output queue
                            │
                            ▼
                     execute() publishes one EngineData per aggregated group
```

The sender assigns the **same** LB tick to every event in a synchronized
group (see `ReplayLoop::sendGroup` and `cpp/README.md`, "Event numbering"),
so the load balancer routes every group member to the same receiver host.
On this host the aggregator collects them, closes the group as soon as the
N-th member arrives (or when the wait window expires — see partial-groups
below), and hands one EVIO v6 record to the pipeline.

## 2. What is published

One published payload is one **complete EVIO v6 record** in big-endian wire
form. Its layout, built by `EvioAggregator::buildEvioV6Record`, is:

```
   word  0   Record length in 32-bit words (header + index + data)
   word  1   Record number (the group's LB tick, low 32 bits)
   word  2   Header length in words = 14
   word  3   Event count = N (members present in this group)
   word  4   Index array length in bytes = 4 * N
   word  5   Bit info | version:  version = 6 (bits 0..7),
                                  event type = 1 (bits 10..13, "EVIO event")
   word  6   User header length in bytes = 0 (no user header)
   word  7   Magic 0xC0DA0100
   word  8   Uncompressed data length low 32 bits (bytes)
   word  9   Uncompressed data length high 32 bits (bytes)
   word 10   Compression type (bits 28..31) | compressed data length (low 28)
             type 0 = uncompressed; compressed length = uncompressed length
   word 11   User register 1 low 32 bits  (group tick low)
   word 12   User register 1 high 32 bits (group tick high)
   word 13   User register 2              (member count, for convenience)
   ---
   Index array: N × 4-byte big-endian entries, each the length in bytes of
                the corresponding event.
   Event data:  the N event banks, concatenated in dataId order.
                Each entry starts with its own EVIO bank length word, so a
                downstream reader can walk it either through the index or
                through the bank chain.
```

The sender's per-stream payload is one EVIO v4 block containing exactly one
EVIO event bank; the aggregator extracts that bank (bytes at offset 32
onward — see `SroWireFormat.hpp`) and concatenates the N banks into the v6
record's data section. Members are ordered by `dataId`, so the record's
event order is deterministic across runs regardless of the arrival order at
the UDP socket.

### Downstream compatibility

**This is a wire-format change from previous versions of the actor**, which
published raw EVIO v4 blocks. A downstream consumer expecting a v4 block
(magic at word 7, 8-word header, single event bank) will not decode the v6
record correctly — the fields are in different positions. The MIME type
(`binary/data-evio` by default) is preserved so the Java `EvioBlockDataType`
still restores `BIG_ENDIAN` correctly, but the record content needs a v6
reader.

## 3. Configuration

Keys are the long option names of `evio_ejfat_recv` with the leading `--`
removed where they overlap. `group-size` is required in addition to `uri`.

### Receiver keys (unchanged from previous versions)

| Key | Type | Default | Meaning | Validation |
| --- | --- | --- | --- | --- |
| `uri` | string | *required* | EJFAT URI. Without `withcp` only its `data=` address is used | non-empty; parsed by `e2sar::EjfatURI::getFromString` |
| `recv-ip` | string | `127.0.0.1` | local IP to listen on | non-empty; parsed by `boost::asio::ip::make_address` |
| `recv-port` | integer | `10000` | starting UDP port; must match the sender's `data=` port | 1 … 65535 |
| `recv-threads` | integer | `1` | number of reassembly threads | ≥ 1 (and ≤ 1024) |
| `event-timeout` | integer | `500` | ms before an incomplete UDP-level event is abandoned | > 0 |
| `poll-timeout` | integer | `1000` | ms `recvEvent()` waits before returning without an event | > 0 |
| `withcp` | boolean | `false` | use the EJFAT control plane | boolean |
| `novalidate` | boolean | `false` | skip control-plane SSL certificate validation. **Applies only when `withcp` is true**, and never to EVIO payload validation | boolean |
| `max-events` | unsigned | `0` | stop receiving after this many complete **per-stream** events; `0` runs until the actor is stopped. Note this counts individual arrivals, not aggregated groups | ≥ 0 |
| `stats-interval` | unsigned | `5` | seconds between progress messages; `0` disables them | ≥ 0 |
| `verbose` | boolean | `false` | log one line per received event | mutually exclusive with `quiet` |
| `quiet` | boolean | `false` | log only warnings and errors | mutually exclusive with `verbose` |

### Aggregation keys (new)

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `group-size` | integer | *required* | number of streams the sender is producing, i.e. the number of members expected per group. 1 … 65535 |
| `group-timeout` | integer | `500` | ms a group may sit open waiting for stragglers before being reaped, measured from the first member's arrival, not the last (> 0) |
| `partial-groups` | string | `drop` | what to do with a group that reaches `group-timeout` with fewer than `group-size` members: `drop` (discard silently, count it) or `emit-partial` (emit an aggregated record with only the members present, `word 3` reflects the true count) |
| `max-open-groups` | integer | `4096` | safety cap on the group table. When hit, the oldest open group is evicted through the same `partial-groups` policy. A steady-state stream keeps only a handful in flight; hitting this cap means groups are timing out faster than they close |

### Actor-specific keys (unchanged)

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `validation` | string | `structural` | payload validation depth applied to each incoming per-stream event before aggregation. `none`: decode only. `structural`: minimum size, EVIO magic, and declared block length vs. delivered byte count must agree; timestamp regressions and frame-counter gaps are counted and logged but the event is still aggregated. `strict`: additionally checks the block-header length word (must be 8) and the EVIO version (must be 4), and a timestamp regression or a frame-counter gap also condemns the event |
| `output-mime` | string | `binary/data-evio` | published ERSAP data type. `binary/data-evio` (default): pairs with `EvioBlockDataType`, which hands Java a `BIG_ENDIAN` buffer. `binary/data-jobj`: pairs with `JavaObjectType.JOBJ`, drop-in for the existing chain, consumer must set the byte order itself. `binary/bytes`: `ersap::type::BYTES`, for chaining behind another native actor |
| `queue-size` | integer | `256` | aggregated groups buffered between the pump thread and `execute()` |

An unrecognised key is reported on stderr and ignored. Any validation
failure returns `EngineStatus::ERROR` with the reason **and** prints the
full key table (`EjfatReceiverActor::configurationHelp()`), so an operator
never has to read the source to find a key name.

---

## 4. Runtime behaviour

* **Threading.** A dedicated pump thread owns the `EjfatReceiver` and the
  `EvioAggregator`; `execute()` only pops from the output queue. No ERSAP
  control thread ever sits inside `recvEvent()`, and E2SAR's internal receive
  queues keep draining even when the downstream chain stalls. The pump never
  blocks longer than 200 ms in one `recvEvent()` call, so a stop request is
  acted on promptly however large `poll-timeout` is.
* **Reap sweep.** After every `recvEvent()` return (including `Timeout` and
  `Error`), the pump calls `EvioAggregator::reap()` to drop or emit any group
  older than `group-timeout`. That means stale groups are evicted even when
  the stream stalls, not only when new arrivals happen.
* **Back pressure.** A full output queue drops the *oldest* aggregated group
  and counts it, which bounds memory at `queue-size × record-size` and keeps
  the newest data flowing. The aggregator's own table is bounded by
  `max-open-groups` with the same oldest-first eviction policy.
* **Poll-timeout expiry** is not an error: `execute()` returns
  `EngineStatus::WARNING` with no data, and nothing is logged.
* **`max-events`** stops the receive loop cleanly after that many per-stream
  events. The aggregator is `flush()`ed through the configured
  `partial-groups` policy, so any groups it still holds either emit as
  partial records or are dropped and counted before the loop ends. The
  actor stays alive; the ERSAP process is not terminated.
* **Errors.** A transport error is counted and logged at most once every 5 s;
  a malformed payload at most once a second; an aggregation rejection (duplicate
  dataId, malformed by the aggregator's own extractor) at most once a second.
  Nothing throws out of `execute()`.
* **Shutdown.** `reset()` and the destructor both call `shutdown()`, which
  stops the pump thread, flushes the aggregator, prints the final statistics
  (receiver + aggregator + queue accounting), deregisters the control-plane
  worker, stops the E2SAR threads and clears the queue. Idempotent and
  `noexcept`; no buffer, socket or thread is leaked.
* **Copies.** Exactly one copy of each per-stream payload — out of the
  `new[]` buffer E2SAR hands over — into an internal member. From there it
  is moved into the assembled v6 record, moved into an `EngineData`, and
  moved again by the serializer's rvalue overload.

---

## 5. What the reassembler returns (background)

Verified from `E2SAR 0.3.2`, from `recv_main.cpp`, from the sender side
(`ReplayLoop::sendGroup()` → `EjfatSender::send()`), and from the on-disk
captures in `data/`.

| Question | Answer |
| --- | --- |
| Buffer type | `std::uint8_t*` out-parameter of `e2sar::Reassembler::recvEvent()`, plus a `std::size_t` length, an `EventNum_t` and a `std::uint16_t` dataId |
| Ownership | E2SAR allocates with `new[]` and **transfers ownership to the caller**. `petsro::ReassembledEvent` is the move-only RAII holder |
| Transport headers | **None remain.** The Reassembler strips UDP, LB and RE headers. With `withcp=false` the LB header is still on the wire and `ReassemblerFlags::withLBHeader = true` accounts for it |
| Length / framing | Exactly the payload the segmenter was given for that event. No length prefix, no padding, no trailer |
| Byte order | Whatever the sender put there — normalised **big-endian** EVIO for this project |
| One per-stream buffer is… | **One complete EVIO v4 block containing exactly one EVIO event bank.** |

Per-block PET/SRO content (from `SroWireFormat.hpp`): word 9 upper half is
the rocid, word 13 the frame counter, words 14/15 the 64-bit nanosecond
timestamp, words 18+ the slow-controls bank and the TDC hit words. The
aggregator preserves this content byte for byte inside the v6 record.

---

## 6. Byte order on the wire to Java

`ersap-cpp`'s raw-bytes serializer and Java's `RawBytesSerializer` both use
the byte sequence itself as the wire image — Java's `read(ByteBuffer)` is
literally `return data;`. Java wraps the received `byte[]` with
`ByteBuffer.wrap()`, so the consumer sees `position = 0`, `limit = capacity
= payload length`: only valid payload bytes, no unused capacity, no length
prefix. The EJFAT event number (group tick) travels in the ERSAP
communication id, not in the payload.

**But the ByteBuffer's byte order arrives wrong.** `DataUtil.deserialize()`
does:

```java
ByteBuffer bb = ByteBuffer.wrap(msg.getData());
if (metadata.getByteOrder() == xMsgMeta.Endian.Little) {
    bb.order(ByteOrder.LITTLE_ENDIAN);
}
```

`xMsgMeta.byteOrder` is a proto2 `optional` over an enum whose first
constant is `Little = 1`, so an **unset** field reads back as `Little`.
`ersap-cpp` never sets it. Every buffer a C++ actor publishes therefore
reaches Java marked `LITTLE_ENDIAN`. The bytes are correct and complete;
only the order flag is wrong. The actor cannot fix it from C++
(`EngineData::meta_` is private).

That is why the default output type is **`binary/data-evio`**, whose Java
counterpart `org.jlab.ersap.actor.datatypes.EvioBlockDataType` restores
`BIG_ENDIAN` inside the deserializer, where no processing actor can forget
it. That is exactly the byte order the aggregated EVIO v6 record needs.

Publishing `binary/data-jobj` still delivers the right bytes and remains a
drop-in for a chain that already declares `JavaObjectType.JOBJ` — but every
consuming actor must call `.order(ByteOrder.BIG_ENDIAN)` itself before
reading a word.

---

## 7. Building

Needs both E2SAR (via `pkg-config`, same `e2sar.pc` as the rest of the tree)
and an `ersap-cpp` installation:

```sh
cmake -S cpp -B build -DERSAP_HOME=$ERSAP_HOME
cmake --build build -j
cmake --install build --prefix $ERSAP_HOME
```

When `ersap-cpp` is absent the actor is skipped with a message and
everything else still builds. `-DPETSRO_BUILD_ERSAP_ACTOR=OFF` skips it
explicitly.

The aggregator itself does not depend on E2SAR or on ERSAP, so its unit
tests run in any build configuration and cover the wire-level details of
the emitted v6 record independently.
