# Running `evio_ejfat_replay` in a container

This image builds and runs the C++ tree under [`cpp/`](cpp/) — the EVIO replay
sender `evio_ejfat_replay` and its counterpart `evio_ejfat_recv`. Nothing about
the programs changes in a container; see [`cpp/README.md`](cpp/README.md) for
the behaviour and [`cpp/HOWTO.md`](cpp/HOWTO.md) for the loopback walkthrough.

The point of the image is that the EJFAT stack is awkward to install: E2SAR
needs gRPC 1.74.1, protobuf 31.1.0 and **exactly** Boost 1.89.0, none of which
match what a distribution ships. The image installs Jefferson Lab's own E2SAR
release package, which carries them all, so the host needs nothing but Docker.

---

## What is inside

| | |
|---|---|
| Base | `ubuntu:24.04`, pinned by digest |
| E2SAR | `0.3.2`, official `e2sar_0.3.2_amd64.deb` from the `E2SAR-0.3.2-main-ubuntu-24.04` release, verified by SHA-256 |
| gRPC / protobuf / Abseil / Boost / c-ares | 1.74.1 / 31.1.0 / 20250512 / 1.89.0 / 1.19.1, all from that package, installed under `/usr/local` |
| Build type | `Release`, out-of-source in `/src/build` |
| Executables | `/usr/local/bin/evio_ejfat_replay`, `/usr/local/bin/evio_ejfat_recv` |
| Entrypoint | `evio_ejfat_replay` (exec form, so it is PID 1 and gets `SIGINT`/`SIGTERM`) |
| Default command | `--help` |
| User | `petsro`, uid/gid `10001`, non-root |
| Working directory | `/data` — mount captures here |
| Size | ~160 MB (78 MB Ubuntu base, 67 MB gRPC/protobuf/Abseil/Boost, ~2.4 MB executables) |

The runtime stage carries only the two executables, the 96 shared objects
`ldd` resolves for them, `libre2` and the CA bundle. Executables and libraries
are both stripped — a `Release` build carries no debug info, so this removes
only symbol tables (worth 40 MB, because `libe2sar.a` is static). There is no
compiler, no header tree, no source and no package cache in the image.

**The image is `linux/amd64` only.** Jefferson Lab publishes E2SAR binaries for
x86_64 alone; an arm64 image would mean building E2SAR, gRPC and Boost from
source, which this Dockerfile deliberately does not do. It fails with an
explicit message on any other architecture rather than part-way through a
300 MB download.

---

## Build

```bash
docker build -t pet-sro/evio-ejfat-replay:latest .
```

### Building on Apple silicon or another non-amd64 host

Use `buildx` and name the platform. The build runs under emulation, so expect
it to take considerably longer than a native one.

```bash
docker buildx build --platform linux/amd64 \
    -t pet-sro/evio-ejfat-replay:latest --load .
```

The same flag is what you want when producing an image on a developer laptop
for an x86_64 DAQ node.

### Build options

Everything version-specific is a build argument, so a different E2SAR release
needs no edit to the Dockerfile:

```bash
docker buildx build --platform linux/amd64 \
    --build-arg E2SAR_VERSION=0.3.2 \
    --build-arg E2SAR_RELEASE_TAG=E2SAR-0.3.2-main-ubuntu-24.04 \
    --build-arg E2SAR_DEB=e2sar_0.3.2_amd64.deb \
    --build-arg E2SAR_DEB_SHA256=c0e43c4e8553af5c1bf23e1844418a40b9299a9cccd575ccf49fcda53d1f6e3e \
    -t pet-sro/evio-ejfat-replay:0.3.2 --load .
```

The unit tests run inside the builder stage, and the build fails if CMake did
not find E2SAR — an image can never ship a binary that silently cannot send.

---

## Show the help

The default command is `--help`, so this prints the full option reference:

```bash
docker run --rm --platform linux/amd64 pet-sro/evio-ejfat-replay:latest
```

Explicitly, and for the receiver:

```bash
docker run --rm --platform linux/amd64 pet-sro/evio-ejfat-replay:latest --help

docker run --rm --platform linux/amd64 \
    --entrypoint /usr/local/bin/evio_ejfat_recv \
    pet-sro/evio-ejfat-replay:latest --help
```

(Drop `--platform linux/amd64` when the host is already x86_64.)

---

## Run with EVIO captures mounted read-only

`WORKDIR` is `/data`, so file arguments can be relative. The captures stay on
the host; nothing is copied into the image.

Dry run first — this reads, synchronizes and does packet accounting but sends
nothing, so it needs no load balancer:

```bash
docker run --rm --network none \
    -v "$PWD/data:/data:ro" \
    pet-sro/evio-ejfat-replay:latest \
    --file-count 2 --dry-run --loop-limit 1 \
    evio_192.168.0.13.bin evio_192.168.0.16.bin
```

A real send, two streams, forever, until `Ctrl-C` (or `docker stop`):

```bash
docker run --rm -it --network host \
    -v "$PWD/data:/data:ro" \
    pet-sro/evio-ejfat-replay:latest \
    --file-count 2 \
    --uri 'ejfat://token@cp.example.org:18008/lb/1' \
    --file evio_192.168.0.13.bin \
    --file evio_192.168.0.16.bin \
    --mtu 9000 --rate 5.0 --sockets 4 --withcp \
    --stats-interval 5
```

### If the captures are not world-readable

The container runs as uid `10001`. Either make the files readable, or run as
your own uid — the program needs no privileges of its own:

```bash
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD/data:/data:ro" \
    pet-sro/evio-ejfat-replay:latest --file-count 1 --dry-run --loop-limit 1 f.bin
```

---

## Supplying EJFAT / load-balancer settings

**`evio_ejfat_replay` reads no environment variables.** Every setting is a
command-line option, and the load balancer is addressed entirely through the
single `--uri` value. The real options are:

| Option | Default | Meaning |
|---|---|---|
| `-u, --uri URI` | required unless `--dry-run` | EJFAT URI: token, control-plane host, `/lb/<id>`, and `sync=` / `data=` query parameters |
| `-c, --withcp` | off | Register this sender with the control plane |
| `--mtu N` | `1500` | Segmenter MTU, 576–9000; `0` auto-detects |
| `--rate F` | `1.0` | Send rate in Gbps; negative means unlimited |
| `--sockets N` | `4` | UDP send sockets the segmenter opens |
| `--dataid-base N` | `1` | `dataId` of the first file; file *i* gets `dataid-base + i` |
| `--eventsrcid N` | `1` | Event source id carried in the Sync header |
| `--entropy-per-source` | off | Pin each source to one LB path instead of randomising |
| `--async` | off | `addToSendQueue()` instead of inline `sendEvent()` |
| `-V, --novalidate` | off | Skip control-plane TLS certificate validation |

`--novalidate` exists for lab setups with a self-signed control plane. **Leave
it off in production** — the image ships the system CA bundle precisely so that
validation works.

### Keeping the token out of the image and out of shell history

The URI contains an instance token, so it must never be baked into the image or
written into a `Dockerfile`. Pass it at run time. The cleanest form uses an env
file plus a one-line `sh -c`, since the program itself has no env-var
interface:

```bash
# ejfat.env  (chmod 600, git-ignored, never COPYed into the image)
EJFAT_URI=ejfat://REDACTED_TOKEN@cp.example.org:18008/lb/1

docker run --rm -it --network host \
    --env-file ./ejfat.env \
    -v "$PWD/data:/data:ro" \
    --entrypoint /bin/sh \
    pet-sro/evio-ejfat-replay:latest \
    -c 'exec /usr/local/bin/evio_ejfat_replay \
            --file-count 2 --uri "$EJFAT_URI" --withcp --mtu 9000 \
            evio_192.168.0.13.bin evio_192.168.0.16.bin'
```

`exec` keeps the program as PID 1, so signal handling is unchanged.

---

## Networking

The program sends UDP outbound and, with `--withcp`, opens a gRPC/TLS
connection to the control plane. **It never listens on a port**, which is why
the image declares no `EXPOSE`.

* **`--network host`** is the right choice on a DAQ node. The load balancer
  routes on the source address it was registered with, jumbo frames need the
  host NIC's MTU, and NAT would rewrite what the control plane was told. This
  is Linux-only; on Docker Desktop for macOS or Windows `--network host` does
  not give the container the host's interfaces.
* **Default bridge** works for a dry run or for loopback tests confined to one
  container, but a replay through a real LB from behind the bridge NAT will
  usually not reach its destination correctly.
* **`--network none`** is what the smoke test uses: it guarantees the container
  cannot transmit.

`evio_ejfat_recv` *does* bind UDP ports — a range starting at `--recv-port`,
whose exact extent E2SAR chooses and prints at startup (`listening on <ip>
ports <first>:<last> ...`). Run it with `--network host`, or publish that range
explicitly with `-p`. Neither needs `EXPOSE`.

---

## Overriding the default command

The entrypoint is `evio_ejfat_replay`, so anything after the image name
replaces the default `--help` and becomes its arguments:

```bash
docker run --rm pet-sro/evio-ejfat-replay:latest --file-count 1 --dry-run --loop-limit 1 f.bin
```

To run a *different* program, override the entrypoint:

```bash
# The receiver
docker run --rm -it --network host \
    --entrypoint /usr/local/bin/evio_ejfat_recv \
    pet-sro/evio-ejfat-replay:latest \
    --uri 'ejfat://useless@127.0.0.1:9876/lb/1?sync=127.0.0.1:12345&data=127.0.0.1' \
    --recv-ip 127.0.0.1 --recv-port 10000 --recv-threads 1

# A shell, for poking around
docker run --rm -it --entrypoint /bin/bash pet-sro/evio-ejfat-replay:latest
```

### Signals

`ENTRYPOINT` is in exec form, so the program is PID 1 and receives signals
directly. `docker stop` sends `SIGTERM`, and `Ctrl-C` under `-it` sends
`SIGINT`; both are handled — files are closed, the sink drained and the final
statistics printed before exit. A second signal exits immediately with
`128 + signal`. Give the container time to finish: `docker stop -t 30 <name>`.

---

## Smoke test

[`cpp/tests/docker_smoke_test.sh`](cpp/tests/docker_smoke_test.sh) validates a
built image. Every container it starts runs with `--network none`, so it cannot
emit traffic; the only execution beyond `--help` is a `--dry-run` over a
two-frame synthetic capture the script generates itself.

```bash
cpp/tests/docker_smoke_test.sh pet-sro/evio-ejfat-replay:latest
```

It checks that both executables answer `--help`, that they exist at the
documented paths, that no shared library is unresolved, that the process runs
as a non-root uid, that no compiler, header tree or source survived into the
runtime image, and that a real `--dry-run` produces the expected event
accounting and that bad arguments are still rejected with exit status 2.

---

## Notes

* The image contains no EVIO data, no EJFAT URI, no token and no site
  configuration. All of it arrives at `docker run` time.
* `data/` is excluded by `.dockerignore`, so the captures never enter the build
  context.
* The ERSAP actor (`cpp/src/actor`) is **not** built here. It is a `dlopen()`ed
  plugin that has to be installed into `$ERSAP_HOME` alongside an `ersap-cpp`
  runtime; build it on the ERSAP host as `cpp/src/actor/README.md` describes.
* Real captures are tens of megabytes; mount them, and prefer `:ro`.
