# Jack In USB gadget: setup

The PET presents itself to a computer as a USB device with three functions in one
configuration (c.1):

- RNDIS (`rndis.usb0`): Windows, with Microsoft OS descriptors so Windows binds it without a driver.
- ECM (`ecm.usb1`): native on macOS and Linux, no driver to install.
- Mass storage (`mass_storage.0`): the read-only ALICEPET drive with the Jack In apps.

Both network functions run over the same link to `10.55.0.1/30` on the PET, so the
computer gets `10.55.0.2` from dnsmasq on the bridge `br-jack`.

## Required `config.txt` line (the PET's boot config)

The Pi's USB-C port 2 must run in device mode. Add this under the `[all]` section of the
PET's `/boot/firmware/config.txt`:

```
[all]
dtoverlay=dwc2,dr_mode=peripheral
```

Do not edit `config.txt` from a script without reading it first. `bin/pet-setup.sh` does
not write this line; it has to be added by hand (or by a reviewed change to pet-setup).

## Hardware steps

1. Plug the computer into USB-C port 2 (the one wired to the gadget).
2. Put the USB-SW slider on port 2 into the device (PC) position.
3. Wait a few seconds for the drive to appear and the link to come up.

The USB-C mux LED (`/sys/class/leds/ext_usb_gpio_fun/brightness`) is set to 1 while the
gadget is bound and 0 when it is torn down. The script writes it only if the path exists,
so boards without that LED are unaffected.

## Verifying

- On the PET: `ip -4 addr show br-jack` shows `10.55.0.1`.
- On the computer: the ALICEPET drive mounts, and `10.55.0.2` is assigned on the network
  interface (ECM on macOS and Linux, RNDIS on Windows).
- Launch `JackIn.command` (macOS) or `JackIn.sh` (Linux), or `JackIn.ps1` on Windows.

## Windows risk

The configuration change in this revision only adds the unix client files to the drive
image and the mux LED write. The RNDIS function, its OS descriptors, and the Windows
launcher are unchanged. A Windows PC that already bound RNDIS keeps working; the risk is
limited to the image content changing on the next rebuild.
