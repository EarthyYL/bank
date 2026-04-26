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
// 614-1023: unused (410 bytes free)

#define MAX_SHIFTS 50
#define DEBOUNCE_MS 50
#define STEPS_PER_REV 2048 // 0.176 degrees per step for 28BYJ-48 stepper motor
#define STEP_INTERVAL_US 1757813UL // step inteval in microseconds for 1 hour per revolution

// create objects to represent LCD, RTC, stepper modules, with appropriate parameters and I2C addresses
LiquidCrystal_I2C lcd(0x27, 16, 2); 
RTC_DS3231 rtc;
Stepper motor(STEPS_PER_REV, 2, A0, 5, A1); // pins for ULN2003

// custom struct for shift data to be stored in EEPROM
struct Shift {
  long balance;
  long time;
  unsigned long timestamp;
};



long balance = 0; // long: 4-byte integer (up to +- 2 million)
long timeBalance = 0;
unsigned long lastSeenTime = 0;
int button1 = 6;
int button2 = 7;
int button3 = 8;
int button4 = 9;
int button5 = 10;
int button6 = 11;
int button7 = 12;
int buttonSubToggle = 3;
int buttonTimeToggle = 4;
bool lastState1 = HIGH;
bool lastState2 = HIGH;
bool lastState3 = HIGH;
bool lastState4 = HIGH;
bool lastState5 = HIGH;
bool lastState6 = HIGH;
bool lastState7 = HIGH;
bool lastStateSubToggle = HIGH;
bool lastStateTimeToggle = HIGH;
unsigned long lastStepTime = 0;
bool useTime = false;       
bool useSubtract = false;
int shiftCount = 0;

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


bool onPress(int pin, bool &last) { 
  /// Return true when button is pressed to trigger actions in loop and update last state
  bool current = digitalRead(pin);
  if (current == LOW && last == HIGH) { // edge detection for press
    return true;
  }
  last = current; // update last state (function is constantly called in loop)
  return false;
}

void waitRelease(int pin, bool &last) {
  /// Wait for button release and handle debouncing. Called after a button press is detected
  delay(DEBOUNCE_MS);
  while (digitalRead(pin) == LOW); // wait for button release
  delay(DEBOUNCE_MS);
  last = HIGH; // force update of last state to HIGH, just in case of debounce oddities
}


void increment(long &target, int value) {
  /// Increment or decrement target counter by given value.
  target += useSubtract ? -value : value; 
  if (target < 0) target = 0; // prevent negative values
  saveState(); // save new value to EEPROM after every change
  defaultDisplay(); // refresh display to show new value
}

void toggle(bool &flag) {
  /// Toggle a boolean flag and refresh display.
  flag = !flag;
  defaultDisplay();
}

void saveState() { 
  /// Save state of counters into EEPROM.
  EEPROM.put(BALANCE_ADDR, balance); // EEPROM.put only writes when value has changed 
  EEPROM.put(TIME_ADDR, timeBalance); // this helps reduce likelihood of reaching the rated 100000 write cycles
  lastSeenTime = rtc.now().unixtime();
  EEPROM.put(TIMESTAMP_ADDR, lastSeenTime); // update last seen time as unixtime
}

void loadState() { 
  /// Load state from EEPROM into variables.
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

void defaultDisplay() {
// LCD display update function - called after every state change to reflect new values
// Only for standard display - other screens handeled in other functions
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


void saveShift() {
  // Prompt user to save shift. Write appropiate data to EEPROM if they confirm. Trigger by TIME4.
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


void shiftReviewMode() {
  /// Review past shifts stored in EEPROM. Triggered by TIME5.
  
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

unsigned long timeDiff() {
  /// Calculate difference between current time and last seen time.
  DateTime now = rtc.now();
  return now.unixtime() - lastSeenTime;

}

void deleteShift(int index) {
  /// Delete a shift at given index by rewriting subsequent shifts over it. Adjust shift count accordingly.
  for (int i = index; i < shiftCount - 1; i++) {
    Shift s;
    EEPROM.get(SHIFTS_START_ADDR + (i + 1) * sizeof(Shift), s);
    EEPROM.put(SHIFTS_START_ADDR + i * sizeof(Shift), s);
  }
  shiftCount--;
  EEPROM.put(SHIFT_COUNT_ADDR, shiftCount);
}




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

void loadTestShifts() {
  int testDataMarker;
  EEPROM.get(1000, testDataMarker);
  if (testDataMarker == 1) {
    // test data already loaded, do not reload
    return;
  }
  Shift testShifts[] = {
    { 4500,  480, 1714000000UL },  // $45.00, 8h, Apr 2024
    { 7225,  540, 1714100000UL },  // $72.25, 9h, Apr 2024
    { 3000,  360, 1714200000UL },  // $30.00, 6h, Apr 2024
    { 12050, 600, 1714300000UL },  // $120.50, 10h, Apr 2024
    { 0,     0,   1714400000UL },  // empty shift, edge case
  };

  shiftCount = 5;
  EEPROM.put(SHIFT_COUNT_ADDR, shiftCount);

  for (int i = 0; i < shiftCount; i++) {
    EEPROM.put(SHIFTS_START_ADDR + i * sizeof(Shift), testShifts[i]);
  }

  EEPROM.put(1000, 1); // marker to indicate test shifts have been loaded
}
