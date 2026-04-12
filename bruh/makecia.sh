#!/bin/sh

make
cxitool bruh.3dsx bruh.cxi
makerom -v -f cia -o bruh.cia -target t -i bruh.cxi:0:0 -ignoresign -icon bruh.smdh