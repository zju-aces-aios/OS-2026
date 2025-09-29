# Lab4: Hexagon NPU GEMM

## 目录

- [环境准备](#环境准备)
  - [Hexagon SDK 安装](#hexagon-sdk-安装)
  - [Android NDK 安装](#android-ndk-安装)
- [代码编译与运行](#代码编译与运行)
  - [编译 NPU 代码](#编译-npu-代码)
  - [实体设备运行 (推荐)](#实体设备运行-推荐)
  - [模拟器运行 (备选)](#模拟器运行-备选)

---

## 环境准备https://github.com/zju-aces-aios/OS-2026/blob/main/Lab4/Lab4.md

### Hexagon SDK 安装

如果高通账号没申请下来，可以通过[直链下载](https://apigwx-aws.qualcomm.com/qsc/public/v1/api/download/software/sdks/Hexagon_SDK/Linux/Debian/6.3.0.0/Hexagon_SDK.zip) Hexagon SDK，并将压缩包解压到 `/workspace/Qualcomm/Hexagon_SDK/6.3.0.0`。

如果高通账号申请通过，推荐通过下面的QPM工具安装SDK。

#### 1. 下载并安装 QPM

1. **下载 QPM**
   
   访问 [QPM 官方下载页面](https://qpm.qualcomm.com/#/main/tools/details/QPM3) 下载安装包。

2. **安装系统依赖**
   
   ```bash
   sudo apt update && sudo apt install -y bc xdg-utils
   ```

3. **安装 QPM**
   
   ```bash
   sudo dpkg -i <your-qpm-package>.deb
   ```
   
   > 注：请将 `<your-qpm-package>.deb` 替换为实际下载的文件名

#### 2. QPM 登录与配置

1. **登录 QPM**
   
   ```bash
   qpm-cli --login <username>/<email>
   ```

2. **激活许可证并安装 Hexagon SDK**
   
   ```bash
   qpm-cli --license-activate HexagonSDK6.x
   sudo qpm-cli --install HexagonSDK6.x
   ```

3. **配置环境变量**
   
   在 `~/.bashrc` 中添加：
   
   ```bash
   export HEXAGON_SDK_PATH=/local/mnt/workspace/Qualcomm/Hexagon_SDK/6.3.0.0
   ```

4. **修改文件权限 (仅限 Codespaces 用户)**
   
   ```bash
   sudo chown -R codespace:codespace /local/mnt/workspace/Qualcomm/Hexagon_SDK/6.3.0.0/utils
   ```

### Android NDK 安装

#### 1. 下载并配置 Android SDK Command Line Tools

1. **下载 Command Line Tools**
   
   ```bash
   wget https://googledownloads.cn/android/repository/commandlinetools-linux-13114758_latest.zip
   ```

2. **解压并组织目录结构**
   
   ```bash
   unzip commandlinetools-linux-13114758_latest.zip
   mkdir -p ~/Android/Sdk/cmdline-tools/latest
   mv cmdline-tools/* ~/Android/Sdk/cmdline-tools/latest/
   ```

3. **配置环境变量**
   
   在 `~/.bashrc` 中添加：
   
   ```bash
   export PATH=~/Android/Sdk/cmdline-tools/latest/bin/:$PATH
   ```
   
   使配置生效：
   
   ```bash
   source ~/.bashrc
   ```

#### 2. 安装 NDK

1. **查看可用版本并安装**
   
   ```bash
   sdkmanager --list
   sdkmanager "ndk;29.0.13113456"
   ```

2. **配置 NDK 环境变量**
   
   在 `~/.bashrc` 中添加：
   
   ```bash
   export ANDROID_NDK_ROOT=~/Android/Sdk/ndk/29.0.13113456/
   ```

3. **安装 ADB 工具**
   
   ```bash
   sudo apt install android-tools-adb
   ```

---

## 代码编译与运行

### 编译 NPU 代码

首先设置 Hexagon SDK 环境并编译 DSP 代码：

```bash
source $HEXAGON_SDK_PATH/setup_sdk_env.source
cd dsp
make hexagon BUILD=Debug DSP_ARCH=v79
```

### 实体设备运行 (推荐)

#### 1. 申请 QDC 设备

1. 在 [高通 QDC 平台](https://qdc.qualcomm.com) 申请一台骁龙 8 Elite 手机
2. 选择 SSH 连接方式
3. 创建私钥并保存到 `~/qdc.pem`，修改权限：
   
   ```bash
   chmod 600 ~/qdc.pem
   ```

#### 2. 建立设备连接

1. **创建 SSH 隧道**
   
   点击 QDC 页面右上角的【Connect】按钮，复制连接命令，例如：
   
   ```bash
   ssh -i ~/qdc.pem -L 5037:sa324277.sa.svc.cluster.local:5037 -N sshtunnel@ssh.qdc.qualcomm.com
   ```

2. **验证设备连接**
   
   ```bash
   adb devices
   ```
   
   应该能看到已连接的设备。

#### 3. 编译 Android 测试工具

```bash
mkdir build
cd build
cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
  -DHEXAGON_SDK_ROOT=$HEXAGON_SDK_ROOT ..
make
```

#### 4. 部署并运行测试

1. **推送文件到设备**
   
   ```bash
   adb push npu_gemm_test /data/local/tmp/
   adb push ../dsp/hexagon_Debug_toolv88_v79/ship/libcalculator_skel.so /data/local/tmp/
   ```

2. **在设备上执行测试**
   
   ```bash
   adb shell
   cd /data/local/tmp
   chmod +x npu_gemm_test
   ./npu_gemm_test 64 64 64 --cpu-check
   ```

### 模拟器运行 (备选)

> ⚠️ **注意**：模拟器运行性能较差，且不方便进行功能验证。

#### 1. 修改必要的依赖库

```bash
sudo ln -s /usr/lib/x86_64-linux-gnu/libncurses.so.6 \
           /usr/lib/x86_64-linux-gnu/libncurses.so.5
```

#### 2. 运行 Hexagon 模拟器

```bash
cd dsp
export DSP_BUILD_DIR=dsp/hexagon_Debug_toolv88_v79

${HEXAGON_SDK_ROOT}/tools/HEXAGON_Tools/8.8.06/Tools/bin/hexagon-sim \
    -mv79 \
    --simulated_returnval \
    --usefs ${DSP_BUILD_DIR} \
    --pmu_statsfile ${DSP_BUILD_DIR}/pmu_stats.txt \
    --cosim_file ${DSP_BUILD_DIR}/q6ss.cfg \
    --l2tcm_base 0xd800 \
    --rtos ${DSP_BUILD_DIR}/osam.cfg \
    ${HEXAGON_SDK_ROOT}/rtos/qurt/computev79/sdksim_bin/runelf.pbn \
    -- \
    ${HEXAGON_SDK_ROOT}/libs/run_main_on_hexagon/ship/hexagon_toolv88_v79/run_main_on_hexagon_sim \
    stack_size=0x400000 \
    -- \
    libtest_calculator_sim.so 64 64 64
```

---

## 常见问题

- **相对地址**：教程中的shell命令有许多相对地址，请确保当前终端所处目录是否正确
- **权限问题**：确保 Codespaces 用户对 Hexagon SDK 目录有适当的读写权限
- **环境变量**：每次新开终端时需要重新 source Hexagon 环境变量
- **设备连接**：如果 ADB 无法识别设备，检查 SSH 隧道是否正常建立
- **编译错误**：确保所有必要的环境变量都已正确设置
- **libinfo.so.5报错**：如果遇到缺少libinfo5的报错，执行`sudo apt install libtinfo5`