# EjfatReceiverActor

An ERSAP C++ source actor that receives EJFAT packets, reassembles them with
the E2SAR `Reassembler`, and publishes each complete payload to the next actor
in the chain — normally a Java ERSAP processing actor.

It reuses `evio_ejfat_recv`'s reception and reassembly code rather than
duplicating it: both drive `petsro::EjfatReceiver` (`cpp/include/EjfatReceiver.hpp`,
`cpp/src/EjfatReceiver.cpp`) and both account with `petsro::ReceiveStats`
(`cpp/include/ReceiveStats.hpp`). Defaults live in `EjfatReceiverConfig`, so the
executable and the actor cannot drift apart.

---

## 1. What the reassembler actually returns

Verified from `E2SAR 0.3.2`, from `recv_main.cpp`, from the sender side
(`ReplayLoop::sendGroup()` → `EjfatSender::send()`), and from the on-disk
captures in `data/`.

| Question | Answer |
| --- | --- |
| Buffer type | `std::uint8_t*` out-parameter of `e2sar::Reassembler::recvEvent()`, plus a `std::size_t` length, an `EventNum_t` and a `std::uint16_t` dataId |
| Ownership | E2SAR allocates with `new[]` and **transfers ownership to the caller**. It must be released with `delete[]`. `petsro::ReassembledEvent` is the move-only RAII holder that does this |
| Lifetime | Unbounded until the caller frees it; not recycled, not owned by the Reassembler |
| Transport headers | **None remain.** The Reassembler strips the UDP, LB and RE headers. With `withcp=false` the LB header is still on the wire and `ReassemblerFlags::withLBHeader = true` accounts for it |
| Length / framing | Exactly the payload the segmenter was given for that event. No length prefix, no padding, no trailer |
| Byte order | Whatever the sender put there. This project's sender emits normalised **big-endian** EVIO |
| One buffer is… | **one complete EVIO block containing exactly one EVIO event** |

### Is it valid EVIO?

Yes. Checked field by field against `data/evio_192.168.0.13.bin`, which is what
`evio_ejfat_replay` transmits verbatim:

```
w0  0x0000002C  block length            44 words = 176 bytes
w1  0x00000001  block number            1, 2, 3, … dense
w2  0x00000008  header length           8 words   (EVIO v4 block header)
w3  0x00000001  event count             1
w4  0x00000002  reserved
w5  0x00002204  bit info | version      version = 4
w6  0x00000000  reserved
w7  0xC0DA0100  magic                   big-endian EVIO magic
w8  0x00000023  event bank length       35 words → 36 with its own length word
w9  0x00021011  bank tag/type           rocid = 0x0002 in the upper 16 bits
```

`8 (header) + 36 (bank) = 44 = w0`, and `44 × 4 = 176` bytes is precisely what
one reassembled buffer contains. `EvioEventView::inspectEvioEvent()` asserts
that identity on every received event — a short or over-long delivery is the
one thing the file reader cannot get wrong and the network can.

So the output is **a complete EVIO v4 block that holds a single EVIO event**.
It is not a bare event without a block header, and it is not a CODA time-frame
object. No conversion or additional framing is needed to hand it to an EVIO
reader; the bytes are already a self-framing EVIO block.

Per-block PET/SRO content (from `SroWireFormat.hpp`): word 9 upper half is the
rocid, word 13 the frame counter, words 14/15 the 64-bit nanosecond timestamp,
words 18+ the slow-controls bank and the TDC hit words.

---

## 2. Data-type compatibility

### 2.1 `CodaTImeFrameBinaryDataType.cpp` vs the reassembled output

They are **unrelated representations**, and one is not a re-framing of the
other:

* `binary/coda-time-frame` carries a *decoded object graph* — time frames → ROC
  banks → `FADCHit{crate, slot, channel, charge, time}` — laid out as counts
  followed by struct-of-arrays. There is no EVIO magic, no block header, no
  block-length word anywhere in it.
* The reassembled buffer is *undecoded EVIO bytes*.

Turning one into the other needs a full EVIO → hit decode. That decode is not
possible from this repository: it requires the bit layout of the FEB TDC hit
words at word 24 and beyond, which lives in `org.jlab.detimg.petiroc`
(PetirocJava). **That tree is not present in this checkout**, and
`SroWireFormat.hpp` names the hit words without decoding them. Nothing here
guesses at it — `configure()` rejects `output-mime: binary/coda-time-frame`
with an explanation instead.

### 2.2 `CodaTImeFrameBinaryDataType.cpp` vs `CodaTimeFrameBinaryDataType.java`

| Aspect | C++ (before) | C++ (after fix) | Java |
| --- | --- | --- | --- |
| MIME id | `binary/coda-time-frame` | same | `binary/coda-time-frame` ✔ |
| Field order | counts → rocId, frameNumber, timeStamp, hitCount → SoA hits | same | same ✔ |
| Field widths | 32/64-bit as Java | same | same ✔ |
| **Byte order** | **little-endian** ✗ | **big-endian** ✔ | big-endian (`DataOutputStream`) |
| Length prefix / envelope | none | none | none ✔ |

The byte order was a real, total interop break: the same one-hit event
serialized 52 bytes on both sides, but not the same 52 bytes, and Java reading
the C++ buffer decoded `timeFrameCount = 16777216` instead of `1`.

`src/CodaTImeFrameBinaryDataType.cpp` in `ersap-actor` has been changed to
write and read big-endian, matching Java. Verified byte-for-byte against a real
`DataOutputStream` run:

```
000000010000000100000007000000030000011f71fb04cb00000001
00000001000000020000000300000004000000014b230ce3
```

The same edit also fixes two latent defects in the reader: `buffer[offset] << 24`
shifted into the sign bit of `int` (undefined behaviour for byte values ≥ 0x80),
and an unchecked count word let a corrupt buffer drive an arbitrary allocation.

**Compatibility risk.** Write and read change together in one shared library, so
any pair of C++ actors rebuilt against it stays consistent, and C++↔Java was
already broken in both directions, so nothing that worked stops working. The
only thing this breaks is a *persisted* little-endian `binary/coda-time-frame`
buffer written by an older build. If such data exists, the backward-compatible
route is to keep the fixed big-endian type under the shared MIME name and read
legacy files through a separate, explicitly-named legacy decoder — not to make
the live wire format ambiguous by sniffing the first word.

### 2.3 `PetMultiStreamSourceEngine.java`

| Question | Finding |
| --- | --- |
| Declared output type | `JavaObjectType.JOBJ` — MIME `binary/data-jobj`, serializer `EngineDataType.BYTES.serializer()` (raw bytes). **Not** `CodaTimeFrameBinaryDataType` |
| Type actually used when publishing | The same `JOBJ`, via `AbstractEventReaderService` |
| Object actually returned | `Object[]`, one element per module, each an `org.jlab.ersap.actor.pet.source.Event` — the live LMAX Disruptor ring-buffer slot returned by `AbstractConnectionHandler.getNextEvent()`, or `null`. Not `byte[]`: the array is two levels down, inside `Event.data`. The raw-bytes serializer cannot serialize an `Object[]`, so the declared type and the emitted object do not agree |
| Underlying bytes | `SocketConnectionHandler.receiveData()` does one `InputStream.read(byte[1024])` per event, so each `Event.data` is an arbitrary ≤1024-byte slice of a TCP stream — no EVIO block boundary, no length framing. `getNextEvent()` hands back the live slot, so the producer can overwrite it under a consumer still holding the reference |
| Structure emitted | Neither a complete EVIO record, nor an EVIO event, nor a CODA time frame |
| Consistent with `evio_ejfat_recv` output? | No — arbitrary stream slices vs. whole EVIO blocks |
| Can the C++ actor replace it? | **Yes, and it is strictly better.** Its downstream consumers now accept the actor's output: `PetGeometryProcessorEngine` declares `EvioBlockDataType.EVIO_BLOCK` alongside `JavaObjectType.JOBJ` and echoes the type it received, and `PetStreamSinkEngine` takes its accepted type from a `dataType` key (default `binary/data-evio`). `PetSinglesProcessorEngine` decodes the block directly. The simulator chain keeps working by setting `dataType: binary/data-jobj` in its writer block |

There is therefore no byte-for-byte match to preserve. `PetMultiStreamSourceEngine`
adds no framing that the actor must reproduce, and removes the EVIO framing the
actor preserves.

Two further notes on the Java side, left unchanged because fixing them is
outside this task's scope:

* `JavaObjectType`'s private `JavaSerializer` (Java-object serialization) is
  dead code — `JOBJ` is constructed with `EngineDataType.BYTES.serializer()`.
  That is what makes the C++ interop work, so it is load-bearing, not a bug to
  "fix" casually.
* `SocketConnectionHandler.receiveData()` opens a `DataInputStream` in a
  try-with-resources block per call, which closes the socket's input stream
  after the first read.

### 2.4 What the Java side sees, and the byte-order trap

`ersap-cpp`'s raw-bytes serializer and Java's `RawBytesSerializer` both use the
byte sequence itself as the wire image — Java's `read(ByteBuffer)` is literally
`return data;`. Java wraps the received `byte[]` with `ByteBuffer.wrap()`, so
the consumer sees `position = 0`, `limit = capacity = payload length`: only
valid payload bytes, no unused capacity, no length prefix. Signedness never
enters the wire; the payload is opaque bytes on both sides. The EJFAT event
number travels in the ERSAP communication id, not in the payload.

**But the ByteBuffer's byte order arrives wrong.** `DataUtil.deserialize()`
does:

```java
ByteBuffer bb = ByteBuffer.wrap(msg.getData());
if (metadata.getByteOrder() == xMsgMeta.Endian.Little) {
    bb.order(ByteOrder.LITTLE_ENDIAN);
}
```

`xMsgMeta.byteOrder` is a proto2 `optional` with no explicit default over an
enum whose first constant is `Little = 1`, so an **unset** field reads back as
`Little` — confirmed against the shipped jar (`hasByteOrder = false`,
`getByteOrder = Little`). `ersap-cpp` never sets it. Every buffer a C++ actor
publishes therefore reaches Java marked `LITTLE_ENDIAN`.

The bytes are correct and complete; only the order flag is wrong. The actor
cannot fix it from C++: `EngineData::meta_` is private, with only
`EngineDataAccessor` as a friend, so there is no public API to stamp the field.

That is why the default output type is **`binary/data-evio`**, whose Java
counterpart `org.jlab.ersap.actor.datatypes.EvioBlockDataType` restores
`BIG_ENDIAN` inside the deserializer, where no processing actor can forget it.
Measured through the real `DataUtil.deserialize()` on the first block of
`data/evio_192.168.0.13.bin`:

| Output type | `bb.order()` | `w0` | `w7` magic | Decodes as EVIO |
| --- | --- | --- | --- | --- |
| `binary/data-evio` | `BIG_ENDIAN` | 44 | `0xC0DA0100` | **yes** |
| `binary/data-jobj` | `LITTLE_ENDIAN` | 738197504 | `0x0001DAC0` | no |

Publishing `binary/data-jobj` still delivers the right bytes and remains a
drop-in for a chain that already declares `JavaObjectType.JOBJ` — but every
consuming actor must call `.order(ByteOrder.BIG_ENDIAN)` itself before reading
a word.

## 3. Configuration

Keys are the long option names of `evio_ejfat_recv` with the leading `--`
removed, and the defaults and semantics are the executable's.

| Key | Type | Default | Meaning | Validation |
| --- | --- | --- | --- | --- |
| `uri` | string | *required* | EJFAT URI. Without `withcp` only its `data=` address is used | non-empty; parsed by `e2sar::EjfatURI::getFromString`, the same call the executable makes |
| `recv-ip` | string | `127.0.0.1` | local IP to listen on | non-empty; parsed by `boost::asio::ip::make_address` |
| `recv-port` | integer | `10000` | starting UDP port; must match the sender's `data=` port | 1 … 65535 |
| `recv-threads` | integer | `1` | number of reassembly threads | ≥ 1 (and ≤ 1024) |
| `event-timeout` | integer | `500` | ms before an incomplete event is abandoned | > 0 |
| `poll-timeout` | integer | `1000` | ms `recvEvent()` waits before returning without an event | > 0 |
| `withcp` | boolean | `false` | use the EJFAT control plane | boolean |
| `novalidate` | boolean | `false` | skip control-plane SSL certificate validation. **Applies only when `withcp` is true**, and never to EVIO payload validation | boolean |
| `max-events` | unsigned | `0` | stop receiving after this many complete events; `0` runs until the actor is stopped | ≥ 0 |
| `stats-interval` | unsigned | `5` | seconds between progress messages; `0` disables them | ≥ 0 |
| `verbose` | boolean | `false` | log one line per received event | mutually exclusive with `quiet` |
| `quiet` | boolean | `false` | log only warnings and errors | mutually exclusive with `verbose` |

### Actor-specific keys

These have no counterpart in `evio_ejfat_recv` and exist only for the ERSAP
lifecycle or for the validation requirement:

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `validation` | string | `structural` | payload validation depth. `none`: decode only. `structural`: minimum size, EVIO magic, and declared block length vs. delivered byte count must agree; timestamp regressions and frame-counter gaps are counted and logged but the event is still published. `strict`: additionally checks the block-header length word (must be 8) and the EVIO version (must be 4), and a timestamp regression or a frame-counter gap also condemns the event |
| `output-mime` | string | `binary/data-evio` | published ERSAP data type. `binary/data-evio` (default): pairs with `EvioBlockDataType`, which hands Java a `BIG_ENDIAN` buffer. `binary/data-jobj`: pairs with `JavaObjectType.JOBJ`, drop-in for the existing chain, consumer must set the byte order itself. `binary/bytes`: `ersap::type::BYTES`, for chaining behind another native actor |
| `queue-size` | integer | `256` | events buffered between the receive thread and `execute()` |

An unrecognised key is reported on stderr and ignored. Any validation failure
returns `EngineStatus::ERROR` with the reason **and** prints the full key table
(`EjfatReceiverActor::configurationHelp()`), so an operator never has to read
the source to find a key name.

---

## 4. Runtime behaviour

* **Threading.** A dedicated pump thread owns the `EjfatReceiver` and drains it
  continuously into a bounded queue; `execute()` only pops. No ERSAP control or
  lifecycle thread ever sits inside `recvEvent()`, and E2SAR's internal receive
  queues keep draining even when the downstream chain stalls. The pump never
  blocks longer than 200 ms in one `recvEvent()` call, so a stop request is
  acted on promptly however large `poll-timeout` is.
* **Back pressure.** A full queue drops the *oldest* event and counts it, which
  bounds memory at `queue-size × event-size` and keeps the newest data flowing.
* **Poll-timeout expiry** is not an error: `execute()` returns
  `EngineStatus::WARNING` with no data, and nothing is logged.
* **`max-events`** stops the receive loop cleanly. The actor stays alive, the
  queue drains, and the ERSAP process is not terminated.
* **Errors.** A transport error is counted and logged at most once every 5 s; a
  malformed payload at most once a second. Nothing throws out of `execute()`.
* **Shutdown.** `reset()` and the destructor both call `shutdown()`, which stops
  the pump thread, prints the final statistics, deregisters the control-plane
  worker, stops the E2SAR threads and clears the queue. Idempotent and
  `noexcept`; no buffer, socket or thread is leaked.
* **Copies.** Exactly one copy of each payload, out of the `new[]` buffer E2SAR
  hands over — no container can adopt that allocation. From there it is moved
  into the `EngineData` and moved again by the serializer's rvalue overload.

---

## 5. Building

Needs both E2SAR (via `pkg-config`, same `e2sar.pc` as the rest of the tree) and
an `ersap-cpp` installation:

```sh
cmake -S cpp -B build -DERSAP_HOME=$ERSAP_HOME
cmake --build build -j
cmake --install build --prefix $ERSAP_HOME
```

When `ersap-cpp` is absent the actor is skipped with a message and everything
else still builds. `-DPETSRO_BUILD_ERSAP_ACTOR=OFF` skips it explicitly.
