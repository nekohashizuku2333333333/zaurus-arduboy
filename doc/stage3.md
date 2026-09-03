# Stage 3: IPK Packaging

## Conclusion

The first installable package has been built:

```text
dist/zaurusarduboy_0.1_arm.ipk
```

SHA256:

```text
85632ec4522877a3ab6d86d619cb4d9239a9e555235900a08c77bc37905f4daa
```

## Package Layout

The IPK uses the old Sharp/Zaurus-compatible gzip tar outer format:

```text
./debian-binary
./control.tar.gz
./data.tar.gz
```

Installed files:

```text
/home/QtPalmtop/bin/zaurusarduboy
/home/QtPalmtop/apps/Games/zaurusarduboy.desktop
/home/QtPalmtop/pics/zaurusarduboy.xpm
```

## Build Package

After the ARM binary has been copied to `dist/zaurusarduboy`:

```sh
sh scripts/package_ipk.sh
```

The package script creates:

```text
dist/zaurusarduboy_0.1_arm.ipk
```

## Verification

Package structure was checked with:

```sh
tar tzf dist/zaurusarduboy_0.1_arm.ipk
```

The ARM binary was checked with:

```sh
file dist/zaurusarduboy
readelf -d dist/zaurusarduboy
```

Observed dynamic dependencies:

```text
libqpe.so.1
libqte.so.2
libm.so.6
libc.so.6
```

No `libstdc++.so.6` or `libgcc_s.so.1` direct dependency is present.

## Runtime Notes

The desktop entry launches the graphical frontend directly:

```sh
zaurusarduboy
```

Use the top Load button to open the built-in browser and choose an
Arduboy `.hex` file.  A `.hex` path can still be passed as the first
argument for command-line testing.

EEPROM save data is stored at:

```text
$HOME/.arduboy-eeprom.bin
```
