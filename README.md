# CPU Meter

A small speedometer-style gauge showing the average CPU load of the machine
(0–100%), with a button that reveals the load of every core.

Written in C, GTK 3 + Cairo. Reads `/proc/stat` once per second.

## Build

```sh
make            # needs: gcc + libgtk-3-dev (sudo apt install libgtk-3-dev)
```

## Run

```sh
./cpu_meter
```

- Click **Show all cores** to toggle the per-core panel.
- The needle eases smoothly to the current value; the arc/colour goes
  green → yellow → red as load rises.
- The per-core panel auto-fits the screen: if the window would be taller
  than your display, the gauge shrinks (down to a minimum) so that every
  core row stays fully visible.

## Options

| option          | meaning                                                    |
|-----------------|------------------------------------------------------------|
| `--show-cores`  | start with the per-core panel open                          |
| `--fake-load N` | show a fixed value `N` (0–100) instead of the real load     |
| `--screenshot F`| save a PNG of the window once it has rendered, then exit    |

## How it works

Each second the app samples `/proc/stat` and computes per-core busy % from
the deltas of the jiffies counters (`busy = total − idle − iowait`). The
overall gauge value is the average across all cores.

![Example1](https://github.com/osburn/cpu_meter/blob/master/example_1.jpg "Example1")
![Example2](https://github.com/osburn/cpu_meter/blob/master/example_2.jpg "Example2")
