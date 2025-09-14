# OS-2026

## 环境准备与编译

1. 安装依赖：
  ```bash
  sudo apt install -y --no-install-recommends \
    build-essential \
    sudo \
    gcc-aarch64-linux-gnu \
    libncurses-dev \
    libyaml-dev \
    flex \
    bison \
    git \
    wget \
    uuid-runtime \
    qemu-kvm \
    qemu-system-x86 \
    qemu-system-arm \
    sgabios \
    gdb-multiarch
  ```

2. 克隆项目并进入目录：
  ```bash
  git clone https://github.com/zju-aces-aios/OS-2026.git
  cd app-helloworld
  ```

3. 初始化和编译：
  ```bash
  UK_DEFCONFIG=$(pwd)/defconfigs/qemu-arm64 make defconfig
  make -j8
  ```

4. 启动 QEMU 运行 helloworld：
  ```bash
  qemu-system-aarch64 -kernel workdir/build/helloworld_qemu-arm64 -nographic -machine virt -cpu cortex-a57
  ```

5. 常用操作与命令：
   - **退出 QEMU**：按 `Ctrl+A`，然后按 `X`

## 使用 gdb 和 QEMU 进行调试

1. 启动 QEMU 并开启 gdb 远程调试端口：

  ```bash
  qemu-system-aarch64 -kernel workdir/build/helloworld_qemu-arm64 -nographic -machine virt -cpu cortex-a57 -s -S
  ```

  参数说明：
  - `-s`：等价于 `-gdb tcp::1234`，在 1234 端口开启 gdb 远程调试。
  - `-S`：QEMU 启动后暂停，等待 gdb 连接。

2. 另开一个终端，使用 gdb-multiarch 连接 QEMU：

  ```bash
  gdb-multiarch workdir/build/helloworld_qemu-arm64.dbg --eval-command="target remote :1234"
  ```

3. 设置断点并开始调试，例如：

  ```gdb
  (gdb) b main
  (gdb) c
  ```

4. 查看汇编代码，推荐使用 TUI 模式：

  ```gdb
  (gdb) layout asm
  ```
  - 该命令会在 gdb 中打开汇编窗口，方便单步跟踪。
  - 可用 `Ctrl+X A` 切换回普通模式。