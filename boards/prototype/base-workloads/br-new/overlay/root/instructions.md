Use these commands:

Make files executable:
```shell
chmod +x udp_* interrupt_exp
```

(Optional) Pin AccNIC's interrupts to CPU 0. You may use `echo '0-3' >` to assign them to all cores.
```shell
echo 0 | tee /proc/irq/12/smp_affinity_list # console
echo 0 | tee /proc/irq/16/smp_affinity_list # accnet
echo 0 | tee /proc/irq/17/smp_affinity_list # accnet
echo 3 | tee /proc/irq/14/smp_affinity_list # iocache
echo 3 | tee /proc/irq/15/smp_affinity_list # iocache
```

To get CPU utilization measurements prepend the following to your command:
```shell
time -f 'avg CPU: %P  (elapsed=%E)' 
```

`chrt -f 5` 

Running udp echo experiment with kernel `socket` implementation using AccNIC:
```shell
./udp_client_kernel --ntest 64 --payload-size 64
```

UDP echo experiment (Blocking) with the offload engine in AccNIC:
```shell
./udp_exp --src-mac 00:0a:35:06:4d:e2 --src-ip 10.0.0.1 --src-port 1234 --dst-mac 0c:42:a1:a8:2d:e6 --dst-ip 10.0.0.2 --dst-port 1111 --ntest 64 --mode block --payload-size 64
./udp_exp --src-ip 10.0.0.1 --src-port 1234 --dst-ip 10.0.0.2 --dst-port 1111 --ntest 64 --mode block --payload-size 64 --cpu 3
```

UDP echo experiment (Polling) with the offload engine in AccNIC:
```shell
./udp_exp --src-mac 00:0a:35:06:4d:e2 --src-ip 10.0.0.1 --src-port 1234 --dst-mac 0c:42:a1:a8:2d:e6 --dst-ip 10.0.0.2 --dst-port 1111 --ntest 64 --mode poll --payload-size 64
./udp_exp --src-ip 10.0.0.1 --src-port 1234 --dst-ip 10.0.0.2 --dst-port 1111 --ntest 64 --mode poll --payload-size 64 --cpu 3
```

Simple ping:
```shell
ping -c5 10.0.0.2 
```
