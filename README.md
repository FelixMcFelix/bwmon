# Bandwidth Monitor
A simple bandwidth monitor written in C++.

## Requirements
* gcc 7+
* libpcap

## Usage

As with many pcap applications, this typically must be run with `sudo`.

### Find Interfaces
To list all interfaces available for reading from:

```sh
./bwmon
```

### Tracking
To track bandwidth over i.e., `intf1` and `intf2`, type:

```sh
./bwmon intf1 intf2
```

Pressing enter/return will print the elapsed measurement time in nanoseconds, followed by a list of pairs for each interfaces: the count of bytes seen from 'even' and 'odd' IP addresses.
This exists more as a demonstration of how you might 'mod in' a packet classifier.