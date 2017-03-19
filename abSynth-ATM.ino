#include <Arduino.h>
#include "ATMlib.h"
//uncomment this if using PROTON
//#define __PROTON__

#ifdef __PROTON__
#include "Arduboy.h"
Arduboy arduboy;
#else
#include "Arduboy2.h"
Arduboy2 arduboy;
#endif

#include "song.h"
ATMsynth ATM;

void setup() {
  arduboy.begin();
  arduboy.setFrameRate(15);
  arduboy.clear();
  arduboy.print(F("SCORE SIZE:"));
  arduboy.print(sizeof(music));
  ATM.play(music);
}

void loop() {
  if (!(arduboy.nextFrame())) return;
  arduboy.display();
}

