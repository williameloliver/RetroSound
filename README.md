# RetroSound v0.3.4 DEBUG

Based directly on the validated v0.3.3 Windows build.

Changes in v0.3.4:
- Keeps the stable v0.3.3 startup path.
- Adds a native Windows `SysIPAddress32` control instead of the crashing plain EDIT control.
- Default IP is 192.168.1.35, but all four octets are editable.
- CONNECT reads the current IP from the native control.
- Keeps quality, buffer and Atom Mode controls.
- Keeps console checkpoints for Windows 8.1 debugging.
- Android source is unchanged.

Expected startup checkpoint: `[GUI 7/7] READY`.
