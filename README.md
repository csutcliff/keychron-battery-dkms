# keychron-battery-dkms

Linux kernel module for Keychron mouse battery reporting via the power_supply subsystem.

## Overview

Keychron mice don't expose battery level via standard HID battery reports. This kernel module implements Keychron's vendor-specific HID protocol to query battery status and exposes it through the Linux power_supply subsystem, enabling native integration with desktop environments like KDE Plasma, GNOME, etc.

## Supported Devices

| Device | USB ID | Mode |
|--------|--------|------|
| Keychron M5 | `3434:d048` | Wired |
| Keychron Ultra-Link 8K | `3434:d028` | Wireless receiver |

Other Keychron mice using the same protocol may also work.

## Installation

### Arch Linux (AUR)

```bash
yay -S keychron-battery-dkms
# or
paru -S keychron-battery-dkms
```

### Manual (DKMS)

```bash
git clone https://github.com/csutcliff/keychron-battery-dkms.git
cd keychron-battery-dkms
makepkg -si
```

### Manual (without DKMS)

```bash
make
sudo insmod keychron_battery.ko
```

## Usage

Once installed, the module loads automatically when a supported device is connected. Battery information is available at:

```bash
# Battery percentage
cat /sys/class/power_supply/keychron_mouse/capacity

# All properties
cat /sys/class/power_supply/keychron_mouse/uevent

# Via UPower
upower -i /org/freedesktop/UPower/devices/battery_keychron_mouse
```

Desktop environments with battery widgets (KDE, GNOME, etc.) will automatically display the mouse battery.

## How It Works

1. Module binds to Keychron USB devices on interface 4 (vendor-specific HID)
2. Sends status request (report ID `0xB3`, command `0x06`) via USB control endpoint
3. Receives battery response (report ID `0xB4`) via USB interrupt endpoint
4. Polls every 5 minutes to update battery level
5. Exposes battery via power_supply subsystem → UPower → desktop widget

## Troubleshooting

```bash
# Check if module is loaded
lsmod | grep keychron

# View kernel messages
journalctl -k | grep keychron

# Check DKMS status
dkms status keychron-battery

# Manually load module
sudo modprobe keychron_battery
```

## License

GPL-2.0-only

## Author

Chris Sutcliff <chris@sutcliff.me>
