# System Monitor

A live view of what your PC is doing, drawn with DirectX.

No frameworks and no dependencies — just C++20, Direct3D 11, Direct2D and the
Win32 API. The result is a single 506 KB executable that runs anywhere on
Windows 10 or 11 with nothing to install beside it.

![System Monitor](screenshot.png)
![System Monitor](screenshot2.png)

---

## What it shows

- **Processor** — total load, every core individually, and the current clock
- **Memory** — in use, total, and commit charge
- **Graphics** — load, video memory, and the GPU's **temperature and fan speed**
- **Storage** — free space and live read/write rates
- **Network** — up and down, over whichever adapter is busiest
- **Processes** — every one of them, with its icon, CPU and memory

---

## Using it

| | |
|---|---|
| **Click a tile** | points the big chart at it — processor, memory, graphics or disk |
| **Type anything** | filters the process list; Escape clears it |
| **Hold Ctrl** | freezes the ordering so a row stops moving while you aim at it |
| **Right-click a row** | End task, copy name, copy process id |
| **Click a column** | sort by name, CPU or memory, ascending or descending |
| **Scroll or drag** | the process list scrolls |

A tray icon sits in the notification area for as long as it runs, alongside the
ordinary taskbar button rather than instead of it. Clicking it brings the window
forward; right-clicking it offers Exit. Minimising is left completely alone, so
the window keeps its taskbar button and its restore animation, the way Task
Manager does.

---

## Why it might be worth a look

**It reads the machine properly.** The obvious way to get per-process CPU is to
open each process and ask — but Windows refuses that for about half of them, so
`dwm.exe`, `System` and Windows Defender all sit at `0.0%` forever. And the
kernel's own CPU times are counted in whole 15.6 ms ticks, so anything quieter
than a fifth of a percent rounds to zero too. This asks the kernel for the whole
process table in one call and counts CPU **cycles** instead. Nothing reads zero
unless it really is zero.

**Temperature without a driver.** The GPU's temperature comes from the display
driver interface, the same place Task Manager gets it. No kernel driver, no
vendor SDK, nothing to install. (CPU temperature genuinely does need a signed
kernel driver, so it is not here — see [TECHNICAL.md](TECHNICAL.md).)

**It is a real window.** The frame is drawn by hand, but it is an ordinary Win32
window underneath, so Aero Snap, Snap Layouts, per-monitor DPI, Alt+Tab and the
minimise animation all work exactly as they should.

**It costs almost nothing.** About **1.2% of an 8-core machine** at a real 60 fps
with everything animating, and **0.06%** when minimised.

---

## Building

Open `SystemMonitor.slnx` in Visual Studio and press F5, or from a terminal:

```bash
build.bat Release
```

Needs the v145 toolset and the Windows 10/11 SDK. Release links the CRT
statically, so the output is one self-contained `.exe`.

---

## More detail

[TECHNICAL.md](TECHNICAL.md) covers how each part works and the measurements
behind the numbers above — including a few things that turned out to be wrong on
the first attempt and why.

---

Crafted by **bxzx**.
# SystemMonitor 
