Use these commands:

Running udp echo experiment with kernel `socket` implementation using AccNIC:
```shell
./udp_client_kernel --ntest 64 --payload-size 64
```

UDP echo experiment (Blocking) with the offload engine in AccNIC:
```shell
./udp_exp --src-ip 10.0.0.1 --src-port 1234 --dst-ip 10.0.0.2 --dst-port 1111 --ntest 64 --mode block --payload-size 64
```

UDP echo experiment (Polling) with the offload engine in AccNIC:
```shell
./udp_exp --src-ip 10.0.0.1 --src-port 1234 --dst-ip 10.0.0.2 --dst-port 1111 --ntest 64 --mode poll --payload-size 64
```

Simple ping:
```shell
ping -c5 10.0.0.2 
```