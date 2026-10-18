# synthetic-circuit (track generation, AC-M3-04)

CC0-1.0. Written by `tools/gen_track_fixtures.py` (`python -I tools/gen_track_fixtures.py --root .`; ctest
`fixtures.track_generator_current` checks that the committed files are what it writes). Nothing here comes from
the code under test (D-041): the script integrates the circuit from a closed-form curvature profile and simulates
the lap itself.

The circuit is a made-up, counter-clockwise loop of about 2975 m: seven corners (a 160° hairpin of 18 m radius
down to a 42° bend of 120 m radius, each with 10-15 m curvature ramps), a 12° kink of 400 m radius that is not a
corner, and two straights lengthened by the script so the loop closes exactly. The lap follows a velocity profile
limited by 15 m/s² of lateral grip, 14 m/s² of braking and traction falling from 6 m/s² to nothing at 75 m/s.

| File | Contents |
|---|---|
| `circuit.json` | The ENU origin, the true length and direction, the lap window, the corners and the kink (start, end, apex = heading midpoint, angle, radius), the noise of every signal, the GPS dropout |
| `expected.centre.csv` | The true centreline, `dist,x,y` in ENU metres every 1 m; the last row closes the loop |
| `expected.inner.csv`, `expected.outer.csv` | The centreline offset 6 m to the left (inside) and to the right, against the centreline distance |
| `lap.gps.csv` | `t,lat,lon` at 10 Hz (WGS-84 degrees): the true position plus white noise of σ = 1.5 m on each ENU axis, and a 2 s dropout (empty cells) through turn 6 |
| `lap.imu.csv` | `t,speed,latAccel` at 50 Hz: speed in m/s reading 0.8 % high with 0.05 m/s of noise; lateral acceleration in m/s², positive to the left, with 0.25 m/s² of noise and a 0.08 m/s² bias |
| `inner.gps.csv`, `outer.gps.csv` | GPS of a lap driven on the inner and on the outer line (same noise, no dropout) |

Times are integer microseconds from 0; every signal runs 1 s before and after the lap window `[1 s, 1 s + lap
time)`. The tests (`libs/analysis/tests/track_generator_tests.cpp`, `track_fixture.hpp`) measure the generated
maps against the true lines (GPS: ≤ 2 m RMS) and the Speed+LatG map against the GPS map after a best-fit rigid
alignment (≤ 15 m RMS, every default-section apex within 20 m).
