# Release Build

## Tag Pattern

- Use: `blender-vfx-5.2-YYYY-MM-DD`
- Keep the release folder, zip name, and GitHub tag identical.

## Build

```powershell
$tag = 'blender-vfx-5.2-YYYY-MM-DD'
$releaseRoot = 'E:\blender_modify\release'
$prefix = Join-Path $releaseRoot $tag
$zip = Join-Path $releaseRoot ($tag + '.zip')

& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --build 'E:\blender_modify\build_windows_x64_vc17_Release' --target blender --config Release -- /m:28

& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --install 'E:\blender_modify\build_windows_x64_vc17_Release' --config Release --prefix $prefix
```

## Remove PDBs

```powershell
Remove-Item -LiteralPath (Join-Path $prefix 'blender.pdb') -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $prefix '5.2\python\lib\venv\scripts\nt\venvlauncher.pdb') -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $prefix '5.2\python\lib\venv\scripts\nt\venvwlauncher.pdb') -ErrorAction SilentlyContinue
```

## Zip And Checksum

```powershell
tar -a -c -f $zip -C $releaseRoot $tag
Get-FileHash -Algorithm SHA256 $zip
```

## Release Notes

- Draft notes beside the zip under `E:\blender_modify\release\`.
- Mention shipped features, major fixes, and any user-visible constraints.
- Sync README updates before publishing if the release introduces user-facing features.
