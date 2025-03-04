# PBR (Physically Based Rendering)

## pbr_spheres
![alt text](../../images/screenshots/pbr/pbr_spheres.png?raw=true)

**Basic PBR implementation with direct and indirect lighting**

Renders the typical sphere grid of metalness on X-axis and roughness on Y-axis. 
Uses IBL lighting by default, point lights can be turned optionally turned on.

Shaders are found in the [assets/projects/201_202_pbr_spheres](https://github.com/chaoticbob/GraphicsExperiments/tree/main/assets/projects/201_202_pbr_spheres) directory.

HDRI images for IBL are from [Poly Haven](https://polyhaven.com/hdris), which are generously provided under [CC0](https://polyhaven.com/license).

Notes:
* Spheres can spin.
* Lighting is done in world space.
* IBL uses rectangular images and not cube maps.

## pbr_camera
![alt text](../../images/screenshots/pbr/pbr_camera.png?raw=true)

**Slightly more sophisticated PBR implementation with normal map**

PBR render of a [camera model](https://sketchfab.com/3d-models/dae-bilora-bella-46-camera-game-ready-asset-eeb9d9f0627f4783b5d16a8732f0d1a4)
with normal mapping. Tangent and bitangent are calculated using [MikkTSpace](https://github.com/mmikk/MikkTSpace).

Shaders are found in the [assets/projects/203_204_pbr_camera](https://github.com/chaoticbob/GraphicsExperiments/tree/main/assets/projects/203_204_pbr_camera) directory.

HDRI images for IBL are from [Poly Haven](https://polyhaven.com/hdris), which are generously provided under [CC0](https://polyhaven.com/license).

Notes:
* Model can be spin.
* Lighting is done in world space.
* IBL uses rectangular images and not cube maps.

## pbr_explorer
![alt text](../../images/screenshots/pbr/pbr_explorer.png?raw=true)

**Exploring the parameters of PBR**

Toy program to explore the various Distribution, Fresnel, and Geometry functions for implementing
PBR. 

Knob is borrowed from [Google Filament](https://github.com/google/filament) and monkey is generated from [Blender](https://www.blender.org/).

Shaders are found in the [assets/projects/251_252_pbr_explorer](https://github.com/chaoticbob/GraphicsExperiments/tree/main/assets/projects/251_252_pbr_explorer) directory.

HDRI images for IBL are from [Poly Haven](https://polyhaven.com/hdris), which are generously provided under [CC0](https://polyhaven.com/license).

Notes:
* The view camera can spin around the scene.
* Lighting is done in world space.
* IBL uses rectangular images and not cube maps.

## pbr_material_properties
![alt text](../../images/screenshots/pbr/pbr_material_properties.png?raw=true)

**Exploring PBR material properties**

First pass implementation of the [Google Filament](https://github.com/google/filament) standard model with the base set of parameters.

Knob is borrowed from [Google Filament](https://github.com/google/filament) and monkey is generated from [Blender](https://www.blender.org/).

Shaders are found in the [assets/projects/253_254_pbr_material_properties](https://github.com/chaoticbob/GraphicsExperiments/tree/main/assets/projects/253_254_pbr_material_properties) directory.

HDRI images for IBL are from [Poly Haven](https://polyhaven.com/hdris), which are generously provided under [CC0](https://polyhaven.com/license).

Notes:
* Lighting is done in world space.
* IBL uses rectangular images and not cube maps.

## pbr_material_textures
![alt text](../../images/screenshots/pbr/pbr_material_textures.png?raw=true)

**Exploring PBR material textures**

More advanced PBR material with textures sets and different lighting environments.

Knob is borrowed from [Google Filament](https://github.com/google/filament) and monkey is generated from [Blender](https://www.blender.org/).

Shaders are found in the [assets/projects/255_256_pbr_material_textures](https://github.com/chaoticbob/GraphicsExperiments/tree/main/assets/projects/255_256_pbr_material_textures) directory.

Textures and HDRI images for IBL are from [Poly Haven](https://polyhaven.com/hdris), which are generously provided under [CC0](https://polyhaven.com/license).

Notes:
* Lighting is done in world space.
* IBL uses rectangular images and not cube maps.
