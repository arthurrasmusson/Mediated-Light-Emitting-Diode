#!/usr/bin/env python3
"""
scheduler.py
============

Daemon that sits on the HOST, opens every vfio-mdev instance created
from mled-vfio.ko, reads the single data byte guests write,
and forwards the *latest* value to the same driver via its “vendor/value”
sysfs attribute (which toggles the GPIO pin).

One scheduler handles an arbitrary number of VMs.
"""

import os, select, fcntl, glob, sys

VFIO_PATH = "/sys/bus/mdev/devices"

# --------------------------------------------------------------------------- #
# Helper: make /dev/vfio/<group> non-blocking and register in poller
def open_vfio_fd(uuid):
    group = os.readlink(os.path.join(VFIO_PATH, uuid, "iommu_group")).split('/')[-1]
    fd = os.open(f"/dev/vfio/{group}", os.O_RDWR | os.O_NONBLOCK)
    return fd

# --------------------------------------------------------------------------- #
def main():
    uuids = [os.path.basename(p) for p in glob.glob(f"{VFIO_PATH}/*")]
    if not uuids:
        print("scheduler: no mediated devices found", file=sys.stderr)
        return

    poller = select.poll()
    uuid_for_fd = {}
    for u in uuids:
        fd = open_vfio_fd(u)
        poller.register(fd, select.POLLIN)
        uuid_for_fd[fd] = u
        print(f"scheduler: watching {u}")

    print("scheduler: waiting for guest writes …")
    while True:
        for fd, ev in poller.poll():
            if ev & select.POLLIN:
                try:
                    data = os.read(fd, 1)   # our region is only one byte
                except BlockingIOError:
                    continue
                if data:
                    val = data[0] & 1
                    uuid = uuid_for_fd[fd]
                    # write back into sysfs → kernel driver updates GPIO
                    with open(f"{VFIO_PATH}/{uuid}/vendor/value", "wb", buffering=0) as f:
                        f.write(bytes([val]))
                    print(f"[{uuid}] LED set to {val}")

if __name__ == "__main__":
    main()

