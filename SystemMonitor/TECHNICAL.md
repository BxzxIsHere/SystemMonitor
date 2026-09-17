# System Monitor

A live view of the machine, drawn with DirectX into a window that keeps all of
its native behaviour and none of its native chrome.

No frameworks, no dependencies: C++20, Direct3D 11, Direct2D, DirectWrite and
the Win32 API.

---

## Layout

```
SystemMonitor/
├─ Libs/     reusable window + rendering library  (namespace slick)
├─ Logic/    the probes that read the machine     (namespace sysmon)
├─ Gui/      the dashboard those two meet in      (namespace sysmon::ui)
├─ Assets/   master art the icon is generated from
└─ Main.cpp  wires the three together
```

The three folders only ever depend downwards: `Gui` uses `Libs` and `Logic`,
`Logic` knows nothing about drawing, and `Libs` knows nothing about system
monitoring. Dropping `Libs` into another project takes the whole custom window
with it.

---

## Libs — the window

`slick::Window` is a real top-level Win32 window that has had its non-client
area removed and redrawn with Direct2D. It keeps a normal `WS_OVERLAPPEDWINDOW`
style and lets `DefWindowProc` run the interactions, so everything the shell
expects still works:

| | |
|---|---|
| Aero Snap, drag to maximise, window shake | `HTCAPTION` from `WM_NCHITTEST` |
| Snap Layouts flyout on the maximise button | real `HTMAXBUTTON` hit codes |
| Resize from all eight edges and corners | frame grab band reinstated per-DPI |
| Double-click caption, Alt+Space, right-click menu | `DefWindowProc` |
| Rounded corners, drop shadow, dark border | DWM window attributes |
| Taskbar, Alt+Tab, minimise and restore animations | ordinary overlapped window |
| Per-monitor DPI | `WM_DPICHANGED` + a per-monitor-v2 manifest |
| Activation | chrome dims when the window loses focus |
| Notification area | permanent tray icon beside the taskbar button, with a menu |
| Hand cursor over clickable content | `WM_SETCURSOR` asks the delegate |

Three details that are easy to get wrong and are handled here: a maximised
window is inflated by the frame thickness, so `WM_NCCALCSIZE` insets the client
area to stop content falling off the monitor edges; a maximised window leaves
one pixel of non-client space on whichever edge hosts an auto-hiding taskbar, so
the taskbar can still be summoned; and the tray icon is re-added when Explorer
broadcasts `TaskbarCreated`, so restarting the shell cannot strand the window
hidden with no way back to it.

The tray icon is there for the whole session, next to the ordinary taskbar
button rather than instead of it, the way a resident monitor behaves. Minimising
is left completely alone, so the window keeps its taskbar button and its restore
animation; clicking the tray icon just surfaces it again. The render loop idles
whenever the window is not on screen: **0.06% CPU** minimised, against roughly
**1.4%** visible at a real 60 fps.

Recording a frame costs **0.57 ms**, measured per panel, so the frame rate is a
straight trade: 30 fps costs about half as much and looks very nearly as good,
because the readings arrive four times a second and everything on screen is
eased towards them over elapsed time rather than over frames. It is one line in
`Main.cpp` either way.

Worth being clear about what that CPU figure is and is not. The UI is drawn by
the **GPU** — Direct2D on a hardware Direct3D 11 device, with WARP only as a
fallback — and the process shows up against the GPU's 3D engine while it runs.
The CPU time above is the cost of *recording* the draw calls, which is the part
that cannot move to the GPU.

**Renderer** owns the D3D11 device, a flip-model swap chain and the Direct2D
device context, and rebuilds itself from scratch on device loss. Direct2D is
given the real window DPI, so every coordinate in the UI is a DIP and nothing
multiplies by a scale factor.

**Canvas** is a small drawing surface over that context — rectangles, rounded
rectangles, gradients, arcs, paths and text — caching brushes, gradient stop
collections and text formats so a frame only allocates when something genuinely
new appears on screen.

The render loop is paced rather than left to run at the display refresh rate,
and idles in `MsgWaitForMultipleObjectsEx` between frames so input never waits
on the clock.

Two things take the pump away from that loop, and both would otherwise freeze
the window solid for as long as they lasted: the modal move/size loop while the
window is being dragged, and `TrackPopupMenu` while a context menu is open.
Neither returns until it is done. A timer on the owning window fires through
both of them, so the dashboard carries on drawing underneath a menu instead of
appearing to hang.

### Why the frame rate was never the frame rate

Asking `MsgWaitForMultipleObjectsEx` to wake you in sixteen milliseconds does
not wake you in sixteen milliseconds. A timeout in milliseconds is rounded up to
the system timer granularity, which is **15.6 ms** by default, so a sixty frame
loop was being woken every 31 ms and running at **33 fps**. Setting the target
to thirty made it 21. Everything on screen juddered, and the process list — the
one thing being dragged around under a pointer — showed it worst.

The wait is a **high resolution waitable timer** now, which is exact and which,
unlike `timeBeginPeriod`, costs nothing to every other process on the machine.

| | before | after |
|---|---|---|
| frame rate | 35.5 fps | **58.9 fps** |
| median gap | 30.50 ms | **17.01 ms** |
| worst gap | 31.78 ms | **17.36 ms** |
| gaps over 25 ms | 88 of 107 | **0** |

---

## Logic — the probes

Each probe is one file, runs on a single background thread, and publishes a
complete, consistent `Readings` snapshot the UI copies once a frame. The UI
never blocks on a probe.

| Probe | Source |
|---|---|
| `CpuProbe` | `NtQuerySystemInformation` processor performance counters, per core, plus the current clock from the power management interface |
| `MemoryProbe` | `GlobalMemoryStatusEx` and `GetPerformanceInfo` |
| `StorageProbe` | `PhysicalDisk` performance counters, and `GetDiskFreeSpaceEx` for capacity |
| `NetworkProbe` | `GetIfTable2`, summed over every live non-loopback adapter |
| `GpuProbe` | DXGI for the adapter and its video memory, `GPU Engine` counters for utilisation, `D3DKMTQueryAdapterInfo` for temperature and fan |
| `ProcessProbe` | `NtQuerySystemInformation`, one call for the whole process table |

Fast probes run at 4 Hz, process enumeration at 1 Hz and volume capacity every
10 seconds. Performance counters are added by their **English** path, because
counter display names are localised and a hard-coded localised path silently
returns zero forever on a non-English install.

### Why the process table is read in one call

The obvious way to do this is a toolhelp snapshot and then `OpenProcess` plus
`GetProcessTimes` on each process in turn. It is also wrong, and quietly so.

`OpenProcess` is **refused for about half of them** — 127 of 254 on this
machine. A process that cannot be opened has no CPU figure, so it sits at
`0.0%` forever. That is not a rounding error at the bottom of the list: the
processes being hidden this way were `dwm.exe` at 6.2%, `System` at 2.9%,
`svchost.exe`, and Defender's `MsMpEng.exe` — four of the eight busiest things
on the machine, all reported as idle.

Asking the kernel for the whole table instead returns every process with its
kernel and user time, working set and thread count, with no handle to open and
nothing to be refused. It recovered a CPU figure for **120 of the 127** that
`OpenProcess` would not touch, and it is faster as well: **2.2 ms** against
4.1 ms. The seven still without one genuinely have no threads, such as the
`Secure System` container.

The old path is kept as a fallback should the call ever fail, since the layout
of that structure is not contractual.

### And why it counts cycles rather than time

Reading the whole table fixed the processes that could not be seen. It did not
fix the ones that could be seen but still read `0.0%`, and that turned out to be
a different bug wearing the same clothes.

Kernel and user time are accumulated in whole **15.625 ms clock ticks**. On
eight cores over a one second sample that is 0.195% at a time, so every process
quieter than a fifth of a percent lands on either zero or 0.195 and nothing in
between. Defender at 0.50%, Discord at 0.17%, a busy `svchost` at 0.07% — all
reported as doing nothing at all.

`CycleTime` in the same structure counts cycles actually retired, with no such
floor. Each process gets its share of the total cycles burned in the interval,
the idle process included in that total but kept out of the table: what it
burned is precisely the part of the machine nothing else was using.

| | kernel + user time | cycle time |
|---|---|---|
| `MsMpEng.exe` | 0.00% | **0.50%** |
| `CurseForge.exe` | 0.00% | **0.22%** |
| `Discord.exe` | 0.00% | **0.17%** |
| `svchost.exe` | 0.00% | **0.07%** |
| `claude.exe` | 1.37% | **3.73%** |

Intervals are measured with the performance counter rather than `GetTickCount64`,
whose 15.6 ms granularity is 1.5% of a one second sample and showed up as every
figure in the table quivering.

### Temperature without a driver

The graphics tile carries the GPU temperature, and the fan speed whenever the
fan is actually turning. Both come from `D3DKMTQueryAdapterInfo` — the display
miniport interface in `gdi32`, which is where the task manager gets the same
number. Plain user mode, no vendor SDK, no kernel driver, nothing to install.
The adapter is opened by the LUID DXGI already reported, so it is certainly the
same card the rest of the tile is describing.

**CPU temperature is deliberately absent.** It lives in a model specific
register, and reading one of those requires ring 0 — which means shipping a
signed kernel driver, the approach every hardware monitor takes and the reason
they all trip antivirus. On this machine the driver-free alternatives come to
nothing: the ACPI thermal zone answers `Not supported`, and `Win32_TemperatureProbe`
is empty, as it is nearly everywhere. Showing no figure is better than showing a
motherboard sensor labelled as the CPU.

---

## Gui — the dashboard

Four ring-gauge tiles, a CPU history chart, per-core columns, a network chart
and a sortable process table.

**Scaling.** Every measurement in the layout goes through one function that
multiplies it by a factor derived from the window size, clamped between 1.0 and
1.5. Type, padding, gauges, columns and row heights all grow together, so a
maximised window is a bigger dashboard rather than the same small one surrounded
by empty panels. Below the design size the factor stays at 1.0, because
shrinking text costs more legibility than the space is worth.

**Motion.** Nothing on screen snaps. Every value the eye can follow is eased
exponentially towards its target -- loads, per-core columns, transfer rates,
clock, process and thread counts, and each process row individually, keyed by
process id so a row never inherits the value of whatever used to sit in that
slot. The step comes from elapsed time rather than a frame count, so a dropped
frame does not show up as a stutter. On top of that the charts interpolate
between samples: a phase value runs 0 to 1 across each sample interval and
slides the series left by that fraction of a step, so the graphs glide
continuously at 60 fps instead of stepping four times a second.

**Sorting.** The process table orders by name, CPU or memory, ascending or
descending; click a column to choose it, click again to flip it. The probe
publishes every running process unsorted rather than a top few by one fixed
metric, because a list already truncated by CPU cannot be re-sorted by memory.
When the table is sorted by CPU, the visible rows are ordered by the **eased**
figure rather than the raw one — by the same number the row is showing. Ordering
by the raw value was the original choice, on the reasoning that a row should not
drift up or down mid-animation, and it was wrong: it let a row reading 9.3% sit
below a row reading 8.7% for as long as the two were still catching up, which
reads as a sorting bug rather than as animation. Rows now glide past each other
as the values genuinely cross. The meter beside each row follows whichever column
is sorted, so it always says something about the order you are looking at.

**The table scrolls**, by wheel or by dragging its scrollbar, and only the rows
that could be on screen are animated — there is no point easing the four
hundredth process. That last point bit once: a process is seeded at its real
value the first time it is seen rather than eased up from zero, because a
newly started process that is already busy never ramped up, and while the rows
are ordered by the eased value, easing from zero sorted a busy process straight
out of the window that was keeping it animated. Three processes each burning a
full core disappeared from the table entirely.

**The layout answers the window's shape, not just its size.** Everything scales
from a design size of 1240 by 756, but scaling alone handles a window that is
bigger, not one that is a different shape, and the two extremes went wrong in
opposite directions.

On a **tall** window the middle row grew past anything its contents could use: a
75 second trace stretched over half a metre says nothing more than it did, and a
core meter taller than nine times its own width stops reading as a meter, so the
group capped itself and the panel filled with dead space while the process list
showed ten rows. The middle row now takes what it can use and no more, and
everything left over goes to the list, which has no such ceiling — more height
is simply more rows. The same window now shows twenty five.

On a **very wide** window the eight core meters sat marooned in the middle of a
card three times wider than they needed. Widening the columns to fill it would
have turned them into slabs, so the spacing gives way first: leftover width goes
into the gaps, up to eight tenths of a column, and the group spreads across the
card instead of huddling in it.

The floor on the bottom row accounts for what the process card spends before its
first row — a title, a filter box and column headers — because without allowing
for those a short window left the list showing three.

**Typing filters the list.** There is nothing else on the window that wants the
keyboard, so anything typed goes to the filter box above the column headers, and
Escape clears it, and the header counts what is being shown against what is
running. The box takes the editing shortcuts a text box is expected to take:
Ctrl+A, C, X, V, and Ctrl+Z and Ctrl+Y for undo and redo, with Ctrl+Shift+Z as
the other spelling of redo. A run of typed characters collapses into one undo
step, because undoing a word at a time is what every other text box on the
machine does and undoing a letter at a time is infuriating.

**Holding Ctrl freezes the ordering**, the way the task manager does, so a row
stops sliding out from under a pointer that is aiming at it. Readings still
arrive and every number still moves; only the order that decides which row is
where is held still.

**Right-clicking a row** offers End task, copy name and copy process id. End
task goes straight to `TerminateProcess` with no request to close first, which
is what the task manager's own End task does. Two things stop it: a process the
account cannot open, and a process the kernel marks critical — terminating one
of those bugchecks the machine, so it is refused and says so. The handle is
opened with query rights as well as terminate rights, because `IsProcessCritical`
needs the former and asking for terminate alone leaves that check silently
failing every time.

**Clicking a tile** points the chart panel at it. The four headline readouts
double as the chart's tab strip, so the same panel shows processor, memory,
graphics or disk history, with peak and average following the selected series
and a detail line underneath naming what the percentage is of.

**Rows carry the application's own icon**, from the shell. Two caches, because
the two costs differ: a process id resolves to a path once, and a path resolves
to an icon once, so ten Chrome processes share one icon rather than building ten
identical ones. Both halves are budgeted per frame — two of each — because
asking the shell about a path it has not seen can reach the disk, and doing six
of those in a frame is enough to turn a scroll into a slideshow.

### Depth

Cards answer the pointer in two separate ways, and keeping them separate is the
whole point. **Press** is movement: the card shrinks slightly and a recess opens
behind it, which the eye reads as a surface pushed in. Every card does it, and
the four tiles — the ones that can actually be clicked — move about three times
as far as the panels. **Glow** is accent light falling from above, drawn as a
gradient across the whole face rather than a rule along its top edge, because a
rule reads as a drawn line and a gradient reads as illumination. Only the tile
the chart is following gets any.

Lighting everything the pointer touches was the first attempt and it was wrong:
the whole page looked selected at once, and the thing that was actually selected
stopped standing out at all.

The palette is a warm near-black in the spirit of the Claude Code terminal —
clay-tinted surfaces, cream text, one clay accent doing the talking, and
desaturated companions for the secondary series.

---

## Building

Open `SystemMonitor.slnx` in Visual Studio and press F5, or from a terminal:

```
build.bat Release
```

Output lands in `SystemMonitor\x64\Release\SystemMonitor.exe`.

Requires the v145 toolset and the Windows 10/11 SDK. Targets Windows 10 1709 or
newer; the Windows 11 specific chrome (rounded corners, custom border colour,
dark context menus) falls back cleanly on Windows 10.

### Self contained

Release builds link the CRT statically, so the result is one 437 KB executable
with nothing to install beside it: no Visual C++ redistributable, no side by side
assemblies, no loose DLLs. Its entire import table is Windows itself.

```
d3d11  dxgi  d2d1  DWrite  dwmapi  pdh  IPHLPAPI  SHELL32  USER32  GDI32  ADVAPI32  KERNEL32
```

The icon is embedded as a resource carrying frames from 16 to 256 px, so the
taskbar, Alt+Tab, Explorer and the tray each get a size that was rendered for
them rather than rescaled from one that was not. `Assets/App.png` is the master
art those frames are generated from.

---

Crafted by **bxzx**.
