#!/bin/bash
set -e

source /opt/ros/jazzy/setup.bash
source /root/talicska-ws/install/setup.bash

exec "$@"
