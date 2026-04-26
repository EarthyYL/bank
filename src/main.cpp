#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <RTClib.h>
#include <Stepper.h>

// EEPROM addresses
#define BALANCE_ADDR 0 
#define TIME_ADDR    4 
#define SHIFT_COUNT_ADDR 8 
#define TIMESTAMP_ADDR 10
#define SHIFTS_START_ADDR 14
// CURRENT EEPROM ALLOCATION:
// 0-3: balance (long, 4 bytes)
// 4-7: timeBalance (long, 4 bytes)
// 8-9: shiftCount (int, 2 bytes)
// 10-13: last seen timestamp (unsigned long, 4 bytes)
// 14-613: shifts (struct (3 longs), 12 bytes, 50 entries allocated)
// 614-1023: unused (410 bytes free) (test marker stored at 1000/1001 for debugging)

#define MAX_SHIFTS 50
#define DEBOUNCE_MS 50
#define STEPS_PER_REV 2048 // 0.176 degrees per step for 28BYJ-48 stepper motor
#define STEP_INTERVAL_US 1757813UL // step inteval in microseconds for 1 hour per revolution

// create objects to represent LCD, RTC, stepper modules, with appropriate parameters and I2C addresses
LiquidCrystal_I2C lcd(0x27, 16, 2); 
RTC_DS3231 rtc;
Stepper motor(STEPS_PER_REV, 2, A0, 3, A1); // pins for ULN2003

// custom struct for shift data to be stored in EEPROM
struct Shift {
  long balance;
  long time;
  unsigned long timestamp;
};

int button1 = 6;
int button2 = 7;
int button3 = 8;
int button4 = 9;
int button5 = 10;
int button6 = 11;
int button7 = 12;
int buttonSubToggle = 4;
int buttonTimeToggle = 5;
int subtractLED = A2;
int timeLED = A3;

// PINOUT:
// D0/D1: Serial TX/RX pins 
// D2/D3: Stepper motor control pins
// D4: Subtract mode toggle button
// D5: Time mode toggle button
// D6-D12: Increment buttons
// D13: Do not use - connected to onboard LED, may cause issues if used as button pin
// A0/A1: Stepper motor control pins
// A2: Subtract mode LED
// A3: Time mode LED
// A4/A5: RTC SDA/SCL pins, LCD SDA/SCL pins (shared I2C bus)


// global declarations for counters, mode flags, and time tracking
bool lastState1 = HIGH;
bool lastState2 = HIGH;
bool lastState3 = HIGH;
bool lastState4 = HIGH;
bool lastState5 = HIGH;
bool lastState6 = HIGH;
bool lastState7 = HIGH;
bool lastStateSubToggle = HIGH;
bool lastStateTimeToggle = HIGH;
unsigned long lastSeenTime = 0;
unsigned long lastStepTime = 0;
bool useTime = false;       
bool useSubtract = false;
int shiftCount = 0;
long balance = 0;
long timeBalance = 0;

// forward function declarations

bool onPress(int pin, bool &last);
unsigned long timeDiff();
void waitRelease(int pin, bool &last);
void increment(long &target, int value);
void toggle(bool &flag);
void saveState();
void loadState();
void defaultDisplay();
bool confirmDialog(const char *message);
void saveShift();
void deleteShift(int index);
void shiftReviewMode();
void debugEEPROM();
void loadTestShifts();

void setup() {
  pinMode(button1, INPUT_PULLUP); // pull-up resistor logic - HIGH when not pressed, LOW when pressed
  pinMode(button2, INPUT_PULLUP);
  pinMode(button3, INPUT_PULLUP);
  pinMode(button4, INPUT_PULLUP);
  pinMode(button5, INPUT_PULLUP);
  pinMode(button6, INPUT_PULLUP);
  pinMode(button7, INPUT_PULLUP);
  pinMode(buttonSubToggle, INPUT_PULLUP);
  pinMode(buttonTimeToggle, INPUT_PULLUP);
  pinMode(subtractLED, OUTPUT);
  pinMode(timeLED, OUTPUT);

  Serial.begin(9600);

  // initalize RTC
  rtc.begin();

  // COMMENT THIS OUT TO AVOID RESETTING TIME EVERY TIME YOU UPLOAD NEW CODE
  rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); // set RTC to compile time - only needs to be done once.

  loadState(); // grab values of stored vars before we begin - next two initalizations use them
  loadTestShifts(); // load test shifts for debugging - comment out in prod

  // initalize LCD display
  lcd.init();
  lcd.backlight();
  defaultDisplay(); // initial display update to show loaded values

  // initalize stepper motor
  motor.setSpeed(1); // low speed, doesn't matter since we're stepping manually
  unsigned long offSeconds = timeDiff(); // grab time since device was last on
  unsigned long stepCatchup = (offSeconds * STEPS_PER_REV) / 3600UL; // calculate how many steps we need to catch up
  motor.step(stepCatchup); // catch up on steps missed while off

  debugEEPROM(); // print EEPROM values to serial for debugging - comment out in production
}

void loop() {
  // Default display and counter update block - most button actions are handled here

  // determine target counter based on mode.
  // declare as an alias reference to point back to actual counters
  long &target = useTime ? timeBalance : balance; 

  // loop follows this logic for each button:
  // 1. check for press event (edge detection)
  // 2. call action functions
  // 3. wait for release and handle debouncing
 
  if (onPress(button1, lastState1)) {
    increment(target, useTime ? 15 : 1); // increment by 15 minutes or $0.01 depending on mode
    waitRelease(button1, lastState1);
  }
  if (onPress(button2, lastState2)) {
    increment(target, useTime ? 60 : 5); // 1 hr or $0.05
    waitRelease(button2, lastState2);
  }
  if (onPress(button3, lastState3)) {
    increment(target, useTime ? 300 : 25); // 5 hr or $0.25
    waitRelease(button3, lastState3);
  }

  // this block only executes for money mode. the buttons have special functions in time mode that are handled in the else case
  if (!useTime) {
    if (onPress(button4, lastState4)) {
      increment(balance, 100); // $1.00
      waitRelease(button4, lastState4);
    }
    if (onPress(button5, lastState5)) {
      increment(balance, 500); // $5.00
      waitRelease(button5, lastState5);
    }
    if (onPress(button6, lastState6)) {
      increment(balance, 2000); // $20.00
      waitRelease(button6, lastState6);
    }
    if (onPress(button7, lastState7)) {
      increment(balance, 10000); // $100.00
      waitRelease(button7, lastState7);
    }

  }else {
    // time mode special functions for buttons 4-7
    if (onPress(button4, lastState4)) { 
      saveShift(); // complicated function, described in its own definition below
      waitRelease(button4, lastState4);
    }
    if (onPress(button5, lastState5)) {
      waitRelease(button5, lastState5); 
      shiftReviewMode(); // also complicated, described in its own definition below
    }
    if (onPress(button6, lastState6)) { 
      waitRelease(button6, lastState6);
      // calculate and display hourly wage based on current balance and time, with appropriate formatting
      lcd.clear();
      if (timeBalance == 0) {
        lcd.print("No time worked");
        delay(3000);
        defaultDisplay();
      } 
      else {
        long wage = (balance * 60) / timeBalance; // cents per hour, timeBalance in minutes
        char line1[17];
        snprintf(line1,17, "Wage: $%ld.%02ld/hr", wage / 100, wage % 100);
        lcd.print(line1);
        delay(3000);
        defaultDisplay();
      }
    }
    // ask user to reset their shift (new shift without saving to memory)
    if (onPress(button7, lastState7)) { 
      waitRelease(button7, lastState7);
      // confirmation block
      if (confirmDialog("Reset shift?")) {
        balance = 0;
        timeBalance = 0;
        saveState(); // save reset state to EEPROM
        lcd.clear();
        lcd.print("Shift reset!");
        delay(2500);
        defaultDisplay();
      }
      // cancellation block
      else {
        // cancellation printout
        lcd.clear();
        lcd.print("Cancelled");
        delay(2500);
        defaultDisplay();
      }
    }
  }
  if (onPress(buttonSubToggle, lastStateSubToggle)) { // toggle subtract mode
    toggle(useSubtract);
    waitRelease(buttonSubToggle, lastStateSubToggle);
  }
  if (onPress(buttonTimeToggle, lastStateTimeToggle)) { // toggle time mode
    toggle(useTime);
    waitRelease(buttonTimeToggle, lastStateTimeToggle);
  }


  // STEPPER MOTOR CONTROL BLOCK
  // one rotation per hour, one step per 1.757 seconds
  if (micros() - lastStepTime >= STEP_INTERVAL_US) {
  motor.step(1);
  lastStepTime += STEP_INTERVAL_US; // increment last step time by interval directly, avoiding drift. will also catchup on missed steps if the loop is busy for a while (when awaiting user input)
  }
}

/// @brief Check if a button is pressed and update last state for edge detection. Should be called in a loop for constant monitoring of button state.
/// @param pin Digital pin corresponding to checked button.
/// @param last Reference to the last known button state.
/// @return True if button is pressed, false otherwise.
bool onPress(int pin, bool &last) { 
  bool current = digitalRead(pin);
  if (current == LOW && last == HIGH) { // edge detection for press
    return true;
  }
  last = current; // update last state (function is constantly called in loop)
  return false;
}

/// @brief Wait for button release and handle debouncing.
/// @param pin The digital pin to monitor.
/// @param last Reference to the last known button state.
void waitRelease(int pin, bool &last) {
  delay(DEBOUNCE_MS);
  while (digitalRead(pin) == LOW); // wait for button release
  delay(DEBOUNCE_MS);
  last = HIGH; // force update of last state to HIGH, just in case of debounce oddities
}

/// @brief Increment or decrement target counter by given value.
/// @param target Reference to the target counter.
/// @param value Value to increment or decrement by.
void increment(long &target, int value) {
  target += useSubtract ? -value : value; 
  if (target < 0) target = 0; // prevent negative values
  saveState(); // save new value to EEPROM after every change
  defaultDisplay(); // refresh display to show new value
}

/// @brief Toggle a boolean flag and update associated LED states.
/// @param flag Reference to toggled flag.
void toggle(bool &flag) {
  flag = !flag;
  digitalWrite(subtractLED, useSubtract); // should autocast bool to HIGH/LOW
  digitalWrite(timeLED, useTime);
  defaultDisplay();
}

/// @brief Save state of counters into EEPROM.
void saveState() { 
  EEPROM.put(BALANCE_ADDR, balance); // EEPROM.put only writes when value has changed 
  EEPROM.put(TIME_ADDR, timeBalance); // this helps reduce likelihood of reaching the rated 100000 write cycles
  lastSeenTime = rtc.now().unixtime();
  EEPROM.put(TIMESTAMP_ADDR, lastSeenTime); // update last seen time as unixtime
}

/// @brief Load state from EEPROM into variables.
void loadState() { 
  EEPROM.get(BALANCE_ADDR, balance);
  EEPROM.get(TIME_ADDR, timeBalance);
  EEPROM.get(SHIFT_COUNT_ADDR, shiftCount);
  EEPROM.get(TIMESTAMP_ADDR,lastSeenTime);

  // sanity checks - uninitalized EEPROM may contain garbage values, so we set any out-of-range values to 0 to prevent issues
  if (balance < 0) {
    balance = 0;
    EEPROM.put(BALANCE_ADDR, balance);
  }
  if (timeBalance < 0) { 
    timeBalance = 0;
    EEPROM.put(TIME_ADDR, timeBalance);
  }
  if (shiftCount < 0 || shiftCount >= MAX_SHIFTS + 1) { 
    shiftCount = 0;
    EEPROM.put(SHIFT_COUNT_ADDR, shiftCount);
  }
  if (lastSeenTime == 0xFFFFFFFF) { // expected value on first boot with blank EEPROM
  lastSeenTime = rtc.now().unixtime();
  EEPROM.put(TIMESTAMP_ADDR, lastSeenTime);
  }
  if (lastSeenTime> rtc.now().unixtime()) {
    // handles future timestamps - causes overflow in the unsigned long and results in stepper motor spinning wildly
    // encountered in Wokwi because I cannot comment out rtc.adjust() without recompiling
    // should work fine IRL in real life since rtc.adjust() only needs to be done once and can be commented out after initial setup
    lastSeenTime = rtc.now().unixtime();
    EEPROM.put(TIMESTAMP_ADDR, lastSeenTime);
  }

}
/// @brief Refresh and display default screen. Should be called after any action that changes shift counters.
void defaultDisplay() {
  lcd.clear();
  lcd.setCursor(0, 0);
  if (useTime) {
    lcd.print(timeBalance / 60);
    lcd.print(" hours ");
    lcd.print(timeBalance % 60);
    lcd.print(" min");
  } else {
    lcd.print("$");
    lcd.print(balance / 100);
    lcd.print(".");
    long cents = balance % 100;
    if (cents < 10) lcd.print("0"); // leading zero for cents if needed
    lcd.print(cents);
  }
  lcd.setCursor(0, 1);
  if (useSubtract) lcd.print("SUBTRACT");
}

/// @brief Display a confirmation dialog on the LCD display. "+/-" to confirm, "time" to cancel.
/// @param prompt Prompt appearing above confirmation options. 16 char limit.
/// @return True if confirmed, false if cancelled
bool confirmDialog(const char* prompt) {
  lcd.clear();
  lcd.print(prompt);
  lcd.setCursor(0, 1);
  lcd.print("+/-:Yes  Time:No");

  // captive loop until user makes a choice, returns true for confirm and false for cancel
  while (true) {
    if (onPress(buttonSubToggle, lastStateSubToggle)) {
      waitRelease(buttonSubToggle, lastStateSubToggle);
      return true;
    }
    if (onPress(buttonTimeToggle, lastStateTimeToggle)) {
      waitRelease(buttonTimeToggle, lastStateTimeToggle);
      return false;
    }
  }
}

/// @brief Prompt user to save shift. Write appropiate data to EEPROM if they confirm. Triggered by TIME4.
void saveShift() {
  // confirm/cancel prompt:

  if (confirmDialog("Save shift?")) { // CONFIRMATION BLOCK     
      // memory full check, printout and exit if appropiate
      if (shiftCount >= MAX_SHIFTS) {
        lcd.clear();
        lcd.print("Memory full!");
        lcd.setCursor(0,1);
        lcd.print("Delete old shifts");
        delay(2500);
        defaultDisplay(); // return to main display and exit function
        return;
      }

      // create shift struct and write to EEPROM
      DateTime now = rtc.now(); // pull time as DataTime object from the RTC module
      // create shift struct with current balance, time, and timestamp (converted to unix time for storage)
      Shift newShift = {balance, timeBalance, now.unixtime()};
      // write new shift to EEPROM at the next available shift slot
      // we start at the shift start address, then add the number of shifts already stored multiplied by each one's size (in bytes)
      // this will give us the next valid address
      EEPROM.put(SHIFTS_START_ADDR + shiftCount * sizeof(Shift), newShift);
      shiftCount++; // increment shift count
      EEPROM.put(SHIFT_COUNT_ADDR, shiftCount); // update shift count in EEPROM
      balance = 0; // reset counters after saving shift
      timeBalance = 0;  
      saveState(); // save reset state to EEPROM

      // confirmation printout
      lcd.clear();
      lcd.print("Shift saved!");
      delay(2500);

      // refresh display and leave function
      defaultDisplay();
      return;
    }

    // CANCELLATION BLOCK - just return to main display and exit function
    else{
      // cancellation printout
      lcd.clear();
      lcd.print("Cancelled");
      delay(2500);

      // refresh and exit
      defaultDisplay();
      return;
    }
  }

/// @brief Review past shifts stored in EEPROM. Triggered by TIME5.
void shiftReviewMode() {
  
  // early exit for no shifts case
  if (shiftCount == 0) {
    lcd.clear();
    lcd.print("No shifts saved");
    delay(2000);
    defaultDisplay();
    return;
  }

  int index = shiftCount - 1; // start at most recent 

  // Looping block for reviewing shifts
  while (true) { 
    Shift s;
    EEPROM.get(SHIFTS_START_ADDR + index * sizeof(Shift), s); // pull shift data from EEPROM for current index, write into variable 
    DateTime dt(s.timestamp); // grab timestamp for shift

    // PRINTOUT BLOCK
    char line1[17]; // 16 chars + null terminator for LCD display
    char line2[17];
    snprintf(line1,17, "$%ld.%02ld %ldh%ldm", //"$X.XX YhZm"
      s.balance / 100, s.balance % 100, s.time / 60, s.time % 60);
    snprintf(line2,17, "%d/%d/%d #%d/%d", //"M/D/Y INDEX/TOTAL"
       dt.month(), dt.day(), dt.year() % 100, index + 1, shiftCount);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1);
    lcd.setCursor(0, 1);
    lcd.print(line2);

    // INPUT PROCESSING BLOCK
    while (true) {
      // button5: previous shift
      if (onPress(button5, lastState5)) {
        waitRelease(button5, lastState5);
        index--; // move to previous shift
        if (index < 0) index = shiftCount - 1; // wrap around
        break;
      }

      // button6: hourly wage calculation
      if (onPress(button6,lastState6)){
        waitRelease(button6, lastState6);
        lcd.clear();
        if (s.time == 0) {
          lcd.print("No time worked");
          delay(3000);
          break; // return to shift review after message display
        } 
        else {
          long wage = (s.balance * 60) / s.time; // cents per hour, s.time in minutes
          snprintf(line1,17, "Wage: $%ld.%02ld/hr", wage / 100, wage % 100);
          lcd.print(line1);
          delay(3000);
          break; // return to shift review after wage display
        }
      }
      // button7: delete shift from memory
      if (onPress(button7, lastState7)) {
        waitRelease(button7, lastState7);
        if (confirmDialog("Delete shift?")) {
          deleteShift(index); // delete shift at current index and update shift count/EEPROM accordingly
          lcd.clear();
          lcd.print("Shift deleted");
          delay(2500);
          if (shiftCount == 0) {
            defaultDisplay();
            return; // return to main display if no shifts remain after deletion
          }
          else {
            if (index >= shiftCount) index = shiftCount - 1; // adjust index if the most recent shift was deleted
            break; // back to review
          }
        }
        else{
          // cancellation printout
          lcd.clear();
          lcd.print("Cancelled");
          delay(2500);
          break; // return to shift review after cancellation
        }
      }
      // time button - exit review mode
      if (onPress(buttonTimeToggle, lastStateTimeToggle)) {
        waitRelease(buttonTimeToggle, lastStateTimeToggle);
        defaultDisplay();
        return; // leave the function and return to main display
      }
    }
  }
}

/// @brief Calculate difference between current time and last seen time.
/// @return Difference in seconds.
unsigned long timeDiff() {
  return rtc.now().unixtime() - lastSeenTime;
}

/// @brief Delete a shift at given index by rewriting subsequent shifts over it. Adjust shift count accordingly.
/// @param index Index of shift to delete
void deleteShift(int index) {
  for (int i = index; i < shiftCount - 1; i++) {
    Shift s;
    EEPROM.get(SHIFTS_START_ADDR + (i + 1) * sizeof(Shift), s);
    EEPROM.put(SHIFTS_START_ADDR + i * sizeof(Shift), s);
  }
  shiftCount--;
  EEPROM.put(SHIFT_COUNT_ADDR, shiftCount);
}

/// @brief Dump EEPROM contents to serial monitor.
void debugEEPROM() {
  Serial.println("--- EEPROM DUMP ---");
  Serial.print("Balance: ");
  Serial.println(balance);
  Serial.print("Time: ");
  Serial.println(timeBalance);
  Serial.print("Shift Count: ");
  Serial.println(shiftCount);

  for (int i = 0; i < shiftCount; i++) {
    Shift s;
    EEPROM.get(SHIFTS_START_ADDR + i * sizeof(Shift), s);
    DateTime dt(s.timestamp);

    Serial.print("Shift ");
    Serial.print(i);
    Serial.print(": $");
    Serial.print(s.balance / 100);
    Serial.print(".");
    if (s.balance % 100 < 10) Serial.print("0");
    Serial.print(s.balance % 100);
    Serial.print(" | ");
    Serial.print(s.time / 60);
    Serial.print("h ");
    Serial.print(s.time % 60);
    Serial.print("m | ");
    Serial.print(dt.month());
    Serial.print("/");
    Serial.print(dt.day());
    Serial.print("/");
    Serial.println(dt.year());
  }
  Serial.println("--- END DUMP ---");
}

/// @brief Upload test shifts for debugging in EEPROM. Only loads once.
void loadTestShifts() {
  int testDataMarker;
  EEPROM.get(1000, testDataMarker);
  if (testDataMarker == 1) {
    // test data already loaded, do not reload
    return;
  }
  Shift testShifts[] = {
    { 7200,  480, 1735776000UL },  // $72.00, 8h, Jan 1 2025
    { 5400,  360, 1736467200UL },  // $54.00, 6h, Jan 9
    { 9600,  540, 1737072000UL },  // $96.00, 9h, Jan 16
    { 4800,  300, 1737763200UL },  // $48.00, 5h, Jan 24
    { 10800, 600, 1738368000UL },  // $108.00, 10h, Feb 1
    { 6000,  420, 1739059200UL },  // $60.00, 7h, Feb 8
    { 8400,  480, 1739664000UL },  // $84.00, 8h, Feb 15
    { 11400, 660, 1740355200UL },  // $114.00, 11h, Feb 23
    { 7800,  480, 1740960000UL },  // $78.00, 8h, Mar 2
    { 5100,  300, 1741651200UL },  // $51.00, 5h, Mar 10
    { 9000,  540, 1742256000UL },  // $90.00, 9h, Mar 17
    { 6600,  420, 1742947200UL },  // $66.00, 7h, Mar 25
    { 10200, 600, 1743552000UL },  // $102.00, 10h, Apr 1
    { 4500,  300, 1744243200UL },  // $45.00, 5h, Apr 9
    { 8700,  480, 1744848000UL },  // $87.00, 8h, Apr 16
    { 12000, 720, 1745539200UL },  // $120.00, 12h, Apr 24
    { 7500,  480, 1746144000UL },  // $75.00, 8h, May 1
    { 5700,  360, 1746835200UL },  // $57.00, 6h, May 9
    { 9300,  540, 1747440000UL },  // $93.00, 9h, May 16
    { 6300,  420, 1748131200UL },  // $63.00, 7h, May 24
    { 10500, 600, 1748736000UL },  // $105.00, 10h, Jun 1
    { 4200,  240, 1749427200UL },  // $42.00, 4h, Jun 8
    { 8100,  480, 1750032000UL },  // $81.00, 8h, Jun 15
    { 11700, 660, 1750723200UL },  // $117.00, 11h, Jun 23
    { 7350,  480, 1751328000UL },  // $73.50, 8h, Jun 30
    { 5550,  360, 1752019200UL },  // $55.50, 6h, Jul 8
    { 9900,  540, 1752624000UL },  // $99.00, 9h, Jul 15
    { 6750,  420, 1753315200UL },  // $67.50, 7h, Jul 23
    { 10050, 600, 1753920000UL },  // $100.50, 10h, Jul 30
    { 4650,  300, 1754611200UL },  // $46.50, 5h, Aug 7
    { 8850,  480, 1755216000UL },  // $88.50, 8h, Aug 14
    { 11100, 660, 1755907200UL },  // $111.00, 11h, Aug 22
    { 7050,  480, 1756512000UL },  // $70.50, 8h, Aug 29
    { 5250,  360, 1757203200UL },  // $52.50, 6h, Sep 6
    { 9450,  540, 1757808000UL },  // $94.50, 9h, Sep 13
    { 6450,  420, 1758499200UL },  // $64.50, 7h, Sep 21
    { 10350, 600, 1759104000UL },  // $103.50, 10h, Sep 28
    { 4350,  300, 1759795200UL },  // $43.50, 5h, Oct 6
    { 8250,  480, 1760400000UL },  // $82.50, 8h, Oct 13
    { 11850, 720, 1761091200UL },  // $118.50, 12h, Oct 21
    { 7650,  480, 1761696000UL },  // $76.50, 8h, Oct 28
    { 5850,  360, 1762387200UL },  // $58.50, 6h, Nov 5
    { 9150,  540, 1762992000UL },  // $91.50, 9h, Nov 12
    { 6150,  420, 1763683200UL },  // $61.50, 7h, Nov 20
    { 10650, 600, 1764288000UL },  // $106.50, 10h, Nov 27
    { 4950,  300, 1764979200UL },  // $49.50, 5h, Dec 5
    { 8550,  480, 1765584000UL },  // $85.50, 8h, Dec 12
    { 11550, 660, 1766275200UL },  // $115.50, 11h, Dec 20
    { 7950,  480, 1767052800UL },  // $79.50, 8h, Jan 30 2026
    { 6900,  420, 1769472000UL },  // $69.00, 7h, Mar 2 2026
  };

  shiftCount = 50;
  EEPROM.put(SHIFT_COUNT_ADDR, shiftCount);

  for (int i = 0; i < shiftCount; i++) {
    EEPROM.put(SHIFTS_START_ADDR + i * sizeof(Shift), testShifts[i]);
  }

  EEPROM.put(1000, 1); // marker to indicate test shifts have been loaded
}