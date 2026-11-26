

Prototype of a 

# Software Frequency-Band Warping (RTA-SFW)

Real-Time Topographic Soundstage, 
Cross-Modulation Patch Matrix, and C-Operator Architecture


Practical Examples includes a Finalizer and a Test Terrain Flight Simulation.



### Bands

- The Bedrock (20 Hz−90 Hz)
- The Foothills and Ridges (90 Hz−2.5 kHz)
- The Atmosphere (2.5 kHz−20 kHz)


### Perspective Transformation: Frog vs. Flight View

- Mid/Side Spatial Expansion
- Atmospheric Spectral Tilt: High-frequency air absorption and low-mid ground-reflection
roll-off are coupled to θ, softening harsh close-range transients into a wide panoramic relief
without using fatiguing harmonic exciters or feedback distortion loops.




```
cmake -B build -DCMAKE_C_STANDARD=23
cmake --build build -j
```


# Live mic/line-in with matrix & visualization; press R to record, V for viz-only
```
./build/rta_sdl3_live --out out_live.wav
```

# Feed a WAV file, run Techno Pumper preset at 0.65 flight, save finalized WAV
```
./build/rta_sdl3_live --in assets/input.wav --out finalized_techno.wav --preset 1 --flight 0.65
```

## SDL Finalizer

```
cmake --build build -j

# Studio 54 baseline with gentle flight tilt
./build/rta_sdl3_finalizer --in assets/input.aif --out out_studio54.wav --preset studio54 --flight 0.25

# Techno Pumper with heavier kick-to-body ducking and wider air
./build/rta_sdl3_finalizer --in assets/input.wav --out out_techno.wav --preset techno --flight 0.50 --kick-body -0.60 --air-width 1.40

```


