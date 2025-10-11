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
4. Create a user on the device that matches your host username (adjust the password as needed):
   ```bash
   adb shell "mkdir -p /home && useradd -m $USER"
   adb shell "echo '$USER:your_password_here' | chpasswd"
   ```
5. Prepare the device for file synchronisation (first-time prerequisites are marked accordingly):
   ```bash
   adb shell "apt update && apt install -y rsync"   # run once
   adb forward tcp:2222 tcp:22                      # run once
   adb shell "mkdir -p /home/$USER/work"
   ```
6. Synchronise the project from the host to the device:
   ```bash
   rsync -avz --delete \
     --exclude '.git/' --exclude 'toolchain/' \
     -e 'ssh -p 2222' \
     "/home/$USER/work/llm630-axsample/" \
     "$USER@localhost:/home/$USER/work/llm630-axsample/"
   ```
7. Run the sample on the device:
   ```bash
   adb shell "cd /home/$USER/work/llm630-axsample/build/cpp/hello_world && ./hello_world"
   ```
