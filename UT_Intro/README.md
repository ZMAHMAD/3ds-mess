# bruh

Rubbish title for a 3DS software that, (maybe terribly), plays the undertale intro.

## To build cia quickly:
- Only once:
```bash
chmod +x makecia.sh
```
- Each build:
```bash
./makecia.sh
```

## To build each part:
3dsx: make
cxi: cxitool bruh.3dsx bruh.cxi
cia: makerom -v -f cia -o bruh.cia -target t -i bruh.cxi:0:0 -ignoresign -icon bruh.smdh
