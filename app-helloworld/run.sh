cd ~/OS-2026/app-helloworld
qemu-system-aarch64 -kernel workdir/build/helloworld_qemu-arm64 -nographic -machine virt -cpu cortex-a57
