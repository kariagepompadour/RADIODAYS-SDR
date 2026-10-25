#!/bin/bash
# Draws the firmware's analogue radio screen on a PC and checks display consistency.
# Needs: g++, python3, and the M5GFX library (for its font tables only): M5GFX=/path/to/libraries/M5GFX ./run.sh
set -e
cd "$(dirname "$0")"
INO=${INO:-../../RadioDays_SDR/RadioDays_SDR.ino}
F=${M5GFX:?set M5GFX to the M5GFX library folder}/src/lgfx/Fonts
mkdir -p build out
cp $F/GFXFF/FreeSans9pt7b.h $F/GFXFF/FreeSansBold9pt7b.h $F/glcdfont.h build/
python3 - "$INO" <<'PY'
import sys
s=open(sys.argv[1]).read()
def func(name):
    a=s.index(name); b=s.index('{',a); d=0; i=b
    while True:
        if s[i]=='{': d+=1
        elif s[i]=='}':
            d-=1
            if d==0: return s[a:i+1]+"\n"
        i+=1
def between(a,b):
    i=s.index(a); j=s.index(b,i); return s[i:j]
out=[s[s.index('enum AuState'):s.index('enum AuState')+s[s.index('enum AuState'):].index('\n')+1],
     func('static uint8_t auStateNow()'), func('static const char* auStateName(uint8_t st)'),
     func('static int defaultStepForMode(int mode)'),
     func('static void setScreen(Screen s)'),
     func('static void scanRequest(int dir)'), func('static bool scanRunning()'), func('static void setTuneAuto(bool a)'), func('static void autoTurn(int detents)'),
     func('static void changeMode(int m)'),
     func('static void txt(int x'), func('static void header(const char* title)'), func('static void drawBar(int x'),
     s[s.index('// ==== ANALOG BEGIN'):s.index('// ==== ANALOG END')],
     s[s.index('static constexpr int LCD_MAIN_W'):s.index('\n',s.index('static constexpr int LCD_MAIN_W'))+1],
     func('static void statusText(char*'), func('static void drawLamp('), func('static void drawChrome()'), func('static uint16_t lcdInk('), func('static void drawLcdLine('), func('static void drawTinyJst('), func('static void drawClockPanel('), func('static void drawLcdLine1('), func('static void drawRadioAnalog(bool full)'),
     func('static void drawRadioSimple(bool full)'),
     func('static bool macNoticeActive()'), func('static void drawMacNotice(bool full)'), func('static void drawRadio(bool full)'),
     between('static constexpr int GLOBE_R','static void handleGlobe('),
     func('static void drawDiag(bool full)'),
     'static void loopDispatch(KeyInput& k, KnobInput& kn) {\n'+between('// ==== DISPATCH BEGIN','// ==== DISPATCH END')+'}\n']
open('build/extract.inc','w').write("\n".join(out))
PY
cp harness.cpp build/
cp ../../RadioDays_SDR/globe_land.h ../../RadioDays_SDR/RadioDays_Types.h build/
echo '#pragma once' > build/Arduino.h
cp ../../RadioDays_SDR/RadioDays_AudioProto.h ../../RadioDays_SDR/RadioDays_Playout.h build/
(cd build && g++ -std=c++17 -O1 -w -I. -o harness harness.cpp && OUT=../out ./harness)
echo "PPM images are in out/"
