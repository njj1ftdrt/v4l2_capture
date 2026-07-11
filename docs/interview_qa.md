
## Why validate the TCP header before allocating the payload?

`payload_size` comes from the network and must be treated as untrusted input. Allocating a vector directly from that value can cause excessive memory use or process termination. The receiver therefore checks the protocol identity, version, dimensions, format-specific size relationship, and a configurable maximum payload before any payload allocation. CRC remains a later integrity check after the payload has been received.
