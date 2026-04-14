#!/bin/sh

make
cxitool UT_Intro.3dsx UT_Intro.cxi
makerom -v -f cia -o UT_Intro.cia -target t -i UT_Intro.cxi:0:0 -ignoresign -icon UT_Intro.smdh
