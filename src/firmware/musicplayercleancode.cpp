#include <MFRC522.h>
#include <Adafruit_SSD1306.h>
#include <IRremote.h>
#include "DFPlayer.h"

SYSTEM_THREAD(ENABLED);

// --- Pin Definitions ---
#define PIN_IR_RECV     D3
#define PIN_DF_BUSY     D4   //Busy Pin of the DFPlayer
#define OLED_RESET      -1   
#define SS_PIN          A2   // SDA pin on RC522
#define RST_PIN         D2   // Reset pin on RC522
#define BUZZER          A7

const unsigned int SUCCESS_FREQ = 1500; // High tone for successful authentication
const unsigned int FAIL_FREQ    = 350;  // Low tone for failed authentication
const unsigned long BEEP_DURATION = 75; // Duration of each tone 

byte authorizedUID[] = { }; 
const int UID_SIZE = sizeof(authorizedUID);

// --- System Constants ---
const int VOL_MAX             = 30;
const int VOL_DEFAULT         = 10;
const int TOTAL_TRACKS        = 8;
const int TRACKS_PER_PAGE     = 4;
const int MAX_TITLE_LEN_LIST  = 18;
const int MAX_TITLE_LEN_PLAY  = 20;
const int MAX_REC_LINES       = 5;

// --- IR Remote Codes---
namespace IR {
    const char* CH_PREV     = "ffa25d";
    const char* CH          = "ff629d";
    const char* CH_NEXT     = "ffe21d";
    const char* PREV        = "ff22dd";
    const char* NEXT        = "ff02fd";
    const char* PLAY_PAUSE  = "ffc23d";
    const char* VOL_DOWN    = "ffe01f";
    const char* VOL_UP      = "ffa857";
    const char* EQ          = "ff906f"; // Triggers Recommendations
    
    // Direct Track Selection
    const char* TRACK_1     = "ff30cf";
    const char* TRACK_2     = "ff18e7";
    const char* TRACK_3     = "ff7a85";
    const char* TRACK_4     = "ff10ef";
    const char* TRACK_5     = "ff38c7";
    const char* TRACK_6     = "ff5aa5";
    const char* TRACK_7     = "ff42bd";
    const char* TRACK_8     = "ff4ab5";
    
    const char* LOGOUT      = "ff6897";
}

// --- Song Data ---
const char* SONG_TITLES[] = {
    "", // Placeholder for 0-index
    "Glorious - Macklemore",          // 1
    "LOST IN THE ECHO - Linkin Park", // 2
    "Am I Dreaming",                  // 3
    "Tokyo Ghoul OST",                // 4
    "Feather - Nujabes",              // 5
    "Holiday - Green Day",            // 6
    "Number One Bankai",              // 7
    "Don't Even Try It - Funky DL"    // 8
};

// Display Modes
enum ScreenMode {
    MODE_LOCKED = -1,
    MODE_SELECT_TRACK = 0,
    MODE_NOW_PLAYING  = 1,
    MODE_RECOMMEND    = 2,
    MODE_LOADING      = 3
};

struct PlayerState {
    int currentTrack = 1;
    int volume = VOL_DEFAULT;
    bool isPlaying = false;
    int lastBusyPinState = HIGH;
};

struct UIState {
    ScreenMode currentMode = MODE_LOCKED;
    int currentPage = 1;
    bool isLoading = false;
    
    // Scroll timers and state
    unsigned long lastScrollTime = 0;
    const long scrollInterval = 300;
    
    // Playback Screen Scroll
    int titleScrollOffset = 0;
    int titleScrollDir = 1;

    // Selection & Rec Screen Scroll
    int listScrollOffsets[9] = {0}; 
    int listScrollDirs[9] = {1};

    // Recs Data
    String recStringData = "";
};

// --- Global Objects ---
Adafruit_SSD1306 display(OLED_RESET);
IRrecv irrecv(PIN_IR_RECV);
decode_results results;
DFPlayer dfPlayer;
MFRC522 mfrc522(SS_PIN, RST_PIN);

PlayerState player;
UIState ui;

// --- Function Prototypes for RFID ---
bool isAuthorized();
void drawLockedScreen();
void handleRFID();

void setup() {
    // Init Communications
    Serial.begin(115200);
    Serial1.begin(9600);
    
    // Init Hardware
    irrecv.enableIRIn();
    pinMode(PIN_DF_BUSY, INPUT);
    pinMode(BUZZER, OUTPUT);
    
    // Init RFID
    SPI.begin();
    mfrc522.PCD_Init();
    Serial.println("RFID Ready.");

    // Init Display
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.display();
    delay(2000);
    display.clearDisplay();
    display.display();

    // Init Audio
    dfPlayer.setLogging(true);
    dfPlayer.setPlaybackSource(PS_TFCARD);
    dfPlayer.setVolume(player.volume);
    delay(50);
    dfPlayer.setVolume(player.volume);
    
    // Init Cloud Variables
    registerCloudFunctions();

    // Initial Screen
    drawLockedScreen();
}

void loop() {
    if(ui.currentMode == MODE_LOCKED) {
        handleRFID();
    } else {
        handleRemote();
        checkTrackStatus();
    }
    updateScreen();
}

void handleRFID() {
    // Look for new cards and select one
    if ( ! mfrc522.PICC_IsNewCardPresent() || ! mfrc522.PICC_ReadCardSerial()) {
        return; // No new card or failed read
    }

    if (isAuthorized()) {
        Serial.println("ACCESS GRANTED! Player unlocked.");
        playSuccessTone();
        ui.currentMode = MODE_SELECT_TRACK; // Unlocks the player
    } else {
        Serial.println("ACCESS DENIED! Wrong card.");
        playFailTone();
    }

    // Halt PICC and stop crypto
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
}

bool isAuthorized() {
    // Check if the UID size is correct
    if (mfrc522.uid.size != UID_SIZE) {
        return false;
    }
    
    // Compare each byte
    for (byte i = 0; i < UID_SIZE; i++) {
        if (mfrc522.uid.uidByte[i] != authorizedUID[i]) {
            return false;
        }
    }
    return true;
}

void handleRemote() {
    
    // If player is locked, ignore remote signal
    if (ui.currentMode == MODE_LOCKED) {
        irrecv.resume(); // Clear the remote signal but do nothing
        return;
    }
    
    if (irrecv.decode(&results)) {
        String hexIr = String(results.value, HEX);
        irrecv.resume();
        
        // CH takes you back to the selection screen
        if (hexIr == IR::CH) {
            ui.currentMode = MODE_SELECT_TRACK;
            drawTrackSelection();
            return;
        }
        
        // If in Rec mode and we press something else, leave Rec mode first.
        if (ui.currentMode == MODE_RECOMMEND) {
            // If music is playing, default back to Now Playing view
            if (player.isPlaying) {
                ui.currentMode = MODE_NOW_PLAYING;
            } 
            // If stopped, default to Selection view.
            else {
                ui.currentMode = MODE_SELECT_TRACK;
            }
        }
        
        // Navigating pages
        if (ui.currentMode == MODE_SELECT_TRACK || !player.isPlaying) {
            if (hexIr == IR::CH_NEXT) changePage(1);
            if (hexIr == IR::CH_PREV) changePage(-1);
        }

        // Playback Controls
        if (hexIr == IR::NEXT)        cloudNext("");
        if (hexIr == IR::PREV)        cloudPrevious("");
        if (hexIr == IR::PLAY_PAUSE)  cloudPause("");
        if (hexIr == IR::VOL_UP)      cloudIncreaseVolume("");
        if (hexIr == IR::VOL_DOWN)    cloudDecreaseVolume("");
        
        // Direct Track Access
        if (hexIr == IR::TRACK_1) playSong("1");
        if (hexIr == IR::TRACK_2) playSong("2");
        if (hexIr == IR::TRACK_3) playSong("3");
        if (hexIr == IR::TRACK_4) playSong("4");
        if (hexIr == IR::TRACK_5) playSong("5");
        if (hexIr == IR::TRACK_6) playSong("6");
        if (hexIr == IR::TRACK_7) playSong("7");
        if (hexIr == IR::TRACK_8) playSong("8");

        // Special Modes
        if (hexIr == IR::EQ) {
            ui.currentMode = MODE_LOADING;
            ui.isLoading = true;
            drawLoadingScreen();
            Particle.publish("Recommendations");
        }
        if (hexIr == IR::LOGOUT) cloudLogout("");
    }
}

void checkTrackStatus() {
    int busyState = digitalRead(PIN_DF_BUSY);

    // If track was playing and BUSY pin went HIGH, track finished
    if (player.isPlaying && player.lastBusyPinState == LOW && busyState == HIGH) {
        cloudNext(""); 
    }
    player.lastBusyPinState = busyState;
}

void changePage(int direction) {
    ui.currentPage += direction;
    int maxPages = TOTAL_TRACKS / TRACKS_PER_PAGE;
    
    if (ui.currentPage > maxPages) ui.currentPage = 1;
    if (ui.currentPage < 1) ui.currentPage = maxPages;
    
    drawTrackSelection();
}

void updateScreen() {
    // Only animate if enough time passed
    if (millis() - ui.lastScrollTime < ui.scrollInterval) return;
    ui.lastScrollTime = millis();

    switch (ui.currentMode) {
        case MODE_LOCKED:
            break;
        case MODE_RECOMMEND:
            animateRecommendations();
            break;
        case MODE_LOADING:
            if (ui.isLoading) drawLoadingScreen();
            break;
        case MODE_NOW_PLAYING:
            animateNowPlaying();
            break;
        case MODE_SELECT_TRACK:
            animateSelectionList();
            break;
    }
}

// Helper: Logic for Ping-Pong scrolling
void calculateScroll(int &offset, int &direction, int maxOffset) {
    if (maxOffset <= 0) {
        offset = 0; // Reset if fits
        return;
    }
    offset += direction;
    if (offset >= maxOffset) {
        direction = -1;
        offset = maxOffset;
    } else if (offset <= 0) {
        direction = 1;
        offset = 0;
    }
}

void animateNowPlaying() {
    String title = SONG_TITLES[player.currentTrack];
    int maxOffset = title.length() - MAX_TITLE_LEN_PLAY;
    
    // Calculate new position
    calculateScroll(ui.titleScrollOffset, ui.titleScrollDir, maxOffset);
    
    // Redraw
    drawNowPlaying();
}

void animateSelectionList() {
    bool needsUpdate = false;
    
    // Calculate offsets for all tracks
    for (int i = 1; i <= TOTAL_TRACKS; i++) {
        String title = SONG_TITLES[i];
        int maxOffset = title.length() - MAX_TITLE_LEN_LIST;
        
        int oldOffset = ui.listScrollOffsets[i];
        calculateScroll(ui.listScrollOffsets[i], ui.listScrollDirs[i], maxOffset);
        
        if (ui.listScrollOffsets[i] != oldOffset) needsUpdate = true;
    }

    if (needsUpdate) drawTrackSelection();
}

void animateRecommendations() {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.setTextSize(2);
    display.print("RECOMMENDS");
    
    String recs = ui.recStringData;
    int fromIndex = 0;
    int yPos = 18;
    display.setTextSize(1);

    for (int i = 0; i < MAX_REC_LINES; i++) {
        int delimiter = recs.indexOf('|', fromIndex);
        String line = (delimiter != -1) ? recs.substring(fromIndex, delimiter) : recs.substring(fromIndex);
        fromIndex = (delimiter != -1) ? delimiter + 1 : recs.length();
        line.trim();

        if (line.length() == 0 && delimiter == -1) break;

        // Header Line (Line 0)
        if (i == 0) {
            display.setCursor(0, 0);
            display.print(line); 
        } else {
            // Scroll Logic for list items
            int maxOffset = line.length() - MAX_TITLE_LEN_PLAY;
            calculateScroll(ui.listScrollOffsets[i], ui.listScrollDirs[i], maxOffset);

            int start = ui.listScrollOffsets[i];
            int end = min((int)line.length(), start + MAX_TITLE_LEN_PLAY);
            
            display.setCursor(0, yPos);
            display.print(line.substring(start, end));
            yPos += 12;
        }
        if (delimiter == -1) break;
    }
    display.display();
}

void drawLockedScreen() {
    display.clearDisplay();
    display.setTextColor(WHITE);
    
    display.setTextSize(2);
    display.setCursor(0, 1);
    display.println("LOCKED");

    display.setTextSize(1);
    display.setCursor(0, 40);
    display.println("Scan Authorized Card");
    
    display.display();
}

void drawLoadingScreen() {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(2);
    display.setCursor(0, 24);
    display.print("LOADING...");
    display.display();
}

void drawTrackSelection() {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    
    display.printf("SELECT TRACK (%d/%d):", ui.currentPage, TOTAL_TRACKS / TRACKS_PER_PAGE);

    int startTrack = (ui.currentPage - 1) * TRACKS_PER_PAGE + 1;
    int endTrack = min(startTrack + TRACKS_PER_PAGE - 1, TOTAL_TRACKS);
    int yPos = 18;

    for (int i = startTrack; i <= endTrack; i++) {
        display.setCursor(0, yPos);
        display.print(i); 
        display.print(". ");
        
        String title = SONG_TITLES[i];
        int start = ui.listScrollOffsets[i];
        int end = min((int)title.length(), start + MAX_TITLE_LEN_LIST);
        
        display.print(title.substring(start, end));
        yPos += 12;
    }
    ui.currentMode = MODE_SELECT_TRACK;
    display.display();
}

void drawNowPlaying() {
    display.clearDisplay();
    display.setTextColor(WHITE);
    
    // Status
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.println(player.isPlaying ? "PLAYING" : "PAUSED");

    // Track Info
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.printf("Track %d: ", player.currentTrack);
    
    display.setCursor(0, 30);
    String title = SONG_TITLES[player.currentTrack];
    int start = ui.titleScrollOffset;
    int end = min((int)title.length(), start + MAX_TITLE_LEN_PLAY);
    display.print(title.substring(start, end));

    // Volume
    display.setCursor(0, 40);
    display.printf("Volume: %d / %d", player.volume, VOL_MAX);

    // Volume Bar
    int barWidth = map(player.volume, 0, VOL_MAX, 0, display.width() - 2);
    display.drawRect(0, 50, display.width(), 10, WHITE);
    display.fillRect(1, 51, barWidth, 8, WHITE);

    ui.currentMode = MODE_NOW_PLAYING;
    display.display();
}

void resetScrollState() {
    ui.titleScrollOffset = 0;
    ui.titleScrollDir = 1;
    // Reset list scrolls
    for(int i=0; i<9; i++) {
        ui.listScrollOffsets[i] = 0;
        ui.listScrollDirs[i] = 1;
    }
}

void registerCloudFunctions() {
    Particle.function("pause", cloudPause);
    Particle.function("playNext", cloudNext);
    Particle.function("playPrevious", cloudPrevious);
    Particle.function("setVolume", cloudSetVolume);
    Particle.function("playTrack", playSong);
    Particle.function("increaseVol", cloudIncreaseVolume);
    Particle.function("decreaseVol", cloudDecreaseVolume);
    Particle.function("getVolume", cloudGetVolume);
    Particle.function("displayRecs", displayRecs);
    Particle.function("logout", cloudLogout); 
}

int displayRecs(String recString) {
    ui.recStringData = recString;
    ui.isLoading = false;
    ui.currentMode = MODE_RECOMMEND;
    resetScrollState();
    animateRecommendations();
    return 1;
}

int cloudPause(String command) {
    if (player.isPlaying) {
        dfPlayer.playTrack(255); // Stop
        Particle.publish("StopTrack", String(255));
        player.isPlaying = false;
    } else {
        dfPlayer.playTrack(player.currentTrack);
        Particle.publish("PlayTrack", String(player.currentTrack));
        player.isPlaying = true;
    }
    drawNowPlaying();
    player.lastBusyPinState = HIGH;
    return player.isPlaying;
}

int cloudNext(String command) {
    player.currentTrack++;
    if (player.currentTrack > TOTAL_TRACKS) player.currentTrack = 1;
    return playTrackInternal();
}

int cloudPrevious(String command) {
    player.currentTrack--;
    if (player.currentTrack < 1) player.currentTrack = TOTAL_TRACKS;
    return playTrackInternal();
}

int playSong(String num) {
    player.currentTrack = atoi(num);
    return playTrackInternal();
}

// Internal helper to reduce duplication in Next/Prev/Play
int playTrackInternal() {
    dfPlayer.playTrack(player.currentTrack);
    player.isPlaying = true;
    player.lastBusyPinState = HIGH;
    resetScrollState();
    drawNowPlaying();
    Particle.publish("PlayTrack", String(player.currentTrack));
    return player.currentTrack;
}

int cloudSetVolume(String volumeStr) {
    int v = atoi(volumeStr);
    player.volume = constrain(v, 0, VOL_MAX);
    dfPlayer.setVolume(player.volume);
    drawNowPlaying();
    return player.volume;
}

int cloudIncreaseVolume(String command) {
    return cloudSetVolume(String(player.volume + 1));
}

int cloudDecreaseVolume(String command) {
    return cloudSetVolume(String(player.volume - 1));
}

int cloudGetVolume(String command) {
    return player.volume;
}

void playSuccessTone() {
    // Two quick high beeps
    tone(BUZZER, SUCCESS_FREQ, BEEP_DURATION);
    delay(BEEP_DURATION);
    noTone(BUZZER);
    delay(50);
    tone(BUZZER, SUCCESS_FREQ, BEEP_DURATION);
    delay(BEEP_DURATION);
    noTone(BUZZER);
}

void playFailTone() {
    // One low, distinct tone
    tone(BUZZER, FAIL_FREQ, BEEP_DURATION * 3);
    delay(500);
    noTone(BUZZER);
}

int cloudLogout(String command) {
    // 1. Stop the music playback
    if (player.isPlaying) {
        dfPlayer.playTrack(255); // Stop command
        player.isPlaying = false;
        Particle.publish("StopTrack", String(255));
    }
    
    // 2. Reset the system state
    player.currentTrack = 1; 
    ui.currentPage = 1;
    resetScrollState();
    
    // 3. Lock the system
    ui.currentMode = MODE_LOCKED;
    drawLockedScreen();
    
    Serial.println("System logged out. Player locked.");
    
    // Return 1 for success
    return 1;
}