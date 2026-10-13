ESP32-S3 MP3 Player

A standalone, button-controlled MP3 player built around the ESP32-S3 Super Mini, a PCM5102A I2S DAC, a microSD card and a 0.96" OLED display. It plays music and podcasts straight from an SD card (including files inside folders), supports playlists, an equalizer and seeking, and remembers your settings between power cycles.

Features
High-quality audio: decodes MP3, FLAC, WAV, AAC/M4A, OGG and Opus and streams it digitally (I2S) to a PCM5102A DAC.
Reads folders: music is found automatically in any subfolder of the SD card (up to 6 levels deep).
3-item main menu with icons:
Music → All songs and Playlists
Podcast → everything inside the /Podcast folder (podcasts, audiobooks, BBC-style episodes...)
Setting → Volume (1–20, default 10) and EQ
Playlists: up to 9, created and edited on the device (add a song from All songs, remove it from inside a playlist, delete a whole playlist).
3 EQ presets: EQ1 flat (default), EQ2 bass boost, EQ3 vocal/instrument boost.
Now-playing screen: scrolling track title, progress bar with elapsed/total time, previous / play-pause / next buttons, and a Sequential / Shuffle toggle.
Seeking: hold OK on Previous or Next to rewind / fast-forward in 5-second steps.
Only 5 buttons needed (Up, Down, Left, Right, OK).
Persistent settings: volume, EQ and shuffle mode are saved in flash.
Vietnamese (and other Latin) diacritics are supported in track names via the Unifont font.
Hardware
#	Part	Notes
1	ESP32-S3 Super Mini	Any ESP32-S3 board with enough free GPIOs works, but the pin map below is written for the Super Mini
2	PCM5102A I2S DAC module	Gives line-level stereo output on a 3.5 mm jack
3	microSD card module (SPI)	Plus a microSD card formatted as FAT32
4	0.96" OLED, 128×64, SSD1306, I2C	I2C address 0x3C
5	5 × tactile push buttons	Up, Down, Left (previous), Right (next), OK
6	Breadboard + jumper wires	
7	Headphones or an amplifier	Connect to the PCM5102A 3.5 mm output
8	(optional) 1-cell Li-Po battery	The Super Mini has battery pads (B+ / B−). Check your board's documentation before relying on its charging circuit
Wiring

All GPIO numbers below are ESP32-S3 GPIO numbers.

PCM5102A DAC (I2S)
PCM5102A	ESP32-S3
BCK	GPIO 5
LCK (LRCK)	GPIO 6
DIN	GPIO 7
VIN	3V3 (or 5V, depending on your module)
GND	GND
SCK	GND
XSMT	3V3 (un-mutes the output)
FMT	GND (standard I2S format)

Many PCM5102A boards have solder jumpers (labelled H1L–H4L or similar) on the back instead of separate pins. Set them so that SCK is low, XSMT is high and FMT is low. If you hear nothing, XSMT is the first thing to check.

microSD card module (SPI)
SD module	ESP32-S3
SCK	GPIO 12
MOSI	GPIO 11
MISO	GPIO 13
CS	GPIO 10
VCC	3V3 (or 5V if your module has an on-board regulator + level shifter)
GND	GND
OLED display (I2C)
OLED	ESP32-S3
SCL	GPIO 8
SDA	GPIO 9
VCC	3V3
GND	GND
Buttons

Each button connects one leg to the GPIO and the other leg to GND. No external resistors are needed because the firmware enables the internal pull-ups.

Button	ESP32-S3
Up	GPIO 1
Down	GPIO 2
Left (previous)	GPIO 3
Right (next)	GPIO 4
OK	GPIO 14

Notes:

GPIO 14 is on the small solder pads on the underside of the Super Mini, so you need to solder a wire to that pad.
GPIO 3 is a strapping pin. It works fine as a button, just don't hold it while powering the board on.
If you prefer GPIO 35 for OK, change PIN_OK in the sketch. That only works when PSRAM is set to QSPI or Disabled (Octal PSRAM uses GPIO 33–37).
Pin summary
GPIO	Function	GPIO	Function
1	Button Up	8	OLED SCL
2	Button Down	9	OLED SDA
3	Button Left	10	SD CS
4	Button Right	11	SD MOSI
5	I2S BCK	12	SD SCK
6	I2S LRCK	13	SD MISO
7	I2S DIN	14	Button OK
SD card layout

Format the card as FAT32. Copy your music anywhere on the card; subfolders are fine.

/
├── Music/              <- any folders/names you like, all scanned for "All songs"
│   ├── Album A/
│   │   └── song1.mp3
│   └── song2.flac
├── Podcast/            <- everything here shows up in the Podcast menu
│   └── episode1.mp3
└── Playlists/          <- created automatically (pl1.txt ... pl9.txt)
/Podcast and /Playlists are created automatically on first boot if they don't exist.
Supported extensions: .mp3 .wav .flac .aac .m4a .ogg .opus
Each list is limited to 1500 tracks (MAX_TRACKS) to avoid running out of RAM.
Software setup
1. Arduino IDE
Install Arduino IDE 2.x and the esp32 by Espressif board package (the project was built with core 3.3.x).
2. Libraries (Library Manager / GitHub)
Library	Author	Purpose
U8g2	olikraus	OLED text and graphics
ESP32-audioI2S	schreibfaul1	Audio decoding and I2S output

The sketch uses connecttoFS, setTone, setTimeOffset, getAudioCurrentTime and getAudioFileDuration. Function names in ESP32-audioI2S change between releases, so if the sketch fails to compile, check these calls against the Audio.h of the version you installed.

3. Board settings (Tools menu)
Setting	Value
Board	ESP32S3 Dev Module
USB CDC On Boot	Enabled
Flash Size	4MB (32Mb)
Partition Scheme	Huge APP (3MB No OTA/1MB SPIFFS)
PSRAM	QSPI PSRAM if your chip has PSRAM (e.g. ESP32-S3FH4R2), otherwise Disabled

The default partition scheme is too small: the audio library makes the sketch about 1.6 MB, so you will get "text section exceeds available space in board" unless you choose Huge APP. This disables OTA updates, which this project does not use.

4. Upload
Open MP3Player_ESP32S3_EN/MP3Player_ESP32S3_EN.ino (the folder name must match the .ino file name).
Select the board and COM port, then click Upload.
If the board does not enter upload mode, hold BOOT, tap RESET, then release BOOT.
Controls
In menus and lists
Button	Action
Up / Down	Move the selection (wraps around; hold to scroll fast)
OK or Right	Enter / play
Left	Go back
Extra actions in lists
Where	Button	Action
All songs	Right	Add the selected song to a playlist (pick one, or choose + New playlist)
Playlists	OK on + New playlist	Create a playlist
Playlists	Right	Delete the selected playlist (asks for confirmation)
Inside a playlist	Right	Remove the selected song (asks for confirmation)
Volume	Left / Right	Volume − / + (OK saves)
EQ	OK	Select the preset (the dot marks the active one)
On the now-playing screen
Button	Action
Left / Right	Move the focus between Previous, Play/Pause and Next
Up / Down	Move the focus between the button row and the Sequential/Shuffle button
OK (short press)	Activate the focused button
OK (hold 3 s) on Previous	Rewind 5 s; keep holding to keep rewinding (every 1 s)
OK (hold 3 s) on Next	Fast-forward 5 s; keep holding to keep going
OK (hold 3 s) on Play/Pause	Stop and return to the list
Configuration

Most behaviour can be tuned with the #defines at the top of the sketch:

Name	Default	Meaning
LONG_PRESS_MS	3000	How long to hold OK before seek/exit triggers
SEEK_STEP_S	5	Seconds per seek step
SEEK_REPEAT_MS	1000	Pause between seeks while OK is held
DEBOUNCE_MS	20	Button debounce time
MAX_PLAYLISTS	9	Number of playlists
MAX_TRACKS	1500	Maximum tracks per list
EQ_TABLE	see code	{bass, mid, treble} in dB (−40…+6) for each EQ preset
Troubleshooting
Symptom	Likely cause / fix
text section exceeds available space in board	Set Partition Scheme to Huge APP
Compile error about setTimeOffset / setTone / ...	Your ESP32-audioI2S version uses different function names; check Audio.h
"No SD card" on screen	Check the wiring, make sure the card is FAT32 and the module is powered correctly
No sound	Check BCK/LRCK/DIN wiring and the PCM5102A XSMT (should be high), SCK (low) and FMT (low) jumpers
A song's total time keeps increasing, shown as ~03:35	That file is VBR MP3 without a length header, so the length is only an estimate. Re-encode it as CBR (e.g. with Audacity) to fix it
Track names with accents look wrong	FAT file-name encoding; try renaming the file without accents
Board reboots in a loop after changing board settings	You enabled PSRAM on a board that has none. Set PSRAM to Disabled
OLED stays blank	Check SDA/SCL and that the display address is 0x3C
Limitations
Leaving the now-playing screen stops the music.
Seeking in VBR files can be a few seconds off, because the position is estimated from the average bitrate.
Partition scheme Huge APP means no over-the-air updates.

Credits
ESP32-audioI2S by schreibfaul1
U8g2 by olikraus
