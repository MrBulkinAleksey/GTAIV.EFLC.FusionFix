# Vehicle mirror finder

Research scripts for real reflections in the player car's mirrors: they read `vehicles.img` (IMG3, AES key from the
exe), unpack each `.wft` and find the mirror glass of every car as a flat group of triangles facing backwards:

- side mirrors: on the front door bone (or the chassis/bodyshell), outside the side windows, not below the window
  line, near the windscreen in length;
- the interior mirror: within 0.25 m of the centre line, near the top of the windscreen.

    GTAIV_EXE=bin/GTAIV.exe VEHICLES_IMG=log/vehicles.img python3 tools/vehicle_mirrors/mirrors.py [name.wft ...]

Needs pycryptodome. Output per car: area, plane normal and the bounding box of the glass in the car's space.
Vanilla vehicles.img (2026-10-16): side mirrors found on 94 of 103 land vehicles, the interior mirror on 92.
