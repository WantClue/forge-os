## Flashing Factory Image
You can flash [factory images](https://github.com/wantclue/forge-os/releases) to your BitForge using generic ESP32 flashing tools such as the [esptool-js website](https://espressif.github.io/esptool-js/)

- Power up your BitForge via the barrel connector
- attach your BitForge USB to your computer
- open of the [esptool-js website](https://espressif.github.io/esptool-js/) in Chrome
- Under the `Program` heading set the baudrate to 921600 (this should be the default)
- click Connect
- Chrome will popup a window asking you which serial port you weant to connect to. 
    - on Mac and Linux the correct serial port will have something similar to `usbmodem1434401` in the name
    - on PC it will prolly be one of those COM ports. Good luck.
- Select the proper serial port and then click connect
- Once the ESP Tool has connected to your BitForge over serial, Set the Flash Address to `0x0`
- Under File click "Choose File" and select the [factory image](https://github.com/wantclue/forge-os/releases) you want to flash. it's called something like `bitforgeos-factory-400-v2.1.5.bin` Be sure to use the image with the same version number as your BitForge (400 in this case)
- Down below click the "Program" button
- Wait paitiently.
- Keep waiting until the console on ESP Tool says "Leaving..."
- If you are _sure_ you have seen ESP Tool say "Leaving..." then you're BitForge might restart.
- If you BitForge hasn't restarted, you can press the RESET button on the BitForge
- The self test should run. this can take a second. You'll need to press the RESET button on the BitForge once the self test has passed.
