# Voxel Sandbox

A Minecraft-like voxel sandbox made in Unreal Engine 5.8.

**Author:** Jaime Ortiz Rozalen, Tech Designer
[Portfolio](https://jaimeortizrozalen.wixsite.com/jaimeortizportfolio) · [LinkedIn](https://www.linkedin.com/in/jaime-ortiz-rozalen/)

## Features

* Random chunk-based world from Perlin noise, reproducible from a seed
* Caves: cheese caverns, tunnels and worm entrances from the surface
* Rock, grass and snow, each with its own break time, material and spawn rules
* Minecraft-style movement: sprint, crouch that sticks to edges, jump buffering
* Hold to mine, press or hold to place, with crack and placement previews
* Unbreakable bottom layer and a build height limit
* Save and load as seed plus player changes
* Face culling and greedy meshing (toggle in the settings Data Asset)
* Every value tweakable in Data Assets, with no magic numbers

Blueprint-first: gameplay and feedback are Blueprints; C++ is used only for the per-cube loops (terrain fill, cave carving, meshing).

## Running the project

1. Unreal Engine **5.8.4** and **Visual Studio** with the *Game development with C++* workload.
2. Clone the repo and open `VoxelSandbox.uproject`.
3. When Unreal asks to rebuild the missing modules, click **Yes**.
4. Open `Content/Voxel/Maps/L\\\_VoxelWorld` and press Play.

## Controls

|Action|Key|
|-|-|
|Move / look|W A S D / mouse|
|Jump (hold to keep jumping)|Space|
|Sprint|Left Ctrl|
|Crouch (won't walk off ledges)|Left Shift|
|Mine (hold)|Left mouse button|
|Place (hold to keep placing)|Right mouse button|
|New world / reload / save / delete save|1 / 2 / 3 / 4|
|Wireframe view (editor)|F1, or console: `viewmode wireframe`|

## Copyright

© 2026 Jaime Ortiz Rozalen. All rights reserved.

Sounds: Kenney (kenney.nl), CC0

