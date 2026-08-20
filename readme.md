[![](https://dcbadge.vercel.app/api/server/3E8ca2dkcC)](https://discord.gg/3E8ca2dkcC)

<!-- ![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/skot/esp-miner/total)
![GitHub commit activity](https://img.shields.io/github/commit-activity/t/skot/esp-miner)
![GitHub contributors](https://img.shields.io/github/contributors/skot/esp-miner)

![Alt](https://repobeats.axiom.co/api/embed/9830d39ca088153c7db39a7c0e1645c62a0454fd.svg "Repobeats analytics image")

-->
![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/wantclue/forge-os/total)
![Alt](https://repobeats.axiom.co/api/embed/2db689d8aedc1d49b807526f0930aff59f151fad.svg "Repobeats analytics image")

# Forge-OS
Forge-OS is a fork of the origional esp-miner it's open source firmware can be found here [ESP-Miner](https://github.com/bitaxeorg/esp-miner)

If you are looking for premade images to load on your BitForge, check out the [releases](https://github.com/wantclue/forge-os/releases) page. Maybe you want [instructions](https://github.com/wantclue/forge-os/blob/master/flashing.md) for loading factory images.

# Bitaxetool
We also have a command line python tool for flashing BitForge and updating the config called Bitaxetool 

**Bitaxetool Requires Python3.4 or later and pip**

Install bitaxetool from pip. pip is included with Python 3.4 but if you need to install it check <https://pip.pypa.io/en/stable/installation/>

```
pip install --upgrade bitaxetool
```
The bitaxetool includes all necessary library for flashing the binaries to the BitForge Hardware.

- Flash a "factory" image to a BitForge to reset to factory settings. Make sure to choose an image built for your hardware version (401) in this case:

```
bitaxetool --firmware ./bitforgeos-factory-BitForge-v1.0.bin
```
- Flash just the NVS config to a bitaxe:

```
bitaxetool --config ./config-Nano.cvs
```
- Flash both a factory image _and_ a config to your BitForge: note the settings in the config file will overwrite the config already baked into the factory image:

```
bitaxetool --config ./config-Nano.cvs --firmware ./bitforgeos-factory-BitForge-v1.0.bin
```

## ForgeOS API
The forge-os UI is called ForgeOS and provides an API to expose actions and information.

For more details take a look at `main/http_server/http_server.c`.

Things that can be done are:
  
  - Get System Info
  - Get Swarm Info
  - Update Swarm
  - Swarm Options
  - System Restart Action
  - Update System Settings Action
  - System Options
  - Update OTA Firmware
  - Update OTA WWW
  - WebSocket

Some API examples in curl:
  ```bash
  # Get system information
  curl http://YOUR-BITFORGE-IP/api/system/info
  ```
  ```bash
  # Get swarm information
  curl http://YOUR-BITFORGE-IP/api/swarm/info
  ```
  ```bash
  # System restart action
  curl -X POST http://YOUR-BITFORGE-IP/api/system/restart
  ```

## Administration

The firmware hosts a small web server on port 80 for administrative purposes. Once the BitForge device is connected to the local network, the admin web front end may be accessed via a web browser connected to the same network at `http://<IP>`, replacing `IP` with the LAN IP address of the BitForge device, or `http://bitaxe`, provided your network supports mDNS configuration.

## Initial Connection

The firmware hosts the first 15 minutes of uptime an extra Access Point. You can connect after connecting your BitForge to your Wi-Fi again with the BitForge in order to retrieve the IP addres of the device on your local network. With that information you can connect back to your home network and use the IP-address to connect to it's ForgeOS.

### Recovery

In the event that the admin web front end is inaccessible, for example because of an unsuccessful firmware update (`www.bin`), a recovery page can be accessed at `http://<IP>/recovery`.

### Unlock Settings

In order to unlock the Input fields for ASIC Frequency and ASIC Core Voltage you need to append `?oc` to the end of the settings tab URL in your browser. Be aware that without additional cooling overclocking can overheat and/or damage your BitForge.

## Development

### Prerequisites

- Install the ESP-IDF toolchain from https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/
- Install nodejs/npm from https://nodejs.org/en/download
- (Optional) Install the ESP-IDF extension for VSCode from https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension

### Building

At the root of the repository, run:
```
idf.py build && ./merge_bin.sh ./bitforgeos-merged.bin
```

Note: the merge_bin.sh script is a custom script that merges the bootloader, partition table, and the application binary into a single file.

Note: if using VSCode, you may have to configure the settings.json file to match your esp hardware version. For example, if your bitforge has something other than an esp32-s3, you will need to change the version in the `.vscode/settings.json` file.

### Flashing

With the bitforge connected to your computer via USB, run:

```
bitaxetool --config ./config-xxx.cvs --firmware ./bitforgeos-merged.bin
```

where xxx is the config file for your hardware version. You can see the list of available config files in the root of the repository.

Note: if you are developing within a dev container, you will need to run the bitaxetool command from outside the container. Otherwise, you will get an error about the device not being found.

