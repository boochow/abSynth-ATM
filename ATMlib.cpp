#include "ATMlib.h"

#ifdef ATMLIB_RAM_SONG
#define READ_BYTE(p)  *((p))
#define READ_WORD(p)  *((p))
#else
#define READ_BYTE(p)  pgm_read_byte(p)
#define READ_WORD(p)  pgm_read_word(p)
#endif

ATMLIB_CONSTRUCT_ISR(OCR4A)

byte trackCount;
const word *trackList;
const byte *trackBase;
uint8_t pcm __attribute__((used)) = 128;
bool half __attribute__((used));

byte ChannelActiveMute = 0b11110000;
//                         ||||||||
//                         |||||||└->  0  channel 0 is muted (0 = false / 1 = true)
//                         ||||||└-->  1  channel 1 is muted (0 = false / 1 = true)
//                         |||||└--->  2  channel 2 is muted (0 = false / 1 = true)
//                         ||||└---->  3  channel 3 is muted (0 = false / 1 = true)
//                         |||└----->  4  channel 0 is Active (0 = false / 1 = true)
//                         ||└------>  5  channel 1 is Active (0 = false / 1 = true)
//                         |└------->  6  channel 2 is Active (0 = false / 1 = true)
//                         └-------->  7  channel 3 is Active (0 = false / 1 = true)

//Imports
extern uint16_t cia;

// Exports
osc_t osc[4];


const word noteTable[64] PROGMEM = {
  0,
  262,  277,  294,  311,  330,  349,  370,  392,  415,  440,  466,  494,
  523,  554,  587,  622,  659,  698,  740,  784,  831,  880,  932,  988,
  1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976,
  2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951,
  4186, 4435, 4699, 4978, 5274, 5588, 5920, 6272, 6645, 7040, 7459, 7902,
  8372, 8870, 9397,
};


struct ch_t {
  const byte *ptr;
  byte note;

  // Nesting
  word stackPointer[7];
  byte stackCounter[7];
  byte stackTrack[7]; // note 1
  byte stackIndex;

  // Looping
  word delay;
  byte counter;
  byte track;

  // External FX
  word freq;
  byte vol;
  bool mute;

  // Volume & Frequency slide FX
  char volfreSlide;
  byte volfreConfig;
  byte volfreCount;
  byte volInitial;

  // Arpeggio or Note Cut FX
  byte arpNotes;       // notes: base, base+[7:4], base+[7:4]+[3:0], if FF => note cut ON
  byte arpTiming;      // [7] = reserved, [6] = not third note ,[5] = retrigger, [4:0] = tick count
  byte arpCount;

  // Retrig FX
  byte reConfig;       // [7:2] = , [1:0] = speed
  byte reCount;

  // Transposition FX
  char transConfig;

  // Tremolo or Vibrato FX
  byte treviDepth;
  byte treviConfig;
  byte treviCount;

  // Glissando FX
  char glisConfig;
  byte glisCount;

};

ch_t channel[4];

uint16_t read_vle(const byte **pp) {
  word q = 0;
  byte d;
  do {
    q <<= 7;
    d = READ_BYTE(*pp++);
    q |= (d & 0x7F);
  } while (d & 0x80);
  return q;
}

static inline const byte *getTrackPointer(byte track) {
  return trackBase + READ_WORD(&trackList[track]);
}


void ATMsynth::play(const byte *song) {

  // cleanUp stuff first
  memset(channel,0,sizeof(channel));

  // Initializes ATMsynth
  // Sets sample rate and tick rate
  cia = 15625 / 25;

  // Sets up the ports, and the sample grinding ISR

  osc[3].freq = 0x0001; // Seed LFSR
  channel[3].freq = 0x0001; // xFX

  TCCR4A = 0b01000010;    // Fast-PWM 8-bit
  TCCR4B = 0b00000001;    // 62500Hz
  OCR4C  = 0xFF;          // Resolution to 8-bit (TOP=0xFF)
  OCR4A  = 0x80;
  TIMSK4 = 0b00000100;


  // Load a melody stream and start grinding samples
  // Read track count
  trackCount = READ_BYTE(song++);
  // Store track list pointer
  trackList = (word*)song;
  // Store track pointer
  trackBase = (song += (trackCount << 1)) + 4;
  // Fetch starting points for each track
  for (unsigned n = 0; n < 4; n++) {
    channel[n].ptr = getTrackPointer(READ_BYTE(song++));
  }
}

// Stop playing, unload melody
void ATMsynth::stop() {
  TIMSK4 = 0; // Disable interrupt
  memset(channel,0,sizeof(channel));
}

// Start grinding samples or Pause playback
void ATMsynth::playPause() {
  TIMSK4 = TIMSK4 ^ 0b00000100; // toggle disable/enable interrupt
}

// Mute music on a channel, so it's ready for Sound Effects
void ATMsynth::mute(byte ch) {
  ChannelActiveMute ^ (1 << ch );
}

// Unmute music on a channel, after having played Sound Effects
void ATMsynth::unmute(byte ch) {
  ChannelActiveMute | (1 << ch );
}

__attribute__((used))
void ATM_playroutine() {
  ch_t *ch;

  // if all channels are inactive, stop playing
  if (!(ChannelActiveMute & 0xF0))
  {
    TIMSK4 = 0; // Disable interrupt
    memset(channel,0,sizeof(channel));
  }

  for (unsigned n = 0; n < 4; n++) {
    ch = &channel[n];

    // Skip Channel (stop channel)
    if (ChannelActiveMute & (1<<(n+4))) {

      // Noise retriggering
      if (ch->reConfig) {
        if (ch->reCount >= (ch->reConfig & 0x03)) {
          osc[n].freq = pgm_read_word(&noteTable[ch->reConfig >> 2]);
          ch->reCount = 0;
        }
        else ch->reCount++;
      }
  
  
      //Apply Glissando
      if (ch->glisConfig) {
        if (ch->glisCount >= (ch->glisConfig & 0x7F)) {
          if (ch->glisConfig & 0x80) ch->note -= 1;
          else ch->note += 1;
          if (ch->note < 1) ch->note = 1;
          else if (ch->note > 63) ch->note = 63;
          ch->freq = pgm_read_word(&noteTable[ch->note]);
          ch->glisCount = 0;
        }
        else ch->glisCount++;
      }
  
  
      // Apply volume/frequency slides
      if (ch->volfreSlide) {
        if (!ch->volfreCount) {
          int16_t vf = ((ch->volfreConfig & 0x40) ? ch->freq : ch->vol);
          vf += (ch->volfreSlide);
          if (!(ch->volfreConfig & 0x80)) {
            if (vf < 0) vf = 0;
            else if (ch->volfreConfig & 0x40) if (vf > 9397) vf = 9397;
            else if (!(ch->volfreConfig & 0x40)) if (vf > 63) vf = 63;
          }
          (ch->volfreConfig & 0x40) ? ch->freq = vf : ch->vol = vf;
        }
        if (ch->volfreCount++ >= (ch->volfreConfig & 0x3F)) ch->volfreCount = 0;
      }
  
  
      // Apply Arpeggio or Note Cut
      if (ch->arpNotes && ch->note) {
        if ((ch->arpCount & 0x1F) < (ch->arpTiming & 0x1F)) ch->arpCount++;
        else {
          if ((ch->arpCount & 0xE0) == 0x00) ch->arpCount = 0x20;
          else if ((ch->arpCount & 0xE0) == 0x20 && !(ch->arpTiming & 0x40) && (ch->arpNotes != 0xFF)) ch->arpCount = 0x40;
          else ch->arpCount = 0x00;
          byte arpNote = ch->note;
          if ((ch->arpCount & 0xE0) != 0x00) {
            if (ch->arpNotes == 0xFF) arpNote = 0;
            else arpNote += (ch->arpNotes >> 4);
          }
          if ((ch->arpCount & 0xE0) == 0x40) arpNote += (ch->arpNotes & 0x0F);
          ch->freq = pgm_read_word(&noteTable[arpNote + ch->transConfig]);
        }
      }
  
  
      // Apply Tremolo or Vibrato
      if (ch->treviDepth) {
        int16_t vt = ((ch->treviConfig & 0x40) ? ch->freq : ch->vol);
        vt = (ch->treviCount & 0x80) ? (vt + (ch->treviDepth & 0x1F)) : (vt - (ch->treviDepth & 0x1F));
        if (vt < 0) vt = 0;
        else if (ch->treviConfig & 0x40) if (vt > 9397) vt = 9397;
        else if (!(ch->treviConfig & 0x40)) if (vt > 63) vt = 63;
        (ch->treviConfig & 0x40) ? ch->freq = vt : ch->vol = vt;
        if ((ch->treviCount & 0x1F) < (ch->treviConfig & 0x1F)) ch->treviCount++;
        else {
          if (ch->treviCount & 0x80) ch->treviCount = 0;
          else ch->treviCount = 0x80;
        }
      }
  
  
      if (ch->delay) ch->delay--;
      else {
        do {
          byte cmd = READ_BYTE(ch->ptr++);
          if (cmd < 64) {
            // 0 … 63 : NOTE ON/OFF
            if (ch->note = cmd) ch->note += ch->transConfig;
            ch->freq = pgm_read_word(&noteTable[ch->note]);
            if (ch->arpTiming & 0x20) ch->arpCount = 0; // ARP retriggering
            ch->vol = ch->volInitial; // !! added only for abSynth ATM to add decaying!!
//            if ((ch->volfreConfig & 0x40) == 0) { // Tremoro & Vibrato retriggering
//              ch->vol = ch->volInitial; 
//              ch->volfreCount = 0;
//            }
          } else if (cmd < 160) {
            // 64 … 159 : SETUP FX
            switch (cmd - 64) {
              case 0: // Set volume
                ch->vol = READ_BYTE(ch->ptr++);
                ch->volInitial = ch->vol;
                break;
              case 1: case 4: // Slide volume/frequency ON
                ch->volfreSlide = READ_BYTE(ch->ptr++);
                ch->volfreConfig = (cmd - 64) == 1 ? 0x00 : 0x40;
                break;
              case 2: case 5: // Slide volume/frequency ON advanced
                ch->volfreSlide = READ_BYTE(ch->ptr++);
                ch->volfreConfig = READ_BYTE(ch->ptr++);
                break;
              case 3: case 6: // Slide volume/frequency OFF (same as 0x01 0x00)
                ch->volfreSlide = 0;
                break;
              case 7: // Set Arpeggio
                ch->arpNotes = READ_BYTE(ch->ptr++);    // 0x40 + 0x03
                ch->arpTiming = READ_BYTE(ch->ptr++);   // 0x40 (no third note) + 0x20 (toggle retrigger) + amount
                break;
              case 8: // Arpeggio OFF
                ch->arpNotes = 0;
                break;
              case 9: // Set Retriggering (noise)
                ch->reConfig = READ_BYTE(ch->ptr++);    // RETRIG: point = 1 (*4), speed = 0 (0 = fastest, 1 = faster , 2 = fast)
                break;
              case 10: // Retriggering (noise) OFF
                ch->reConfig = 0;
                break;
              case 11: // ADD Transposition
                ch->transConfig += (char)READ_BYTE(ch->ptr++);
                break;
              case 12: // SET Transposition
                ch->transConfig = READ_BYTE(ch->ptr++);
                break;
              case 13: // Transposition OFF
                ch->transConfig = 0;
                break;
              case 14: case 16: // SET Tremolo/Vibrato
                ch->treviDepth = READ_BYTE(ch->ptr++);
                ch->treviConfig = READ_BYTE(ch->ptr++) + ((cmd - 64) == 14 ? 0x00 : 0x40);
                break;
              case 15: case 17: // Tremolo/Vibrato OFF
                ch->treviDepth = 0;
                break;
              case 18: // Glissando
                ch->glisConfig = READ_BYTE(ch->ptr++);
                break;
              case 19: // Glissando OFF
                ch->glisConfig = 0;
                break;
              case 20: // SET Note Cut
                ch->arpNotes = 0xFF;                        // 0xFF use Note Cut
                ch->arpTiming = READ_BYTE(ch->ptr++);   // tick amount
                break;
              case 21: // Note Cut OFF
                ch->arpNotes = 0;
                break;
              case 93: // SET tempo
                cia = 15625 / READ_BYTE(ch->ptr++);
                break;
              case 94: // Goto advanced
                channel[0].track = READ_BYTE(ch->ptr++);
                channel[1].track = READ_BYTE(ch->ptr++);
                channel[2].track = READ_BYTE(ch->ptr++);
                channel[3].track = READ_BYTE(ch->ptr++);
                break;
              case 95: // Stop channel
                ChannelActiveMute = ChannelActiveMute ^ (1<<(n+4));
                ch->delay = 1; // to exit do while loop
                break;
            }
          } else if (cmd < 224) {
            // 160 … 223 : DELAY
            ch->delay = cmd - 159;
          } else if (cmd == 224) {
            // 224: LONG DELAY
            ch->delay = read_vle(&ch->ptr) + 129;
          } else if (cmd < 252) {
            // 225 … 251 : RESERVED
            switch(cmd - 225) {
              case 0: //NOP
              break;
              case 1: //NOP + 1byte arg
              ch->ptr++;
              break;
              case 2: //NOP + 2bytes arg
              ch->ptr++;
              ch->ptr++;
              break;
              default:
              break;
           }
          } else if (cmd == 252 || cmd == 253) {
            // 252 (253) : CALL (REPEATEDLY)
            // Stack PUSH
            ch->stackCounter[ch->stackIndex] = ch->counter;
            ch->stackTrack[ch->stackIndex] = ch->track; // note 1
            ch->counter = cmd == 252 ? 0 : READ_BYTE(ch->ptr++);
            ch->track = READ_BYTE(ch->ptr++);
            ch->stackPointer[ch->stackIndex] = ch->ptr - trackBase;
            ch->stackIndex++;
            ch->ptr = getTrackPointer(ch->track);
          } else if (cmd == 254) {
            // 254 : RETURN
            if (ch->counter > 0) {
              // Repeat track
              ch->counter--;
              ch->ptr = getTrackPointer(ch->track);
            } else {
              // Check stack depth
              if (ch->stackIndex == 0) {
                // End-Of-File
                ch->delay = 0xFFFF;
              } else {
                // Stack POP
                ch->stackIndex--;
                ch->ptr = ch->stackPointer[ch->stackIndex] + trackBase;
                ch->counter = ch->stackCounter[ch->stackIndex];
                ch->track = ch->stackTrack[ch->stackIndex]; // note 1
              }
            }
          } else if (cmd == 255) {
            // 255 : EMBEDDED DATA
            ch->ptr += read_vle(&ch->ptr);
          }
        } while (ch->delay == 0);
  
        ch->delay--;
      }
  
      if (!(ChannelActiveMute & (1 << n))) {
        if (n == 3) {
          // Half volume, no frequency for noise channel
          osc[n].vol = ch->vol >> 1;
        } else {
          osc[n].freq = ch->freq;
          osc[n].vol = ch->vol;
        }
      }
    }
  }
}
