# wiz3D — Quick Start


## DirectX 9/10/11 Setup

1. Check if your game is 32bit or 64bit.
2. Copy the ENTIRE contents of the `dx9/x86` or `dx9/x64` folder 
   into the game's folder next to the game .exe. Similarly for DirectX 
   10/11, copy the contents of `dx10-11/x86` or `dx10-11/x64` folder.
3. (Optional) Open wiz3D_Config.xml and change settings (instructions
   in wiz3D_Config.xml).
4. Launch the game normally. The proxy loads automatically.


## DirectX 8 Setup (currently in testing)

1. Copy the ENTIRE contents of the `dx8` folder into the game's folder
   next to the game .exe.
2. (Optional) Open wiz3D_Config.xml and change settings (instructions
   in wiz3D_Config.xml).
3. Launch the game normally. The proxy loads automatically.


## DirectX 7 Setup (currently in testing)

1. Copy the ENTIRE contents of the `dx7` folder into the game's folder
   next to the game .exe.
2. (Optional) Open wiz3D_Config.xml and change settings (instructions 
   in wiz3D_Config.xml).
3. Launch the game normally. The proxy loads automatically.


## HD3D Setup

1. Check if your game is 32bit or 64bit.
2. Copy the ENTIRE contents of the `HD3D/x86` or `HD3D/x64` folder into 
   the game's folder next to the game .exe:
3. (Optional) Open HD3D_wiz3D_Config.xml and change settings (instructions
   in HD3D_wiz3D_Config.xml).
4. Launch the game.
5. Enable 'Stereoscopic 3D' or 'HD3D' in the game settings.


## 3D Vision Direct Setup

1. Check if your game is 32bit or 64bit.
2. Copy the ENTIRE contents of the `3D-vision-direct/x86` or `3D-vision-direct/x64` 
   folder into the game's folder next to the game .exe:
3. (Optional) Open 3D-vision-direct_wiz3D_Config.xml and change settings (instructions
   in 3D-vision-direct_wiz3D_Config.xml).
4. Launch the game.
5. Enable 'Stereoscopic 3D' or 'HD3D' in the game settings.


## OpenGL QB Stereo Setup

1. Check if your game is 32bit or 64bit.
2. Copy the ENTIRE contents of the `opengl-quad-buffer-stereo/x86` or 
   `opengl-quad-buffer-stereo/x64` folder into the game's folder next to the game .exe:
3. (Optional) Open opengl-quad-buffer-stereo_wiz3D_Config.xml and change 
   settings (instructions in opengl-quad-buffer-stereo_wiz3D_Config.xml).
4. Launch the game.
5. Enable 'Stereoscopic 3D' or 'HD3D' in the game settings.

---------------------------------------------------------------------------------

## To Remove

Use the corresponding Uninstall_wiz3D.bat.
The game will load the real system DLLs again automatically.

## Troubleshooting

- Game won't launch? → Check you have d3dx9_43.dll (install DirectX runtime)
- No stereo effect? → Make sure EnableStereo="1" in wiz3D_Config.xml
- Wrong display mode? → Change OutputMethod in wiz3D_Config.xml
- Game crashes? → Remove d3d9.dll from game folder to disable
