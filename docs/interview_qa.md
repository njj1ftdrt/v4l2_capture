
## Why validate the TCP header before allocating the payload?

`payload_size` comes from the network and must be treated as untrusted input. Allocating a vector directly from that value can cause excessive memory use or process termination. The receiver therefore checks the protocol identity, version, dimensions, format-specific size relationship, and a configurable maximum payload before any payload allocation. CRC remains a later integrity check after the payload has been received.

## Why support multiple receiver sessions before sender-side replay?

The receiver can safely return to `accept()` after a clean TCP close because the previous byte stream has ended and a new connection starts with a fresh frame boundary. Sender-side replay after an interrupted frame is more ambiguous: the sender may not know how much of the header or payload the peer received, so blind retransmission can create duplicates or stream misalignment. The project therefore adds bounded sequential receiver sessions first and keeps corrupted-stream handling fatal.
