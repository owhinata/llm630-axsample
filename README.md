# LLM630 Compute Kit, AX API Sample

## Clone and Build

1. Prepare a workspace on the host:
   ```bash
   mkdir -p "/home/$USER/work"
   cd "/home/$USER/work"
   ```
2. Clone the repository:
   ```bash
   git clone https://github.com/owhinata/llm630-axsample.git
   cd llm630-axsample
   ```
3. Configure and build:
   ```bash
   cmake -B build
   cmake --build build
   ```

## Host Setup and Deployment

1. Install required host tools:
   ```bash
   sudo apt-get update
   sudo apt-get install -y android-tools-adb
   ```
2. Verify that the device is visible over ADB:
   ```bash
   adb devices
   ```
   A successful connection shows:
   ```
   axera-ax620e        device
   ```
3. *(Optional)* If you see `no permissions`, add the following udev rule and reload:
   ```bash
   echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="32c9", MODE="0666", GROUP="plugdev"' \
     | sudo tee /etc/udev/rules.d/51-android.rules > /dev/null
   sudo chmod a+r /etc/udev/rules.d/51-android.rules
   sudo udevadm control --reload-rules
   sudo udevadm trigger
   ```
4. Prepare the device for file synchronisation (first-time prerequisites are marked accordingly):
   ```bash
   adb shell "apt update && apt install -y rsync"   # run once
   adb forward tcp:2222 tcp:22                      # run once
   adb shell "mkdir -p $(pwd)"
   ```
5. Set up SSH key authentication and SSH config (run once):
   ```bash
   # Generate SSH key if not already present
   [ ! -f ~/.ssh/id_ed25519 ] && ssh-keygen -t ed25519 -f ~/.ssh/id_ed25519 -N ""

   # Copy public key to device (default password: root)
   ssh-copy-id -p 2222 root@localhost

   # Add SSH config entry for the device
   cat >> ~/.ssh/config << 'EOF'
Host ax620e-device
    HostName localhost
    Port 2222
    User root
    IdentityFile ~/.ssh/id_ed25519
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null
EOF

   # Verify passwordless authentication works
   ssh ax620e-device "echo 'SSH key auth working'"
   ```
6. Build and deploy to the device:
   ```bash
   cmake --install build
   ```
7. Run the sample on the device:
   ```bash
   ssh ax620e-device "cd $(pwd)/build/cpp/hello_world && ./hello_world"
   ```
