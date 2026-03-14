#!/bin/bash
# Jetson robot internal network setup (enP8p1s0 → 10.0.10.1/24)

sudo nmcli connection add type ethernet ifname enP8p1s0 con-name robot-internal ipv4.method manual ipv4.addresses 10.0.10.1/24 ipv4.never-default yes ipv6.method disabled
sudo nmcli connection up robot-internal
ip addr show enP8p1s0
