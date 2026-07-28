# Reliable Transport Protocol over UDP

A custom, TCP-like reliable transport protocol (WTP) implemented from scratch on top of raw UDP sockets in C++. It guarantees in-order, lossless file delivery over a network that can drop, delay, duplicate, and reorder packets — using nothing but UDP.

## Overview

UDP provides no delivery guarantees on its own: packets can be lost, arrive out of order, arrive twice, or arrive late. This project implements the reliability layer that UDP is missing — a sliding-window protocol with checksums, cumulative acknowledgements, and timeout-based retransmission — closely mirroring how TCP solves the same problem.

The project includes both a **sender** (`wSender`) and a **receiver** (`wReceiver`), plus an optimized variant of each that avoids unnecessary retransmissions under packet loss.

## How It Works

**Packet format:** every packet carries a type (`START` / `DATA` / `END` / `ACK`), a sequence number, a length, and a 32-bit CRC checksum computed over the payload.

**Connection lifecycle:** a connection opens with a `START` handshake, transfers the file as a stream of `DATA` packets, and closes with an `END` message — each of which is itself acknowledged, since any packet (including control packets) can be lost.

**Sliding window:** the sender keeps a configurable number of packets in flight at once rather than waiting for each ACK individually, which keeps throughput high on links with real round-trip latency. The receiver sends cumulative ACKs — "I've received everything up to sequence number N" — so a single ACK can confirm multiple packets at once.

**Loss recovery:** the sender runs a 500ms retransmission timer that resets every time the window advances. If the timer fires before the window moves, every unacknowledged packet in the window is resent.

**Corruption handling:** the receiver independently recomputes each packet's checksum; a mismatch causes the packet to be silently dropped (no ACK sent), which naturally triggers the sender's retransmission logic.

**Optimized mode (`wSenderOpt` / `wReceiverOpt`):** the baseline protocol's cumulative ACKs can cause unnecessary retransmission. Here's the failure case, with a window size of 3:

Packet 0 is lost in transit, but packets 1 and 2 both arrive safely:

<img src="img/base_case.PNG" alt="Sender window holding packets 0, 1, 2; packet 0 is dropped in transit while packets 1 and 2 successfully reach the receiver window" title="Inefficient transfer of data" width="250" height="250"/>

Because baseline `wReceiver` only sends *cumulative* ACKs, it can't say "I got 1 and 2" — every ACK it sends still just says "I'm waiting on 0." The sender sees no forward progress, its timer fires, and it retransmits **all three** packets — even though 1 and 2 already arrived safely.

`wReceiverOpt` fixes this by ACKing packets individually instead of cumulatively:

<img src="img/improvement.PNG" alt="Receiver individually ACKs packet 1 and packet 2 with their own sequence numbers, rather than one cumulative ACK" title="Only ACK necessary data" width="250" height="250"/>

`wSenderOpt` tracks these per-packet ACKs and maintains an independent timer for each packet in the window, so a timeout on packet 0 no longer drags 1 and 2 down with it:

<img src="img/improvement_2.PNG" alt="Sender marks packets 1 and 2 as acknowledged in its buffer and, after its timer expires, retransmits only packet 0" title="Only send unACKed data" width="250" height="250"/>

The result: only the packet that actually needs it gets resent.

## Usage

**Receiver:**
```bash
./wReceiver -p 8000 -w 10 -d /tmp -o receiver.log
```

**Sender:**
```bash
./wSender -h 127.0.0.1 -p 8000 -w 10 -i input.file -o sender.log
```

| Flag | Meaning |
|---|---|
| `-h` / `--hostname` | Receiver's IP address (sender only) |
| `-p` / `--port` | Port to send to / listen on |
| `-w` / `--window-size` | Max outstanding unacknowledged packets |
| `-i` / `--input-file` | File to transfer (text or binary) |
| `-o` / `--output-log` | Path to write the packet activity log |
| `-d` / `--output-dir` | Directory to write received files (receiver only) |

Every packet sent or received is logged as `<type> <seqNum> <length> <checksum>` for debugging and verification. The receiver can service multiple sequential connections, saving each transferred file separately.

## Building

```bash
mkdir build && cd build
cmake ..
make
```

Produces `wSender`, `wReceiver`, `wSenderOpt`, and `wReceiverOpt` binaries.

## Design Notes

- Built entirely on raw UDP sockets — no TCP sockets are used anywhere, since the whole point is implementing the reliability guarantees TCP normally provides for you.
- Handles arbitrary packet loss, reordering (including of ACKs), duplication, and delay.
- Packet size is capped to fit within a standard Ethernet frame (1472 bytes of payload after UDP/IP headers), matching real-world MTU constraints.

## Technologies

C++, UDP sockets, CMake, `spdlog`, `cxxopts`

## Author

**Mitul Goel** — [GitHub](https://github.com/Mitulol) · [LinkedIn](https://linkedin.com/in/mitul-goel)
