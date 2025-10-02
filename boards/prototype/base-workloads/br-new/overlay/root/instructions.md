Use these commands:

Make files executable:
```shell
chmod +x udp_* run.sh server.sh
```

(Optional) Pin AccNIC's interrupts to CPU 0. You may use `echo '0-3' >` to assign them to all cores.
```shell
echo 0 | tee /proc/irq/12/smp_affinity_list # console
echo 0 | tee /proc/irq/22/smp_affinity_list # accnet
echo 0 | tee /proc/irq/23/smp_affinity_list # accnet
```

To get CPU utilization measurements prepend the following to your command:
```shell
time -f 'avg CPU: %P  (elapsed=%E)' 
```

Running udp echo experiment with kernel `socket` implementation using AccNIC:
```shell
./udp_client_kernel --ntest 64 --payload-size 64
./udp_server_kernel
```

To reset all:
```shell
./udp_exp --reset
```

UDP echo server (Blocking) with the offload engine in AccNIC:
```shell
chrt -f 5 ./udp_exp --src-mac 00:0a:35:06:4d:e2 --src-ip 10.0.0.1 --src-port 1234 --dst-mac b4:96:91:90:3c:81 --dst-ip 10.0.0.2 --dst-port 1111 --mode server --cpu 3 --ring 0
```

UDP echo experiment (Blocking) with the offload engine in AccNIC:
```shell
chrt -f 5 ./udp_exp --src-mac 00:0a:35:06:4d:e2 --src-ip 10.0.0.1 --src-port 1234 --dst-mac b4:96:91:90:3c:81 --dst-ip 10.0.0.2 --dst-port 1111 --ntest 2 --mode block --payload-size 64 --cpu 3 --ring 0
```

UDP echo experiment (Polling) with the offload engine in AccNIC:
```shell
./udp_exp --src-mac 00:0a:35:06:4d:e2 --src-ip 10.0.0.1 --src-port 1234 --dst-mac b4:96:91:90:3c:81 --dst-ip 10.0.0.2 --dst-port 1111 --ntest 2 --mode poll --payload-size 64 --cpu 3 --ring 0
```

Loopback (Blocking) with the offload engine in AccNIC:
```shell
./udp_exp --src-mac 00:0a:35:06:4d:e2 --src-ip 10.0.0.1 --src-port 1234 --dst-mac 00:0a:35:06:4d:e2 --dst-ip 10.0.0.1 --dst-port 1234 --cpu 3 --ring 0 --mode loop --payload-size 1024
```

server-to-server throughput (Blocking) with the offload engine in AccNIC:
```shell
chrt -f 5 ./udp_exp --src-mac 00:0a:35:06:4d:e2 --src-ip 10.0.0.1 --src-port 1234 --dst-mac b4:96:91:90:3c:81 --dst-ip 10.0.0.2 --dst-port 1111 --ntest 8 --mode sink --payload-size 4096 --cpu 3 --ring 0
```

Simple ping:
```shell
ping -c5 10.0.0.2 
```
