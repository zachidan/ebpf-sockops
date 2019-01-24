# eBPF sockops samples

This repo contains samples for eBPF sockops programs.

The sample programs redirect traffic from the sender's socket (egress) to the receiver's socket (ingress) **skipping on the TCP/IP network kernel stack**. In this sample we assume that both the sender and the receiver are running on the **same** machine.

## What is eBPF?

An eBPF program is "attached" to a designated code path in the kernel. When the code path is traversed, any attached eBPF programs are executed. Given its origin, eBPF is especially suited to writing network programs and it's possible to write programs that attach to a network socket to filter traffic, to classify traffic, and to run network classifier actions. It's even possible to modify the settings of an established network socket with an eBPF program. 

## Running the example

### Build and load the eBPF programs

```shell
sudo ./load.sh
```

You can use [bpftool utility](https://github.com/torvalds/linux/blob/master/tools/bpf/bpftool/Documentation/bpftool-prog.rst) to check that the two eBPF programs are loaded.

```shell
sudo bpftool prog show
```

### Run the [iperf3](https://iperf.fr/) server

```shell
iperf3 -s -p 10000
```

### Run the [iperf3](https://iperf.fr/) client

```shell
iperf3 -c 127.0.0.1 -t 10 -l 64k -p 10000
```

### Collect tracing

```shell
./trace.sh
	iperf3-9516  [001] .... 22500.634108: 0: <<< ipv4 op = 4, port 18583 --> 4135
	iperf3-9516  [001] ..s1 22500.634137: 0: <<< ipv4 op = 5, port 4135 --> 18583
	iperf3-9516  [001] .... 22500.634523: 0: <<< ipv4 op = 4, port 19095 --> 4135
	iperf3-9516  [001] ..s1 22500.634536: 0: <<< ipv4 op = 5, port 4135 --> 19095
```
You should see 4 events for socket establishment. If you don't see any events then the eBPF program is NOT attached properly.

### Unload the eBPF programs

```shell
sudo ./unload.sh
```
