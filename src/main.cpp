#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <RTClib.h>
#define BALANCE_ADDR 0 // address for storing balance in EEPROM
#define TIME_ADDR    4 // address for storing time in EEPROM
#define DEBOUNCE_MS 50

// create objects to represent LCD and RTC modules, with appropriate parameters and I2C addresses
LiquidCrystal_I2C lcd(0x27, 16, 2); 
RTC_DS3231 rtc;

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

void handleButton(int pin, int value, bool &last, long &target, bool useSubtract);
void handleToggle(int pin, bool &last, bool &flag, const char* label);
void saveState();
void loadState();
void updateDisplay();

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
  long &target = useTime ? timeBalance : balance;

  if (onPress(button1, lastState1)) increment(target, useTime ? 15 : 1);
  if (onPress(button2, lastState2)) increment(target, useTime ? 60 : 5);
  if (onPress(button3, lastState3)) increment(target, useTime ? 300 : 25);

  if (!useTime) {
    if (onPress(button4, lastState4)) increment(balance, 100);
    if (onPress(button5, lastState5)) increment(balance, 500);
    if (onPress(button6, lastState6)) increment(balance, 2000);
    if (onPress(button7, lastState7)) increment(balance, 10000);
  } else {
    // time mode special functions for buttons 4-7
  }

  if (onPress(buttonSubToggle, lastStateSubToggle)) toggle(useSubtract, "Subtract");
  if (onPress(buttonTimeToggle, lastStateTimeToggle)) toggle(useTime, "Time");
}


bool onPress(int pin, bool &last) {
  bool current = digitalRead(pin);
  if (current == LOW && last == HIGH) {
    delay(DEBOUNCE_MS);
    while (digitalRead(pin) == LOW);
    delay(DEBOUNCE_MS);
    last = HIGH;
    return true;
  }
  last = current;
  return false;
}

void increment(long &target, int value) {
  target += useSubtract ? -value : value;
  if (target < 0) target = 0;
  Serial.println(target);
  saveState();
  updateDisplay();
}

void toggle(bool &flag, const char* label) {
  flag = !flag;
  Serial.print(label);
  Serial.println(flag ? ": ON" : ": OFF");
  updateDisplay();
}
void saveState() { // save state into EEPROM
  EEPROM.put(BALANCE_ADDR, balance); // EEPROM.put only writes when value has changed 
  EEPROM.put(TIME_ADDR, timeBalance); // this helps reduce likelihood of reaching the rated 100000 write cycles
}

void loadState() { // load state into EEPROM
  EEPROM.get(BALANCE_ADDR, balance);
  EEPROM.get(TIME_ADDR, timeBalance);
  // sanity check / workaround for default uninitialized EEPROM value of 0xFF
  if (balance < 0) {
    balance = 0;
    EEPROM.put(BALANCE_ADDR, balance);
  }
  if (timeBalance < 0) { 
    timeBalance = 0;
    EEPROM.put(TIME_ADDR, timeBalance);
  }
}

void updateDisplay() {
// LCD display update function - called after every state change to reflect new values
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