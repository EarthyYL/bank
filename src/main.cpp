#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <RTClib.h>

// EEPROM addresses
#define BALANCE_ADDR 0 
#define TIME_ADDR    4 
#define SHIFT_COUNT_ADDR 8 
#define SHIFTS_START_ADDR 10

// CURRENT EEPROM ALLOCATION:
// 0-3: balance (long, 4 bytes)
// 4-7: timeBalance (long, 4 bytes)
// 8-9: shiftCount (int, 2 bytes)
// 10-609: shift (struct (3 longs), 12 bytes, 50 entries allocated)
// 610-1023: unused (414 bytes free)

#define MAX_SHIFTS 50
#define DEBOUNCE_MS 50


// create objects to represent LCD and RTC modules, with appropriate parameters and I2C addresses
LiquidCrystal_I2C lcd(0x27, 16, 2); 
RTC_DS3231 rtc;

// custom struct for shift data to be stored in EEPROM
struct Shift {
  long balance;
  long time;
  long timestamp;
};


long balance = 0; // long: 4-byte integer (up to +- 2 million)
long timeBalance = 0;
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
bool useTime = false;       
bool useSubtract = false;
int shiftCount = 0;

bool onPress(int pin, bool &last);
void waitRelease(int pin, bool &last);
void increment(long &target, int value);
void toggle(bool &flag);
void saveState();
void loadState();
void updateDisplay();
void saveShift();


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

  loadState(); // grab memory of values before we begin

  // initalize LCD display
  lcd.init();
  lcd.backlight();
  updateDisplay(); // initial display update to show loaded values

  // initalize RTC
  rtc.begin();

  // COMMENT THIS OUT TO AVOID RESETTING TIME EVERY TIME YOU UPLOAD NEW CODE
  rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); // set RTC to compile time - only needs to be done once
}

void loop() {
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
  }

  if (onPress(buttonSubToggle, lastStateSubToggle)) { // toggle subtract mode
    toggle(useSubtract);
    waitRelease(buttonSubToggle, lastStateSubToggle);
  }
  if (onPress(buttonTimeToggle, lastStateTimeToggle)) { // toggle time mode
    toggle(useTime);
    waitRelease(buttonTimeToggle, lastStateTimeToggle);
  }
}


bool onPress(int pin, bool &last) { 
  /// Return true when button is pressed to trigger actions in loop
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
  updateDisplay(); // refresh display to show new value
}

void toggle(bool &flag) {
  /// Toggle a boolean flag and refresh display.
  flag = !flag;
  updateDisplay();
}

void saveState() { 
  /// Save state of counters into EEPROM.
  EEPROM.put(BALANCE_ADDR, balance); // EEPROM.put only writes when value has changed 
  EEPROM.put(TIME_ADDR, timeBalance); // this helps reduce likelihood of reaching the rated 100000 write cycles
}

void loadState() { 
  /// Load state from EEPROM into variables.
  EEPROM.get(BALANCE_ADDR, balance);
  EEPROM.get(TIME_ADDR, timeBalance);
  EEPROM.get(SHIFT_COUNT_ADDR, shiftCount);

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

}

void updateDisplay() {
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

void saveShift() {
  // Prompt user to save shift. Write appropiate data to EEPROM if they confirm.

  // confirm/cancel prompt
  lcd.clear();
  lcd.print("+/-: Confirm");
  lcd.setCursor(0, 1);
  lcd.print("Time: Cancel");
  
  // sit in loop until user makes a choice
  while (true) {

    // confirmation block - save shift and reset counters
    if (onPress(buttonSubToggle, lastStateSubToggle)) {
      waitRelease(buttonSubToggle, lastStateSubToggle);
      
      // memory full check, printout and exit if appropiate
      if (shiftCount >= MAX_SHIFTS) {
        lcd.clear();
        lcd.print("Memory full!");
        delay(2500);
        updateDisplay(); // return to main display and exit function
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
      updateDisplay();
      return;
    }

    // cancellation block - just return to main display and exit function
    if (onPress(buttonTimeToggle, lastStateTimeToggle)) {
      waitRelease(buttonTimeToggle, lastStateTimeToggle);

      // cancellation printout
      lcd.clear();
      lcd.print("Cancelled");
      delay(2500);

      // refresh and exit
      updateDisplay();
      return;
    }
    // still in while loop - update last states for toggle button before next iteration
    lastStateSubToggle = digitalRead(buttonSubToggle);
    lastStateTimeToggle = digitalRead(buttonTimeToggle);
  }

}