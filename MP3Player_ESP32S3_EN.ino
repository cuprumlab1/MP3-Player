/*
  MP3 Player - ESP32-S3 SuperMini + PCM5102A + SD (SPI) + 0.96" SSD1306 OLED (I2C)

  Required libraries (Library Manager / GitHub):
    - U8g2             (olikraus)    -> draws text and shapes on the OLED
    - ESP32-audioI2S   (schreibfaul1, v3.x) -> reads audio files from the SD card and sends them to the PCM5102

  Board settings: "ESP32S3 Dev Module", USB CDC On Boot = Enabled,
                  Partition Scheme = "Huge APP (3MB No OTA/1MB SPIFFS)"

  Wiring:
    PCM5102A : BCK=GPIO5  LRCK=GPIO6  DIN=GPIO7   (SCK -> GND, XSMT -> 3V3, FMT -> GND)
    SD (SPI) : SCK=12  MOSI=11  MISO=13  CS=10
    OLED I2C : SCL=8   SDA=9
    Buttons  : UP=GPIO1, DOWN=GPIO2, LEFT=GPIO3, RIGHT=GPIO4, OK=GPIO14
               Each button: one leg to the GPIO, the other leg to GND (internal pull-ups are used)

  SD card layout:
    /Podcast/...        -> Podcast menu (subfolders are scanned too)
    /Playlists/plN.txt  -> playlists 1..9 (created automatically)
    Everything else (including subfolders) -> Music > All songs

  Controls on the player screen:
    Left/Right : move the focus between Previous - Play/Pause - Next
    Up/Down    : move between the button row and the Shuffle/Sequential button
    OK (short press) : activate the focused button
    OK (hold 3s) on Previous   : rewind 5s, keep holding to keep rewinding (every 1s)
    OK (hold 3s) on Next       : fast-forward 5s, keep holding to keep going
    OK (hold 3s) on Play/Pause : exit back to the song list
*/

// ====================== LIBRARIES ======================
#include <Arduino.h>        // core Arduino library
#include <SPI.h>            // SPI communication (for the SD card)
#include <SD.h>             // read/write the SD card
#include <Wire.h>           // I2C communication (for the OLED)
#include <U8g2lib.h>        // OLED drawing
#include <Preferences.h>    // stores settings (volume, EQ...) in flash so they survive power-off
#include <vector>           // resizable list (std::vector)
#include <algorithm>        // used for sort, min, max
#include <functional>       // std::function: pass a "function" into another function
#include "Audio.h"          // ESP32-audioI2S

// ====================== PIN DEFINITIONS ======================
#define I2S_BCK     5       // I2S bit clock pin           -> PCM5102 BCK
#define I2S_LRC     6       // left/right channel select   -> PCM5102 LRCK
#define I2S_DOUT    7       // audio data pin              -> PCM5102 DIN
#define SD_SCK      12      // SD card SPI pins
#define SD_MOSI     11
#define SD_MISO     13
#define SD_CS       10
#define OLED_SCL    8       // OLED I2C pins
#define OLED_SDA    9
#define PIN_UP      1       // the 5 push buttons
#define PIN_DOWN    2
#define PIN_LEFT    3       // GPIO3 is a strapping pin: don't hold this button while powering on
#define PIN_RIGHT   4
#define PIN_OK      14      // (change to 35 if you use GPIO35 and PSRAM is not Octal)

// ====================== SETTINGS (adjustable) ======================
#define DEBOUNCE_MS      20     // a button must stay stable for 20ms to count as pressed (noise/bounce filter)
#define REPEAT_DELAY     450    // holding Up/Down/Left/Right longer than 450ms starts auto-repeat
#define REPEAT_MS        110    // interval between auto-repeats (ms)
#define LONG_PRESS_MS    3000   // how long OK must be held to count as a "long press" (seek / exit)
#define SEEK_STEP_S      5      // seconds jumped per seek
#define SEEK_REPEAT_MS   1000   // pause between seeks while OK is still held (ms)
#define MAX_PLAYLISTS    9      // maximum number of playlists
#define MAX_TRACKS       1500   // maximum tracks per list (avoids running out of RAM)
#define MAX_DEPTH        6      // scan subfolders up to 6 levels deep
#define PODCAST_DIR      "/Podcast"
#define PLAYLIST_DIR     "/Playlists"

// EQ table: each row is {bass, mid, treble} in dB, the library allows -40..+6
const int8_t EQ_TABLE[3][3] = {
  {  0,  0,  0 },   // EQ1: original sound
  {  6, -2, -2 },   // EQ2: bass boost
  { -3,  5,  3 }    // EQ3: vocal + instrument boost
};
const char* EQ_NAMES[3] = { "EQ1 Flat", "EQ2 Bass", "EQ3 Vocal" };

// Fonts (U8g2 font names)
#define FONT_BIG    u8g2_font_unifont_t_vietnamese1   // 16px font, supports Vietnamese diacritics
#define FONT_SMALL  u8g2_font_6x10_tf                 // small font for headers
#define FONT_TINY   u8g2_font_5x7_tf                  // very small font for the time display

// ====================== GLOBAL OBJECTS ======================
// 128x64 OLED, SSD1306 chip, hardware I2C, "F" = full frame buffer in RAM
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
Audio audio;          // audio player
Preferences prefs;    // persistent settings storage

// ====================== SCREENS ======================
// enum = gives names to numbers. Every screen gets a readable "name".
enum Screen : uint8_t {
  SC_MAIN,        // main menu: Music / Podcast / Setting
  SC_MUSIC,       // All songs / Playlists
  SC_ALL,         // list of all songs
  SC_PLAYLISTS,   // list of playlists
  SC_PL_SONGS,    // songs inside one playlist
  SC_PICK_PL,     // choose a playlist to add a song to
  SC_PODCAST,     // list of podcasts
  SC_SETTING,     // Volume / EQ
  SC_VOLUME,      // volume adjustment
  SC_EQ,          // EQ selection
  SC_PLAYER,      // now-playing screen
  SC_CONFIRM,     // confirmation prompt (delete...)
  SC_COUNT        // (only used to count the screens)
};
Screen screen = SC_MAIN;     // current screen
int sel[SC_COUNT] = {0};     // selected position of EACH screen (so going back keeps the position)
bool needRedraw = true;      // true = the screen needs to be redrawn

// --- Data types for the buttons ---
// (must be declared here, before the first function, otherwise the Arduino IDE reports a prototype error)
enum : uint8_t { B_UP = 1, B_DOWN = 2, B_LEFT = 4, B_RIGHT = 8, B_OK = 16 };  // one bit per button
enum EvType : uint8_t {          // button "event" types
  EV_NONE, EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT,
  EV_OK_DOWN,      // OK was just pressed down
  EV_OK_RELEASE,   // OK was just released (short press)
  EV_OK_LONG       // OK has been held long enough (repeats while still held)
};
struct Event { EvType type; bool rep; };   // rep = true if the event is an auto-repeat caused by holding
const EvType DIR_EV[4] = { EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT };

// ====================== DATA ======================
uint8_t volume = 10;         // volume 1..20 (default 10)
uint8_t eqMode = 0;          // 0 = EQ1, 1 = EQ2, 2 = EQ3
bool shuffleMode = false;    // false = play in order, true = shuffle

std::vector<String> songs, podcasts, plTracks;  // file paths: music, podcasts, tracks of the opened playlist
std::vector<int> plIdx;                         // indexes of the playlists that exist
bool plExists[MAX_PLAYLISTS];                   // does playlist i exist?
uint16_t plCount[MAX_PLAYLISTS];                // number of tracks in playlist i
int curPl = 0;                                  // currently opened playlist
String pendingPath;                             // song waiting to be added to a playlist

// --- Playback state ---
std::vector<String>* queuePtr = nullptr;  // points to the list being played (songs / podcasts / plTracks)
std::vector<uint16_t> order;              // play order (sequential or shuffled)
int orderPos = 0;                         // current position inside "order"
Screen backScreen = SC_ALL;               // screen to return to when leaving the player
String curTitle;                          // title of the current track
bool paused = false;                      // is playback paused?
uint32_t trackStartMs = 0;                // time the track started (millis)
uint8_t failStreak = 0;                   // counts consecutive tracks that failed to play
uint8_t playerFocus = 1;                  // focused item: 0=previous, 1=play/pause, 2=next, 3=play mode
bool playerOkHeld = false;                // was OK pressed down while on the player screen?
uint32_t lastShownSec = 0xFFFFFFFF;       // last second that was drawn (to know when to redraw)
uint32_t lastDur = 0;                     // previous total duration (detects VBR files with estimated length)
bool durEstimated = false;                // true = total duration is only an estimate

// --- Confirmation screen ---
enum { CF_DEL_PL, CF_RM_TRACK };          // delete playlist / remove track from playlist
uint8_t confirmAction = CF_DEL_PL;
int confirmArg = 0;
Screen confirmBack = SC_PLAYLISTS;

// --- Scrolling text (marquee) and small notifications (toast) ---
bool marqueeActive = false;               // is any text scrolling? (to keep redrawing)
uint32_t marqueeT0 = 0;                   // time the text scrolling started
char toastText[20] = "";                  // notification text
uint32_t toastUntil = 0;                  // show the notification until this time

// ====================== HELPER FUNCTIONS ======================

// Get the track name from a path: "/Music/A/abc.mp3" -> "abc"
String baseName(const String& p) {
  int s = p.lastIndexOf('/');                              // position of the last '/'
  String n = (s >= 0) ? p.substring(s + 1) : p;            // cut out the file name
  int d = n.lastIndexOf('.');                              // position of the last '.'
  if (d > 0) n = n.substring(0, d);                        // remove the .mp3 extension
  return n;
}

// Convert seconds to "mm:ss" (or "h:mm:ss" if longer than 1 hour)
String fmtTime(uint32_t s) {
  char b[16];
  if (s >= 3600) snprintf(b, sizeof b, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
  else snprintf(b, sizeof b, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  return String(b);
}

// Show a small notification in the middle of the screen for 1 second
void toast(const char* t) {
  strlcpy(toastText, t, sizeof toastText);   // copy the text (never exceeds the buffer)
  toastUntil = millis() + 1000;
  needRedraw = true;
}

// Switch to another screen. resetSel=true puts the selection back at the top of the list.
void go(Screen s, bool resetSel = true) {
  screen = s;
  if (resetSel) sel[s] = 0;
  marqueeT0 = millis();       // scrolling text restarts from the beginning
  needRedraw = true;
}

// ====================== SD CARD SCANNING ======================

// Is this an audio file? (based on the file extension)
bool isAudioFile(const String& n) {
  if (n.startsWith(".")) return false;       // skip "._xxx" junk files created by macOS
  String l = n; l.toLowerCase();             // lowercase for comparison
  return l.endsWith(".mp3") || l.endsWith(".wav") || l.endsWith(".flac") || l.endsWith(".aac") ||
         l.endsWith(".m4a") || l.endsWith(".ogg") || l.endsWith(".opus");
}

// Scan a folder (and its subfolders) for audio files and add them to "out"
// depth = current level, root = is this the root folder, skipSpecial = skip /Podcast and /Playlists
void scanDir(const String& dir, std::vector<String>& out, uint8_t depth, bool root, bool skipSpecial) {
  File d = SD.open(dir.c_str());
  if (!d) return;
  if (!d.isDirectory()) { d.close(); return; }
  while (true) {
    File f = d.openNextFile();               // get the next entry in the folder
    if (!f) break;                           // no more entries -> stop
    String name = f.name();
    bool isDir = f.isDirectory();
    f.close();
    int s = name.lastIndexOf('/');
    String base = (s >= 0) ? name.substring(s + 1) : name;   // keep only the name, drop the path
    if (base.startsWith(".")) continue;                      // skip hidden files/folders
    String full = dir;
    if (!full.endsWith("/")) full += "/";
    full += base;                                            // full path
    if (isDir) {
      if (depth >= MAX_DEPTH) continue;
      if (base.equalsIgnoreCase("System Volume Information")) continue;   // Windows system folder
      if (root && skipSpecial && (base.equalsIgnoreCase("Podcast") || base.equalsIgnoreCase("Playlists"))) continue;
      scanDir(full, out, depth + 1, false, skipSpecial);     // scan the subfolder (the function calls itself)
    } else if (isAudioFile(base)) {
      if (out.size() < MAX_TRACKS) out.push_back(full);      // add to the list
    }
  }
  d.close();
}

// Sort a list alphabetically (case-insensitive)
void sortList(std::vector<String>& v) {
  std::sort(v.begin(), v.end(), [](const String& a, const String& b) {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  });
}

// ====================== PLAYLISTS ======================

// File path of playlist number i (i starts at 0): "/Playlists/pl1.txt"
String plPath(int i) { return String(PLAYLIST_DIR) + "/pl" + String(i + 1) + ".txt"; }

// Read playlist i from the SD card into "out" (each line of the file is one track path)
void loadPlaylist(int i, std::vector<String>& out) {
  out.clear();
  File f = SD.open(plPath(i).c_str());
  if (!f) return;
  while (f.available()) {
    String l = f.readStringUntil('\n');      // read one line
    l.trim();                                // strip extra spaces / line breaks
    if (l.length() && out.size() < MAX_TRACKS) out.push_back(l);
  }
  f.close();
}

// Write the whole list v to the file of playlist i (overwrites it)
void savePlaylist(int i, const std::vector<String>& v) {
  String p = plPath(i);
  SD.remove(p.c_str());                      // delete the old file
  File f = SD.open(p.c_str(), FILE_WRITE);   // create a new file
  if (!f) return;
  for (auto& s : v) f.println(s);            // one track per line
  f.close();
}

// Count the tracks in playlist i
uint16_t countLines(int i) {
  File f = SD.open(plPath(i).c_str());
  if (!f) return 0;
  uint16_t n = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (l.length()) n++;
  }
  f.close();
  return n;
}

// Refresh the playlist info (which ones exist, how many tracks each)
void refreshPlaylists() {
  plIdx.clear();
  for (int i = 0; i < MAX_PLAYLISTS; i++) {
    plExists[i] = SD.exists(plPath(i).c_str());
    plCount[i] = plExists[i] ? countLines(i) : 0;
    if (plExists[i]) plIdx.push_back(i);
  }
}

// Create a new playlist in the first free slot. Returns its index, or -1 if all 9 are used.
int createPlaylist() {
  for (int i = 0; i < MAX_PLAYLISTS; i++) {
    if (!plExists[i]) {
      File f = SD.open(plPath(i).c_str(), FILE_WRITE);
      if (!f) return -1;
      f.close();
      refreshPlaylists();
      return i;
    }
  }
  return -1;
}

// Add a track to playlist i. Returns: 0 = added, 1 = already in the playlist, 2 = SD write error
int addToPlaylist(int i, const String& path) {
  std::vector<String> v;
  loadPlaylist(i, v);
  for (auto& s : v) if (s == path) return 1;           // duplicate check
  File f = SD.open(plPath(i).c_str(), FILE_APPEND);    // open for appending at the end
  if (!f) return 2;
  f.println(path);
  f.close();
  return 0;
}

// Text shown for each row of the playlist list
String plRowLabel(int i) {
  if (i < (int)plIdx.size()) return "Playlist " + String(plIdx[i] + 1) + " (" + String(plCount[plIdx[i]]) + ")";
  return "+ New playlist";     // last row: create a new playlist
}

// ====================== AUDIO / PLAYBACK ======================
void applyVolume() { audio.setVolume(volume); }    // set the library volume
void applyEQ() { audio.setTone(EQ_TABLE[eqMode][0], EQ_TABLE[eqMode][1], EQ_TABLE[eqMode][2]); }  // set bass/mid/treble

// Build the play order. The track startIdx is the one chosen to play first.
void buildOrder(int startIdx) {
  int n = queuePtr->size();
  order.resize(n);
  for (int i = 0; i < n; i++) order[i] = i;            // sequential order 0,1,2,...
  if (shuffleMode && n > 1) {
    for (int i = n - 1; i > 0; i--) {                  // Fisher-Yates shuffle
      int j = esp_random() % (i + 1);                  // ESP32 hardware random number
      std::swap(order[i], order[j]);
    }
    for (int i = 0; i < n; i++) if (order[i] == startIdx) { std::swap(order[0], order[i]); break; }  // move the chosen track to the front
    orderPos = 0;
  } else {
    orderPos = startIdx;
  }
}

// Start playing the track at position orderPos
void startCurrent() {
  String path = (*queuePtr)[order[orderPos]];          // path of the track to play
  curTitle = baseName(path);
  audio.stopSong();                                    // stop the previous track
  audio.connecttoFS(SD, path.c_str());                 // open the file from the SD card and start playing
  paused = false;
  trackStartMs = millis();
  marqueeT0 = millis();
  lastShownSec = 0xFFFFFFFF;
  lastDur = 0;                                         // reset duration tracking for the new track
  durEstimated = false;
  needRedraw = true;
}

void nextTrack() { orderPos = (orderPos + 1) % order.size(); startCurrent(); }                    // next track (wraps to the start at the end)
void prevTrack() { orderPos = (orderPos + order.size() - 1) % order.size(); startCurrent(); }     // previous track

void togglePause() {
  audio.pauseResume();     // pauses if playing, resumes if paused
  paused = !paused;
  needRedraw = true;
}

void toggleShuffle() {
  shuffleMode = !shuffleMode;
  prefs.putBool("shuf", shuffleMode);                  // save it so it is remembered next time
  int curReal = order[orderPos];                       // remember the current track
  buildOrder(curReal);                                 // rebuild the order, this track stays current
  needRedraw = true;
}

// SEEK: deltaSec > 0 seeks forward, < 0 seeks backward (in seconds)
void seekBy(int deltaSec) {
  int32_t cur = (int32_t)audio.getAudioCurrentTime();   // current position (seconds)
  int32_t dur = (int32_t)audio.getAudioFileDuration();  // total duration (seconds), 0 if unknown
  int32_t target = cur + deltaSec;                      // position we want to jump to
  if (target < 0) target = 0;                           // don't rewind past the start
  if (dur > 0 && target >= dur) {                       // seeking past the end -> go to the next track
    playerOkHeld = false;                               // stop continuous seeking once the new track starts
    nextTrack();
    return;
  }
  if (target != cur) audio.setTimeOffset(target - cur); // relative seek: add/subtract seconds from the current position
  lastShownSec = 0xFFFFFFFF;                            // force the time bar to redraw
  needRedraw = true;
}

// Start playing list q from track number idx. back = screen to return to on exit.
void startPlayback(std::vector<String>* q, int idx, Screen back) {
  if (q->empty()) return;
  queuePtr = q;
  backScreen = back;
  failStreak = 0;
  buildOrder(idx);
  startCurrent();
  screen = SC_PLAYER;
  playerFocus = 1;               // focus Play/Pause by default
  playerOkHeld = false;
  needRedraw = true;
}

// Leave the player screen: stop the music and go back to the list
void exitPlayer() {
  audio.stopSong();
  paused = false;
  playerOkHeld = false;
  go(backScreen, false);
}

// Automatically move to the next track when the current one ends
void autoAdvance() {
  if (screen != SC_PLAYER || paused) return;
  if (millis() - trackStartMs < 800) return;            // wait 0.8s so the library has time to start
  if (audio.isRunning()) return;                        // still playing -> do nothing
  // If we get here the track has ended (or failed to play)
  if (millis() - trackStartMs < 2500) failStreak++; else failStreak = 0;   // a track that dies too fast = error
  if (failStreak >= std::min((int)queuePtr->size(), 5)) {                  // several failures in a row -> stop completely
    exitPlayer();
    toast("Cannot play");
    return;
  }
  nextTrack();
}

// ====================== BUTTONS ======================
// Event queue (8-slot ring buffer): pollButtons() pushes events in, handleEvent() pops them out to process.
Event evq[8];
uint8_t evHead = 0, evTail = 0;

void pushEv(EvType t, bool rep = false) {
  uint8_t n = (evHead + 1) & 7;     // &7 wraps back to 0 after 7
  if (n == evTail) return;          // queue full -> drop the event
  evq[evHead] = { t, rep };
  evHead = n;
}

bool popEv(Event& e) {
  if (evHead == evTail) return false;   // no events waiting
  e = evq[evTail];
  evTail = (evTail + 1) & 7;
  return true;
}

// Read the raw state of the 5 buttons: a bit set to 1 means that button is pressed
uint8_t readRawButtons() {
  uint8_t m = 0;
  if (!digitalRead(PIN_UP))    m |= B_UP;      // pressed button = pin pulled down to GND = LOW
  if (!digitalRead(PIN_DOWN))  m |= B_DOWN;
  if (!digitalRead(PIN_LEFT))  m |= B_LEFT;
  if (!digitalRead(PIN_RIGHT)) m |= B_RIGHT;
  if (!digitalRead(PIN_OK))    m |= B_OK;
  return m;
}

// Poll the buttons: debounce, detect press/release/hold, and push events into the queue
void pollButtons() {
  static uint32_t lastPoll = 0, lastChange = 0;     // "static" = value is kept between calls
  static uint8_t rawPrev = 0, stable = 0;           // rawPrev: previous reading, stable: debounced state
  static uint32_t downAt[5] = {0}, lastRep[5] = {0};// time of press / last repeat for each button
  static bool longFired = false;                    // has OK already triggered its "long press"?

  uint32_t now = millis();
  if (now - lastPoll < 2) return;                   // only poll every 2ms
  lastPoll = now;

  uint8_t raw = readRawButtons();
  if (raw != rawPrev) {                             // state is changing -> restart the timer
    rawPrev = raw;
    lastChange = now;
  } else if (raw != stable && now - lastChange >= DEBOUNCE_MS) {   // stable long enough -> accept it
    uint8_t pressed = raw & ~stable;                // buttons that were just pressed
    uint8_t released = stable & ~raw;               // buttons that were just released
    stable = raw;
    for (int i = 0; i < 4; i++) {                   // the 4 direction buttons
      if (pressed & (1 << i)) { pushEv(DIR_EV[i], false); downAt[i] = now; lastRep[i] = now; }
    }
    if (pressed & B_OK) { pushEv(EV_OK_DOWN); downAt[4] = now; longFired = false; }
    if ((released & B_OK) && !longFired) pushEv(EV_OK_RELEASE);   // OK released without a long press = short press
  }

  // Auto-repeat while a direction button is held (to scroll lists quickly)
  for (int i = 0; i < 4; i++) {
    if ((stable & (1 << i)) && now - downAt[i] > REPEAT_DELAY && now - lastRep[i] > REPEAT_MS) {
      pushEv(DIR_EV[i], true);
      lastRep[i] = now;
    }
  }

  // Holding OK: after LONG_PRESS_MS send EV_OK_LONG once, then every SEEK_REPEAT_MS again (rep = true)
  if ((stable & B_OK) && now - downAt[4] >= LONG_PRESS_MS) {
    if (!longFired) {                               // first time
      pushEv(EV_OK_LONG, false);
      longFired = true;
      lastRep[4] = now;
    } else if (now - lastRep[4] >= SEEK_REPEAT_MS) { // following times (with a pause in between)
      pushEv(EV_OK_LONG, true);
      lastRep[4] = now;
    }
  }
}

// ====================== SCREEN DRAWING ======================

// Draw horizontally centered text. baseY = baseline coordinate.
void drawCentered(int baseY, const char* s) {
  int w = u8g2.getUTF8Width(s);                // measure the text width
  u8g2.drawUTF8((128 - w) / 2, baseY, s);
}

// Draw text inside a box of width w. If the text is wider than the box and scroll=true, it scrolls sideways (marquee).
void drawMarquee(int x, int baseY, int w, const String& s, bool scroll) {
  int tw = u8g2.getUTF8Width(s.c_str());
  u8g2.setClipWindow(x, 0, x + w, 63);         // only allow drawing inside the box (the excess is cut off)
  if (tw <= w || !scroll) {
    u8g2.drawUTF8(x, baseY, s.c_str());        // fits in the box -> draw normally
  } else {
    marqueeActive = true;                      // tell loop() it needs to keep redrawing
    int total = tw + 24;                       // length of one scroll cycle (text + gap)
    uint32_t t = millis() - marqueeT0;
    int off = 0;
    if (t > 900) off = ((t - 900) / 35) % total;   // stay still for the first 0.9s, then start scrolling
    u8g2.drawUTF8(x - off, baseY, s.c_str());
    u8g2.drawUTF8(x - off + total, baseY, s.c_str());   // draw a second copy so the scroll loops seamlessly
  }
  u8g2.setMaxClipWindow();                     // remove the drawing limit
}

// Draw a generic list (header + 3 visible rows). label(i) returns the text of row i.
// mark = index of the row to mark with a dot (used by the EQ screen).
void drawList(const String& header, int count, int selIdx, const std::function<String(int)>& label, int mark = -1) {
  u8g2.setFont(FONT_SMALL);
  u8g2.drawStr(0, 9, header.c_str());                       // header at the top-left
  if (count > 0) {
    char b[16];
    snprintf(b, sizeof b, "%d/%d", selIdx + 1, count);      // "3/45" at the top-right
    u8g2.drawStr(128 - u8g2.getStrWidth(b), 9, b);
  }
  u8g2.drawHLine(0, 11, 128);                               // horizontal line under the header
  u8g2.setFont(FONT_BIG);
  if (count == 0) { drawCentered(40, "Empty"); return; }    // empty list
  int top = constrain(selIdx - 1, 0, std::max(0, count - 3));   // first visible row (keeps the selection in the middle)
  for (int r = 0; r < 3 && top + r < count; r++) {
    int i = top + r;                                        // index of the item in the list
    int y = 13 + r * 17;                                    // y coordinate of the row
    bool s = (i == selIdx);                                 // is this row selected?
    drawMarquee(6, y + 13, (i == mark) ? 104 : 116, label(i), s);   // only the selected row scrolls
    if (i == mark) u8g2.drawDisc(118, y + 8, 3);            // marker dot
    if (s) u8g2.drawRFrame(1, y, 126, 16, 3);               // rounded frame around the selected row
  }
}

// ---------- Main menu icons (drawn with basic shapes, no bitmaps needed) ----------
// cx, cy = center of the icon. Each icon is about 12 pixels wide.

// Music note icon
void iconMusic(int cx, int cy) {
  u8g2.drawDisc(cx - 2, cy + 4, 3);                    // note head: filled circle
  u8g2.drawBox(cx, cy - 6, 2, 11);                     // note stem: vertical rectangle
  u8g2.drawLine(cx + 2, cy - 6, cx + 6, cy - 3);       // note flag: 2 diagonal lines
  u8g2.drawLine(cx + 2, cy - 5, cx + 6, cy - 2);
}

// Microphone icon (for Podcast)
void iconMic(int cx, int cy) {
  u8g2.drawRBox(cx - 2, cy - 6, 5, 9, 2);              // microphone body: pill shape
  u8g2.drawCircle(cx, cy - 1, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);  // lower arc wrapping around the body
  u8g2.drawVLine(cx, cy + 4, 3);                       // microphone stand
  u8g2.drawHLine(cx - 2, cy + 6, 5);                   // microphone base
}

// Gear icon (for Setting)
void iconGear(int cx, int cy) {
  u8g2.drawDisc(cx, cy, 4);                            // gear body: filled circle
  u8g2.drawBox(cx - 1, cy - 6, 3, 3);                  // 4 teeth: top/bottom/left/right
  u8g2.drawBox(cx - 1, cy + 4, 3, 3);
  u8g2.drawBox(cx - 6, cy - 1, 3, 3);
  u8g2.drawBox(cx + 4, cy - 1, 3, 3);
  u8g2.drawBox(cx - 5, cy - 5, 3, 3);                  // 4 teeth on the diagonals
  u8g2.drawBox(cx + 3, cy - 5, 3, 3);
  u8g2.drawBox(cx - 5, cy + 3, 3, 3);
  u8g2.drawBox(cx + 3, cy + 3, 3, 3);
  u8g2.setDrawColor(0);                                // switch to the "erase" color
  u8g2.drawDisc(cx, cy, 2);                            // cut a round hole in the middle
  u8g2.setDrawColor(1);                                // back to the normal drawing color
}

// Main menu: 3 items, each with an icon to the left of the text; the selected item gets a rounded frame
void drawMain() {
  static const char* items[3] = { "Music", "Podcast", "Setting" };
  u8g2.setFont(FONT_BIG);
  for (int i = 0; i < 3; i++) {
    int y = 2 + i * 21;                                // items are 21 pixels apart
    int cy = y + 9;                                    // vertical center of the item
    if (i == 0) iconMusic(24, cy);                     // icons are placed at x = 24
    else if (i == 1) iconMic(24, cy);
    else iconGear(24, cy);
    u8g2.drawUTF8(42, y + 16, items[i]);               // text sits to the right of the icon (x = 42)
    if (i == sel[SC_MAIN]) u8g2.drawRFrame(8, y, 112, 19, 6);   // rounded frame, with some space around the text
  }
}

// Volume screen: big number + a 20-segment bar
void drawVolume() {
  u8g2.setFont(FONT_SMALL);
  u8g2.drawStr(0, 9, "Volume");
  u8g2.drawHLine(0, 11, 128);
  u8g2.setFont(u8g2_font_logisoso24_tn);               // large number font
  char b[4];
  snprintf(b, sizeof b, "%d", volume);
  u8g2.drawStr((128 - u8g2.getStrWidth(b)) / 2, 44, b);
  for (int i = 0; i < 20; i++) {                       // 20 segments for volume 1..20
    int x = 4 + i * 6;
    if (i < volume) u8g2.drawBox(x, 52, 5, 10);        // filled segment: draw solid
    else u8g2.drawFrame(x, 52, 5, 10);                 // empty segment: outline only
  }
}

// Confirmation screen
void drawConfirm() {
  u8g2.setFont(FONT_SMALL);
  u8g2.drawRFrame(4, 8, 120, 48, 5);
  drawCentered(28, confirmAction == CF_DEL_PL ? "Delete this playlist?" : "Remove this song?");
  drawCentered(46, "OK = Yes   Left = No");
}

// ---------- Icons for the buttons on the player screen ----------
void iconPrev(int cx, int cy)  { u8g2.drawBox(cx - 6, cy - 5, 2, 11); u8g2.drawTriangle(cx + 5, cy - 5, cx + 5, cy + 5, cx - 3, cy); }  // |◀
void iconNext(int cx, int cy)  { u8g2.drawBox(cx + 5, cy - 5, 2, 11); u8g2.drawTriangle(cx - 5, cy - 5, cx - 5, cy + 5, cx + 3, cy); }  // ▶|
void iconPlay(int cx, int cy)  { u8g2.drawTriangle(cx - 4, cy - 6, cx - 4, cy + 6, cx + 6, cy); }                                       // ▶
void iconPause(int cx, int cy) { u8g2.drawBox(cx - 5, cy - 5, 4, 11); u8g2.drawBox(cx + 2, cy - 5, 4, 11); }                            // ||

// Now-playing screen
void drawPlayer() {
  // 1) Track title (scrolls if too long)
  u8g2.setFont(FONT_BIG);
  drawMarquee(0, 13, 128, curTitle, true);

  // 2) Progress bar and time
  uint32_t cur = audio.getAudioCurrentTime();          // seconds played
  uint32_t dur = audio.getAudioFileDuration();         // total seconds
  if (dur > 0) {
    if (lastDur > 0 && dur != lastDur) durEstimated = true;   // total duration keeps changing -> only an estimate (VBR file)
    lastDur = dur;
  }
  u8g2.drawFrame(4, 19, 120, 5);                       // progress bar frame
  if (dur > 0) {
    int w = (int)((uint64_t)cur * 118 / dur);          // width of the played part (ratio cur/dur)
    if (w > 118) w = 118;
    if (w > 0) u8g2.drawBox(5, 20, w, 3);              // fill the played part
  }
  u8g2.setFont(FONT_TINY);
  String t = fmtTime(cur);
  if (dur > 0) t += String(" / ") + (durEstimated ? "~" : "") + fmtTime(dur);   // "~" prefix if the length is an estimate
  drawCentered(31, t.c_str());

  // 3) Row of 3 buttons: previous - play/pause - next
  static const int xs[3] = { 4, 48, 92 };              // x coordinates of the 3 buttons
  for (int i = 0; i < 3; i++) {
    int cx = xs[i] + 16, cy = 42;                      // center of the button
    if (i == 0) iconPrev(cx, cy);
    else if (i == 1) { if (paused) iconPlay(cx, cy); else iconPause(cx, cy); }   // show the play icon while paused
    else iconNext(cx, cy);
    if (playerFocus == i) u8g2.drawRFrame(xs[i], 34, 32, 16, 4);   // frame around the focused button
  }

  // 4) Play mode button
  u8g2.setFont(FONT_SMALL);
  drawCentered(61, shuffleMode ? "Shuffle" : "Sequential");
  if (playerFocus == 3) u8g2.drawRFrame(24, 51, 80, 13, 4);
}

// Draw the small notification (toast) over the middle of the screen
void drawToast() {
  if (!toastText[0]) return;                           // no notification -> nothing to do
  u8g2.setFont(FONT_SMALL);
  int w = u8g2.getStrWidth(toastText) + 16;
  int x = (128 - w) / 2;
  u8g2.setDrawColor(0);
  u8g2.drawRBox(x, 22, w, 20, 4);                      // clear the background behind it
  u8g2.setDrawColor(1);
  u8g2.drawRFrame(x, 22, w, 20, 4);                    // frame
  u8g2.drawStr(x + 8, 36, toastText);                  // text
}

// Draw the whole screen for the current screen
void draw() {
  marqueeActive = false;
  u8g2.clearBuffer();                                  // clear the frame buffer
  switch (screen) {
    case SC_MAIN:   drawMain(); break;
    case SC_MUSIC: {
      static const char* it[2] = { "All songs", "Playlists" };
      drawList("Music", 2, sel[SC_MUSIC], [](int i) { return String(it[i]); });
      break;
    }
    case SC_ALL:
      drawList("All songs", songs.size(), sel[SC_ALL], [](int i) { return baseName(songs[i]); });
      break;
    case SC_PLAYLISTS:
      drawList("Playlists", plIdx.size() + (plIdx.size() < MAX_PLAYLISTS ? 1 : 0), sel[SC_PLAYLISTS], plRowLabel);
      break;
    case SC_PL_SONGS:
      drawList("Playlist " + String(curPl + 1), plTracks.size(), sel[SC_PL_SONGS], [](int i) { return baseName(plTracks[i]); });
      break;
    case SC_PICK_PL:
      drawList("Add to playlist", plIdx.size() + (plIdx.size() < MAX_PLAYLISTS ? 1 : 0), sel[SC_PICK_PL], plRowLabel);
      break;
    case SC_PODCAST:
      drawList("Podcast", podcasts.size(), sel[SC_PODCAST], [](int i) { return baseName(podcasts[i]); });
      break;
    case SC_SETTING: {
      static const char* it[2] = { "Volume", "EQ" };
      drawList("Setting", 2, sel[SC_SETTING], [](int i) { return String(it[i]); });
      break;
    }
    case SC_VOLUME:  drawVolume(); break;
    case SC_EQ:
      drawList("EQ", 3, sel[SC_EQ], [](int i) { return String(EQ_NAMES[i]); }, eqMode);
      break;
    case SC_PLAYER:  drawPlayer(); break;
    case SC_CONFIRM: drawConfirm(); break;
    default: break;
  }
  drawToast();
  u8g2.sendBuffer();                                   // push the buffer out to the OLED
}

// ====================== EVENT HANDLING ======================

// Number of rows in the list of screen s
int listCount(Screen s) {
  switch (s) {
    case SC_MAIN: return 3;
    case SC_MUSIC: return 2;
    case SC_ALL: return songs.size();
    case SC_PLAYLISTS:
    case SC_PICK_PL: return plIdx.size() + (plIdx.size() < MAX_PLAYLISTS ? 1 : 0);   // +1 for the "+ New playlist" row
    case SC_PL_SONGS: return plTracks.size();
    case SC_PODCAST: return podcasts.size();
    case SC_SETTING: return 2;
    case SC_EQ: return 3;
    default: return 0;
  }
}

// Carry out the action after the user presses OK on the confirmation screen
void doConfirm() {
  if (confirmAction == CF_DEL_PL) {                    // delete a playlist
    SD.remove(plPath(confirmArg).c_str());
    refreshPlaylists();
    int c = listCount(SC_PLAYLISTS);
    sel[SC_PLAYLISTS] = constrain(sel[SC_PLAYLISTS], 0, std::max(0, c - 1));   // keep the selection inside the list
    toast("Deleted");
  } else {                                             // remove one track from a playlist
    if (confirmArg >= 0 && confirmArg < (int)plTracks.size()) {
      plTracks.erase(plTracks.begin() + confirmArg);
      savePlaylist(curPl, plTracks);
    }
    sel[SC_PL_SONGS] = constrain(sel[SC_PL_SONGS], 0, std::max(0, (int)plTracks.size() - 1));
    toast("Removed");
  }
  go(confirmBack, false);
}

// Handle button events while on the player screen
void handlePlayer(const Event& e) {
  switch (e.type) {
    case EV_OK_DOWN:
      playerOkHeld = true;                             // remember: OK was pressed down on this screen
      break;

    case EV_OK_RELEASE:                                // short press then release -> activate the focused button
      if (playerOkHeld) {
        playerOkHeld = false;
        switch (playerFocus) {
          case 0: prevTrack(); break;
          case 1: togglePause(); break;
          case 2: nextTrack(); break;
          case 3: toggleShuffle(); break;
        }
      }
      break;

    case EV_OK_LONG:                                   // OK held long enough (repeats while still held)
      if (!playerOkHeld) break;                        // OK did not start on this screen -> ignore
      if (playerFocus == 0) seekBy(-SEEK_STEP_S);      // Previous focused -> rewind 5s
      else if (playerFocus == 2) seekBy(SEEK_STEP_S);  // Next focused     -> fast-forward 5s
      else if (playerFocus == 1 && !e.rep) exitPlayer();   // Play/Pause focused -> exit (first trigger only)
      break;

    case EV_LEFT:  if (playerFocus >= 1 && playerFocus <= 2) { playerFocus--; needRedraw = true; } break;   // move focus left
    case EV_RIGHT: if (playerFocus <= 1) { playerFocus++; needRedraw = true; } break;                       // move focus right
    case EV_UP:    if (playerFocus == 3) { playerFocus = 1; needRedraw = true; } break;                     // from Shuffle up to the button row
    case EV_DOWN:  if (playerFocus <= 2) { playerFocus = 3; needRedraw = true; } break;                     // from the button row down to Shuffle
    default: break;
  }
}

// Handle one button event depending on the current screen
void handleEvent(const Event& e) {
  if (screen == SC_PLAYER) { handlePlayer(e); return; }   // the player screen has its own handling

  int& cur = sel[screen];                              // selected position of this screen (reference)
  int cnt = listCount(screen);                         // number of rows in the list
  bool okPress = (e.type == EV_OK_DOWN);               // OK was just pressed
  bool rightPress = (e.type == EV_RIGHT && !e.rep);    // Right was just pressed (auto-repeat excluded)
  bool leftPress = (e.type == EV_LEFT && !e.rep);      // Left was just pressed (auto-repeat excluded)

  // Up/Down: move the selection in lists (wraps around at the ends)
  if ((e.type == EV_UP || e.type == EV_DOWN) && cnt > 0 && screen != SC_VOLUME && screen != SC_CONFIRM) {
    cur = (e.type == EV_UP) ? (cur - 1 + cnt) % cnt : (cur + 1) % cnt;
    marqueeT0 = millis();                              // scrolling text restarts
    needRedraw = true;
    return;
  }

  switch (screen) {
    case SC_MAIN:                                      // main menu
      if (okPress || rightPress) {
        if (cur == 0) go(SC_MUSIC);
        else if (cur == 1) go(SC_PODCAST);
        else go(SC_SETTING);
      }
      break;

    case SC_MUSIC:                                     // All songs / Playlists
      if (okPress || rightPress) {
        if (cur == 0) go(SC_ALL, false);
        else { refreshPlaylists(); go(SC_PLAYLISTS); }
      } else if (leftPress) go(SC_MAIN, false);
      break;

    case SC_ALL:                                       // all songs
      if (okPress && cnt) startPlayback(&songs, cur, SC_ALL);                    // OK: play
      else if (rightPress && cnt) { pendingPath = songs[cur]; refreshPlaylists(); go(SC_PICK_PL); }  // Right: add to playlist
      else if (leftPress) go(SC_MUSIC, false);                                   // Left: go back
      break;

    case SC_PLAYLISTS:                                 // playlist list
      if (okPress) {
        if (cur < (int)plIdx.size()) {                 // an existing playlist is selected -> open it
          curPl = plIdx[cur];
          loadPlaylist(curPl, plTracks);
          go(SC_PL_SONGS);
        } else {                                       // the "+ New playlist" row is selected -> create one
          int n = createPlaylist();
          if (n < 0) { toast("Cannot create"); break; }
          for (int k = 0; k < (int)plIdx.size(); k++) if (plIdx[k] == n) cur = k;
          toast("Created");
        }
      } else if (rightPress && cur < (int)plIdx.size()) {   // Right: delete the playlist (with confirmation)
        confirmAction = CF_DEL_PL; confirmArg = plIdx[cur]; confirmBack = SC_PLAYLISTS;
        go(SC_CONFIRM);
      } else if (leftPress) go(SC_MUSIC, false);
      break;

    case SC_PL_SONGS:                                  // tracks inside a playlist
      if (okPress && cnt) startPlayback(&plTracks, cur, SC_PL_SONGS);
      else if (rightPress && cnt) { confirmAction = CF_RM_TRACK; confirmArg = cur; confirmBack = SC_PL_SONGS; go(SC_CONFIRM); }
      else if (leftPress) { refreshPlaylists(); go(SC_PLAYLISTS, false); }
      break;

    case SC_PICK_PL:                                   // choose a playlist to add the song to
      if (okPress) {
        int target;
        if (cur < (int)plIdx.size()) target = plIdx[cur];
        else target = createPlaylist();                // "+ New playlist" chosen -> create it and add the song right away
        if (target < 0) { toast("Cannot create"); break; }
        int r = addToPlaylist(target, pendingPath);
        refreshPlaylists();
        go(SC_ALL, false);
        toast(r == 0 ? "Added" : (r == 1 ? "Already in" : "SD error"));
      } else if (leftPress) go(SC_ALL, false);
      break;

    case SC_PODCAST:
      if (okPress && cnt) startPlayback(&podcasts, cur, SC_PODCAST);
      else if (leftPress) go(SC_MAIN, false);
      break;

    case SC_SETTING:                                   // Volume / EQ
      if (okPress || rightPress) {
        if (cur == 0) go(SC_VOLUME);
        else { go(SC_EQ); sel[SC_EQ] = eqMode; }       // open EQ with the selection on the EQ currently in use
      } else if (leftPress) go(SC_MAIN, false);
      break;

    case SC_VOLUME:                                    // volume adjustment
      if (e.type == EV_LEFT || e.type == EV_DOWN) { if (volume > 1) { volume--; applyVolume(); needRedraw = true; } }
      else if (e.type == EV_RIGHT || e.type == EV_UP) { if (volume < 20) { volume++; applyVolume(); needRedraw = true; } }
      else if (okPress) { prefs.putUChar("vol", volume); go(SC_SETTING, false); }   // OK: save and go back
      break;

    case SC_EQ:                                        // EQ selection
      if (okPress) { eqMode = cur; applyEQ(); prefs.putUChar("eq", eqMode); needRedraw = true; toast("EQ set"); }
      else if (leftPress) go(SC_SETTING, false);
      break;

    case SC_CONFIRM:                                   // OK = confirm, Left = cancel
      if (okPress) doConfirm();
      else if (leftPress) go(confirmBack, false);
      break;

    default: break;
  }
}

// ====================== SETUP / LOOP ======================

// Show a greeting / status text at startup
void splash(const char* l1, const char* l2 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);
  drawCentered(28, l1);
  drawCentered(44, l2);
  u8g2.sendBuffer();
}

// setup() runs ONCE at power-on
void setup() {
  Serial.begin(115200);                  // open the Serial port for debugging (if needed)

  pinMode(PIN_UP, INPUT_PULLUP);         // INPUT_PULLUP: the pin is pulled up to 3.3V, pressing the button pulls it to GND
  pinMode(PIN_DOWN, INPUT_PULLUP);
  pinMode(PIN_LEFT, INPUT_PULLUP);
  pinMode(PIN_RIGHT, INPUT_PULLUP);
  pinMode(PIN_OK, INPUT_PULLUP);

  u8g2.begin();                          // start the OLED
  u8g2.setBusClock(400000);              // 400kHz I2C for fast drawing
  splash("MP3 Player", "Starting...");

  prefs.begin("mp3", false);                              // open the settings storage named "mp3"
  volume = constrain(prefs.getUChar("vol", 10), 1, 20);   // load the saved volume (default 10)
  eqMode = constrain(prefs.getUChar("eq", 0), 0, 2);      // load the saved EQ (default EQ1)
  shuffleMode = prefs.getBool("shuf", false);             // load the saved shuffle mode

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);             // start SPI with our own pins
  while (!SD.begin(SD_CS, SPI, 25000000)) {               // try to mount the SD card (25MHz); on failure show a message and retry
    splash("No SD card", "Retrying...");
    SD.end();
    delay(1000);
  }
  SD.mkdir(PLAYLIST_DIR);                // create the folders if they don't exist yet
  SD.mkdir(PODCAST_DIR);

  splash("Scanning SD...");
  scanDir("/", songs, 0, true, true);    // scan the whole card (except /Podcast, /Playlists) for music
  sortList(songs);
  scanDir(PODCAST_DIR, podcasts, 0, false, false);   // scan the Podcast folder
  sortList(podcasts);
  refreshPlaylists();

  audio.setPinout(I2S_BCK, I2S_LRC, I2S_DOUT);       // tell the library which I2S pins to use
  applyVolume();
  applyEQ();

  char buf[24];
  snprintf(buf, sizeof buf, "%u songs", (unsigned)songs.size());
  splash("Ready", buf);
  delay(700);
  go(SC_MAIN);                           // enter the main menu
}

// loop() runs over and over forever
void loop() {
  audio.loop();                          // IMPORTANT: call this as often as possible so the music doesn't stutter
  pollButtons();                         // read the buttons

  Event e;
  while (popEv(e)) handleEvent(e);       // process all pending button events

  autoAdvance();                         // go to the next track when the current one ends

  uint32_t now = millis();
  if (toastText[0] && now > toastUntil) { toastText[0] = 0; needRedraw = true; }   // time's up -> hide the notification
  if (screen == SC_PLAYER) {
    uint32_t t = audio.getAudioCurrentTime();
    if (t != lastShownSec) { lastShownSec = t; needRedraw = true; }               // new second -> redraw the time bar
  }

  static uint32_t lastDraw = 0;
  if (needRedraw || (marqueeActive && now - lastDraw >= 120)) {   // redraw when needed, or while text is scrolling (every 120ms)
    needRedraw = false;
    lastDraw = now;
    draw();
  }
}
