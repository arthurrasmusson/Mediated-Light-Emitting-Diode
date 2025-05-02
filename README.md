# MLED – Mediated Light Emitting Diode

## Control an LED from an arbitrary number of QEMU/KVM guests (Raspberry Pi 3/4/4)

This repo contains the **full vertical stack** for a *mediated* GPIO
device:

* **Kernel module** (`mled‑vfio.ko`) – exposes a 1‑byte “BAR”
  that guests can write (`0 = off, 1 = on`).
* **Host scheduler** (`host/scheduler.py`) – listens to *all* mdev fds and
  mirrors the most recent value to a physical GPIO pin.
* **Guest helper** (`guest/ledctl.c`) – convenience CLI; `ledctl /dev/vfio/N 1`.

---

## 1 Hardware checklist & requirements

| component | details |
|-----------|---------|
| **Board** | Raspberry Pi 3 B/ 3 A+, Pi 4/400/CM4, or Pi 5 – must be **Armv8‑A** so that the CPU supports the Virtualisation Extensions needed by KVM. |
| **Power** | Good 3 A USB‑C supply (Pi 4/5) – heavy I/O in KVM guests can brown‑out a weak PSU. |
| **Storage** | 16 GB+ micro‑SD or SSD; you’ll store at least two guest images. |
| **LED + 330 Ω resistor** | Wire LED **anode → GPIO 17** (physical pin 11), **cathode → GND** (pin 9).<br>If you prefer another pin, edit the `gpiod_get(&…, "status"…)` line in the kernel module. |
| **Cooling** | A small heat‑sink is advised – two VMs doing CPU work will push a Pi 4/5 past 70 °C. |

---

## 2 Prepare a 64‑bit Raspbian / Raspberry Pi OS with KVM

### 2.1 Flash the 64‑bit image

```bash
# run on your PC
sudo rpi-imager            # choose Raspberry Pi OS 64‑bit (Bookworm or later)
```

Boot the Pi, expand the filesystem (`raspi-config → Advanced → expand`).

### 2.2 Update & install toolchain

```bash
sudo apt update && sudo apt full-upgrade -y
sudo apt install \
        build-essential raspberrypi-kernel-headers \
        libgpiod-dev python3 python3-libgpiod \
        qemu-system-aarch64 qemu-efi-aarch64 mdevctl
```

### 2.3 Enable KVM (64‑bit & EL2)

Edit `/boot/config.txt` and make sure these lines exist **once**:

```
# ---- enable 64‑bit kernel & KVM ----
arm_64bit=1              # forces 64‑bit kernel even on Pi 3B
kernel=kernel8.img       # optional – Bookworm does this automatically
# Optional: enable USB host overlay on Pi 4 if you need virtio‑usb
dtoverlay=dwc2,dr_mode=host
```

Reboot and verify:

```bash
uname -m                  # must print aarch64
zgrep CONFIG_KVM /proc/config.gz | grep '=y'
sudo modprobe kvm
sudo modprobe kvm_arm
dmesg | grep -i "kvm.*hyp" # expect “HYP mode initialized”
```

If you see *“HYP mode not available”* you are on a 32‑bit kernel or an
older Pi 2 (Cortex‑A7).

---

## 3 Build & load the kernel module

```bash
git clone https://github.com/arthurrasmusson/Mediated-Light-Emitting-Diode.git
cd Mediated-Light-Emitting-Diode/kernel
make                             # builds mled-vfio.ko
sudo insmod mled-vfio.ko
```

Check that the mdev type appears:

```
/sys/devices/virtual/vfio-gpio-led/vfio-gpio-led/
└── mdev_supported_types/led-1/create
```

---

## 4 Create one or more mediated devices

```bash
export UUID0=$(uuidgen)          # store for later
export UUID1=$(uuidgen)

sudo mdevctl start -u $UUID0 -t led-1
sudo mdevctl start -u $UUID1 -t led-1
```

Each command prints the IOMMU‑group; the vfio device is automatically
bound.

---

## 5 Run the host scheduler daemon

```bash
cd ../host
sudo python3 scheduler.py
#   watching $UUID0
#   watching $UUID1
#   waiting for guest writes …
```

The daemon needs root because it opens the vfio fds.

---

## 6 Launch VMs with the LED device attached

Create or download an **aarch64** guest image (e.g. Alpine).
Pass each VM its own mdev:

```bash
qemu-system-aarch64 \
  -M virt,accel=kvm -cpu host -smp 2 -m 1024 \
  -drive if=virtio,file=alpine.img \
  -device vfio-pci,sysfsdev=/sys/bus/mdev/devices/$UUID0

qemu-system-aarch64 \
  -M virt,accel=kvm -cpu host -smp 2 -m 1024 \
  -drive if=virtio,file=alpine.img \
  -device vfio-pci,sysfsdev=/sys/bus/mdev/devices/$UUID1
```

Inside the VM the device usually lands on `/dev/vfio/0` (or `/dev/vfio/1`
if a disk is already assigned).

---

## 7 Build the guest helper & blink

```bash
# inside each VM
scp ledctl.c .
gcc -O2 -o /usr/local/bin/ledctl ledctl.c

# LED on
ledctl /dev/vfio/0 1
# LED off
ledctl /dev/vfio/0 0
```

The scheduler prints:

```
[$UUID0] LED set to 1
[$UUID1] LED set to 0
```

The physical LED follows the *last* writer.

---

## 8 Cleaning up

```bash
sudo mdevctl stop $UUID0
sudo mdevctl stop $UUID1
sudo rmmod mled_vfio
```

---

## 9 Ideas for extension

* Use PWM (`pwm-fan` overlay) to dim an RGB strip.
* Add an IRQ back‑channel (eventfd) so the host can signal the guest.
* Implement live‑migration – copy the logic from
  `samples/vfio-mdev/mtty.c`.

Happy mediated light emitting! ✨
