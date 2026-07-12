
## Why validate the TCP header before allocating the payload?

`payload_size` comes from the network and must be treated as untrusted input. Allocating a vector directly from that value can cause excessive memory use or process termination. The receiver therefore checks the protocol identity, version, dimensions, format-specific size relationship, and a configurable maximum payload before any payload allocation. CRC remains a later integrity check after the payload has been received.

## Why support multiple receiver sessions before sender-side replay?

The receiver can safely return to `accept()` after a clean TCP close because the previous byte stream has ended and a new connection starts with a fresh frame boundary. Sender-side replay after an interrupted frame is more ambiguous: the sender may not know how much of the header or payload the peer received, so blind retransmission can create duplicates or stream misalignment. The project therefore adds bounded sequential receiver sessions first and keeps corrupted-stream handling fatal.

## Why retry only the initial TCP connection?

Before any frame is sent, retrying `connect()` is unambiguous: either a connection is established or no application bytes were delivered. After a header or payload send fails, the sender cannot know how many bytes reached the peer. Blindly reconnecting and retransmitting can produce duplicate frames or protocol stream misalignment. Therefore the current implementation bounds initial connection attempts and reports mid-stream failures without automatic frame retransmission.

## Why connect before starting producer and consumer threads?

If capture continues while the receiver is unavailable, the TCP RingBuffer can fill and discard frames before a connection is established. The pipeline now completes bounded connection establishment first, then starts the application threads, so startup recovery does not create queue loss.
