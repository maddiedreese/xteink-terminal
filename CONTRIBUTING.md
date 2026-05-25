# Contributing

This project is experimental hardware firmware. Please keep changes small and easy to test.

Useful checks before opening a pull request:

```sh
cd native-firmware/x4-terminal/firmware
pio run
```

```sh
python3 -m py_compile mac-bridge/x4_tmux_bridge.py
```

When changing rendering behavior, include the X4 model, firmware build, and a photo or description of the display result.
