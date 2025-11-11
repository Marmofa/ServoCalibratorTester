/*
 * ensure you have a 2.5amp 5volt supply as the motor needs this for start up.
 * Buttons use internal pullup no resistor needed - connect to ground
 * 
 */

#include <Servo.h>
#include <EEPROM.h> // Include the permanent memory library

// --- Configuration ---
const int SERVO_PIN = 9;  // Servo signal wire
const int LED_PIN = 5;    // LED indicator pin (D5)
const int UP_BUTTON_PIN = 2;   // Button for CCW twitch
const int DOWN_BUTTON_PIN = 3; // Button for CW twitch

// --- Calibration Config ---
const int BIG_STEP = 10;  // How much to jump by in the fast search
const int SMALL_STEP = 1; // How much to step by in the fine search
const int CALIB_STOP = 1500; // Theoretical stop pulse for calibration

// --- Ramping Test Config ---
const int TEST_REPEATS = 10;   // How many pulses to send per speed/delay test
int current_step_delay = 100; // Starts at 100ms for the AUTO test
const int LED_FLASH_MS = 100;   // "Beep" duration
const int MANUAL_TWITCH_MS = 100; // Fixed 100ms for manual mode

// --- Debounce Config ---
const unsigned long debounceDelay = 50; // 50ms for button debounce

// --- NEW: Timeout Config ---
const unsigned long serialTimeout = 10000; // 10 seconds (10,000 ms)
unsigned long promptStartTime = 0; // Will store when the prompt starts

Servo myServo;
int testState = 0;    // Tracks progress through the wizard
int currentPulse = 1500; // The pulse width currently testing

// --- Global variables to store findings ---
int cw_limit = 0;
int ccw_limit = 0;
int last_known_move_cw = 0;
int last_known_move_ccw = 0;
int recommended_stop = 0; // Will be calculated at the end of calibration

// --- Global variables for debouncing ---
int upButtonState;
int lastUpButtonState = HIGH; //pull-up
unsigned long lastUpDebounceTime = 0;
int downButtonState;
int lastDownButtonState = HIGH; // pull-up
unsigned long lastDownDebounceTime = 0;


void setup() {
  myServo.attach(SERVO_PIN);
  pinMode(LED_PIN, OUTPUT);
  
  // Set up button pins with internal pull-up resistors
  pinMode(UP_BUTTON_PIN, INPUT_PULLUP);
  pinMode(DOWN_BUTTON_PIN, INPUT_PULLUP);
  
  Serial.begin(9600);
  while (!Serial) { ; } // Wait for serial

  Serial.println(F("--- All-in-One Calibrator & Tester & Focuser(v1.0.0) ---"));
  
  // --- MODIFIED STARTUP LOGIC ---
  if (loadCalibration()) {
    // Found valid data
    Serial.println(F("Loaded saved calibration values:"));
    Serial.print(F("  CW Limit: ")); Serial.println(cw_limit);
    Serial.print(F("  CCW Limit: ")); Serial.println(ccw_limit);
    Serial.print(F("  Stop Value: ")); Serial.println(recommended_stop);
    testState = 1; // <<< Go to NEW "Start Prompt" state
  } else {
    // No data found
    Serial.println(F("No valid calibration data found."));
    Serial.println(F("Forcing new calibration..."));
    testState = 10; // Go directly to calibration
  }
}

/**
 * A robust function to get user input.
 */
char getSerialInput() {
  char input;
  while (true) {
    if (Serial.available() > 0) {
      input = Serial.read();
      if (input == 'y' || input == 'Y' ||
          input == 'n' || input == 'N' ||
          input == '?') {
        return input; // We got a valid command
      }
      // If it's \r, \n, or other junk, just ignore it.
    }
  }
}

/**
 * CALIBRATION: Sends the TEST pulse, flashes the LED, waits 1 sec, and prints.
 */
void sendCalibPulse(int pulse) {
  myServo.writeMicroseconds(pulse);
  digitalWrite(LED_PIN, HIGH);
  delay(100);
  digitalWrite(LED_PIN, LOW);
  delay(900);
  Serial.print(F("Sent "));
  Serial.print(pulse);
  Serial.println(F("µs. Did it move? (y/n/?)"));
}

/**
 * CALIBRATION: Sends the STOP pulse, flashes LED, and waits 1 sec.
 */
void sendCalibStopPulse() {
  Serial.println(F("-> Movement detected. Stopping motor for 1s..."));
  myServo.writeMicroseconds(CALIB_STOP);
  digitalWrite(LED_PIN, HIGH);
  delay(100);
  digitalWrite(LED_PIN, LOW);
  delay(900);
  Serial.println(F("-> Motor stopped. Preparing next test."));
}

/**
 * RAMPING TEST: Helper function to send a pulse, print to console,
 * flash the LED, and wait.
 * --- Will only beep and print on MOVE commands ---
 */
void sendTestPulse(int pulse) {
  // 1. Send command to servo
  myServo.writeMicroseconds(pulse);

  // 2. Check if it's a MOVE or STOP command
  if (pulse != recommended_stop) {
    // --- This is a MOVE command ---
    
    // A. Print to console
    Serial.print(F("Sending MOVE: "));
    Serial.print(pulse);
    Serial.print(F("µs for "));
    Serial.print(current_step_delay);
    Serial.println(F("ms"));

    // B. "Beep" - Flash the LED
    digitalWrite(LED_PIN, HIGH);
    
    // C. Wait for the duration
    if (current_step_delay >= LED_FLASH_MS) {
       delay(LED_FLASH_MS);
       digitalWrite(LED_PIN, LOW);
       delay(current_step_delay - LED_FLASH_MS);
    } else {
       delay(current_step_delay);
       digitalWrite(LED_PIN, LOW);
    }
  } else {
    // --- This is a STOP command ---
    // Beep and print nothing.
    // Just wait the full step delay to hold the motor at stop.
    delay(current_step_delay);
  }
}

/**
 * --- NEW FUNCTION for Manual Mode ---
 * Sends a single move pulse for a fixed 100ms, then stops.
 */
void sendManualTwitch(int pulse) {
  // 1. Print to console
  Serial.print(F("Sending MANUAL MOVE: "));
  Serial.print(pulse);
  Serial.print(F("µs for "));
  Serial.print(MANUAL_TWITCH_MS);
  Serial.println(F("ms"));

  // 2. Send command to servo
  myServo.writeMicroseconds(pulse);

  // 3. "Beep" - Flash the LED
  digitalWrite(LED_PIN, HIGH);
  
  // 4. Wait for the fixed 100ms duration
  delay(MANUAL_TWITCH_MS);
  digitalWrite(LED_PIN, LOW);
  
  // 5. Send STOP
  myServo.writeMicroseconds(recommended_stop);
  Serial.println(F("...Stopped."));
}


/**
 * Saves the calibration data to permanent EEPROM memory
 */
void saveCalibration() {
  Serial.println(F("-> Saving values to EEPROM..."));
  // We use different "addresses" (0, 10, 20) to store each int
  EEPROM.put(0, cw_limit);
  EEPROM.put(10, ccw_limit);
  EEPROM.put(20, recommended_stop);
  Serial.println(F("-> Save complete."));
}

/**
 * Loads calibration data from EEPROM.
 * Returns 'true' if data seems valid, 'false' otherwise.
 */
bool loadCalibration() {
  Serial.println(F("Loading values from EEPROM..."));
  EEPROM.get(0, cw_limit);
  EEPROM.get(10, ccw_limit);
  EEPROM.get(20, recommended_stop);
  Serial.println(F("...Load complete."));

  // Simple check to see if the data is valid
  if (cw_limit < 1000 || cw_limit > 2000) return false;
  if (ccw_limit < 1000 || ccw_limit > 2000) return false;
  if (recommended_stop < 1000 || recommended_stop > 2000) return false;

  return true; // Data looks good
}


void loop() {
  char input;

  switch (testState) {
    // --- STATE 0: Handled in setup() ---
    case 0:
      // This state is just a placeholder in case setup() fails.
      break;

    // --- STATE 1: Print Recalibrate Prompt and Start Timer --- (NEW)
    case 1:
      Serial.println(F("\nRecalibrate? (y/n) (10s timeout)"));
      promptStartTime = millis(); // Start the 10-second timer
      testState = 2; // Move to the "wait for answer" state
      break;

    // --- STATE 2: Wait for Recalibrate Y/N or Timeout --- (NEW)
    case 2:
      // Check for user input first
      if (Serial.available() > 0) {
        char choice = Serial.read();
        while (Serial.available() > 0) Serial.read(); // Clear buffer

        if (choice == 'y' || choice == 'Y') {
          Serial.println(F("\n--- Phase 1: Finding Clockwise (CW) Limit ---"));
          currentPulse = CALIB_STOP;
          testState = 10; // Go to calibration
        } else if (choice == 'n' || choice == 'N') {
          Serial.println(F("-> No. Entering Manual Control Mode."));
          Serial.print(F("Press UP button for 100ms CCW twitch ("));
          Serial.print(ccw_limit);
          Serial.println(F("µs)."));
          Serial.print(F("Press DOWN button for 100ms CW twitch ("));
          Serial.print(cw_limit);
          Serial.println(F("µs)."));
          testState = 40; // Go to Manual Control
        } else {
          Serial.println(F("Invalid choice. Recalibrate? (y/n)"));
          promptStartTime = millis(); // Reset timer on invalid input
        }
      }

      // Check for timeout *after* checking serial
      if (millis() - promptStartTime > serialTimeout) {
        Serial.println(F("\nTimeout. No response."));
        Serial.println(F("Entering Manual Control Mode."));
        Serial.print(F("Press UP button for 100ms CCW twitch ("));
        Serial.print(ccw_limit);
        Serial.println(F("µs)."));
        Serial.print(F("Press DOWN button for 100ms CW twitch ("));
        Serial.print(cw_limit);
        Serial.println(F("µs)."));
        testState = 40; // <<< This is "run the application"
      }
      break;

    // --- STATE 10: Check 1500 (CW) ---
    case 10:
      Serial.println(F("First, checking 1500us (STOP)..."));
      sendCalibPulse(CALIB_STOP);
      input = getSerialInput();
      if (input == 'n' || input == 'N') { // Good, 1500 is stop
        Serial.println(F("-> OK. Begin BIG STEP search..."));
        currentPulse = CALIB_STOP + BIG_STEP;
        testState = 11; // Go to Big Step search
      } else if (input == 'y' || input == 'Y') { // 1500 moves!
        Serial.println(F("-> 1500 moves! Begin FINE search (going down)..."));
        sendCalibStopPulse(); // <<< STOP MOTOR
        last_known_move_cw = CALIB_STOP;
        currentPulse = CALIB_STOP - SMALL_STEP;
        testState = 12; // Go straight to Fine search
      } else if (input == '?') {
        testState = 10; // Repeat this state
      }
      break;

    // --- STATE 11: CW Big Step Search (Going UP) ---
    case 11:
      sendCalibPulse(currentPulse);
      input = getSerialInput();
      if (input == 'n' || input == 'N') { // Still no move
        Serial.println(F("-> No move. Trying next BIG step..."));
        currentPulse += BIG_STEP;
        testState = 11; // Stay in this state
      } else if (input == 'y' || input == 'Y') { // Found movement!
        Serial.println(F("-> Found movement! Begin FINE search (going down)..."));
        sendCalibStopPulse(); // <<< STOP MOTOR
        last_known_move_cw = currentPulse;
        currentPulse -= SMALL_STEP;
        testState = 12; // Go to Fine search
      } else if (input == '?') {
        testState = 11; // Repeat this state
      }
      break;

    // --- STATE 12: CW Fine Step Search (Going DOWN) ---
    case 12:
      sendCalibPulse(currentPulse);
      input = getSerialInput();
      if (input == 'y' || input == 'Y') { // Still moving
        Serial.println(F("-> Still moving. Trying next SMALL step down..."));
        sendCalibStopPulse(); // <<< STOP MOTOR
        last_known_move_cw = currentPulse; // Save this as the new edge
        currentPulse -= SMALL_STEP;
        testState = 12; // Stay in this state
      } else if (input == 'n' || input == 'N') { // It stopped!
        Serial.println(F("-> Motor stopped."));
        cw_limit = last_known_move_cw; // The *previous* value was the limit
        Serial.print(F(">>> CW Move Limit Found: "));
        Serial.println(cw_limit);
        testState = 20; // Move to CCW tests
      } else if (input == '?') {
        testState = 12; // Repeat this state
      }
      break;

    // --- STATE 20: Check 1500 (CCW) ---
    case 20:
      Serial.println(F("\n--- Phase 2: Finding Counter-Clockwise (CCW) Limit ---"));
      Serial.println(F("Checking 1500us (STOP) again..."));
      sendCalibPulse(CALIB_STOP);
      input = getSerialInput();
      if (input == 'n' || input == 'N') {
        Serial.println(F("-> OK. Begin BIG STEP search..."));
        currentPulse = CALIB_STOP - BIG_STEP;
        testState = 21; // Go to Big Step search
      } else if (input == 'y' || input == 'Y') {
        Serial.println(F("-> 1500 moves! Begin FINE search (going up)..."));
        sendCalibStopPulse(); // <<< STOP MOTOR
        last_known_move_ccw = CALIB_STOP;
        currentPulse = CALIB_STOP + SMALL_STEP;
        testState = 22; // Go straight to Fine search
      } else if (input == '?') {
        testState = 20; // Repeat this state
      }
      break;

    // --- STATE 21: CCW Big Step Search (Going DOWN) ---
    case 21:
      sendCalibPulse(currentPulse);
      input = getSerialInput();
      if (input == 'n' || input == 'N') {
        Serial.println(F("-> No move. Trying next BIG step..."));
        currentPulse -= BIG_STEP;
        testState = 21;
      } else if (input == 'y' || input == 'Y') {
        Serial.println(F("-> Found movement! Begin FINE search (going up)..."));
        sendCalibStopPulse(); // <<< STOP MOTOR
        last_known_move_ccw = currentPulse;
        currentPulse += SMALL_STEP;
        testState = 22;
      } else if (input == '?') {
        testState = 21;
      }
      break;

    // --- STATE 22: CCW Fine Step Search (Going UP) ---
    case 22:
      sendCalibPulse(currentPulse);
      input = getSerialInput();
      if (input == 'y' || input == 'Y') {
        Serial.println(F("-> Still moving. Trying next SMALL step up..."));
        sendCalibStopPulse(); // <<< STOP MOTOR
        last_known_move_ccw = currentPulse; // Save this as the new edge
        currentPulse += SMALL_STEP;
        testState = 22;
      } else if (input == 'n' || input == 'N') {
        Serial.println(F("-> Motor stopped."));
        ccw_limit = last_known_move_ccw; // The previous value was the limit
        Serial.print(F(">>> CCW Move Limit Found: "));
        Serial.println(ccw_limit);
        testState = 30; // Move to Report
      } else if (input == '?') {
        testState = 22;
      }
      break;

    // --- STATE 30: Test Complete, Report Results (MODIFIED) ---
    case 30:
      Serial.println(F("\n\n--- CALIBRATION COMPLETE ---"));
      Serial.println(F("Here are your calibrated values:"));
      Serial.print(F("Slowest CW Speed: ")); Serial.println(cw_limit);
      Serial.print(F("Slowest CCW Speed: ")); Serial.println(ccw_limit);
      Serial.print(F("Deadband Range: "));
      Serial.print(ccw_limit + 1);
      Serial.print(F("µs to "));
      Serial.print(cw_limit - 1);
      Serial.println(F("µs"));

      // Calculate the ideal stop point and save to global variable
      recommended_stop = round((cw_limit + ccw_limit) / 2.0);
      Serial.print(F("Recommended STOP Value: ")); Serial.println(recommended_stop);
      
      Serial.println(F("\nSave these values to permanent memory? (y/n)"));
      input = getSerialInput(); // We can re-use the 'y'/'n' function
      
      if (input == 'y' || input == 'Y') {
        saveCalibration();
      } else {
        Serial.println(F("-> Values not saved."));
      }
      
      Serial.println(F("\n--- PART 2: FINEST MOVEMENT (RAMP-UP) TEST ---"));
      testState = 31; // Move to the ramping test setup
      break;

    // --- STATE 31: Ramping Test Setup ---
    case 31:
      Serial.println(F("Stopping motor and starting ramping test in 3 seconds..."));
      Serial.print(F("--- STARTING WITH STEP DELAY: "));
      Serial.print(current_step_delay);
      Serial.println(F("ms ---"));
      sendTestPulse(recommended_stop); // Send first stop (will be silent)
      delay(2000); // Wait 2 more seconds
      testState = 32; // Move to start of ramp test
      break;

    // --- STATE 32: Test SLOWEST CW Speed (Ramp Up) ---
    case 32:
      Serial.println(F("\n--- Testing SLOWEST CW Speed ---"));
      for (int i = 0; i < TEST_REPEATS; i++) {
        sendTestPulse(cw_limit);          // <<< SEND SLOWEST MOVE (will beep)
        sendTestPulse(recommended_stop);  // <<< SEND STOP (will be silent)
      }
      testState = 33; // Go to CCW test
      break;

    // --- STATE 33: Test SLOWEST CCW Speed (Ramp Up) ---
    case 33:
      Serial.println(F("\n--- Testing SLOWEST CCW Speed ---"));
      for (int i = 0; i < TEST_REPEATS; i++) {
        sendTestPulse(ccw_limit);         // <<< SEND SLOWEST MOVE (will beep)
        sendTestPulse(recommended_stop);  // <<< SEND STOP (will be silent)
      }
      testState = 34; // Go to stop/increment
      break;

    // --- STATE 34: Stop, Increment Delay, and Check (Ramp Up) ---
    case 34:
      Serial.println(F("\n--- Stopping Motor (3s) ---"));
      sendTestPulse(recommended_stop); // Send silent stop
      delay(2000); // 2s + 1s from sendTestPulse = 3s total

      // --- NEW LOGIC: Check if we are done ramping up ---
      if (current_step_delay >= 500) {
        // We are done with the ramp-up test.
        Serial.println(F("\n\n*** Ramp-Up Test Complete. ***"));
        Serial.println(F("--- Starting Ramp-Down Test from 500ms ---"));
        current_step_delay = 500; // Set delay for the new test
        testState = 35; // Go to start of ramp-down test
      } else {
        // Not done, keep ramping up
        current_step_delay += 25; // Increase step time by 25ms
        Serial.println(F("\n\n*** Ramping test cycle complete. ***"));
        Serial.print(F("--- INCREASING STEP DELAY TO: "));
        Serial.print(current_step_delay);
        Serial.println(F("ms ---"));
        testState = 32; // Go back to the start of the CW test
      }
      break;

    // --- STATE 35: Test SLOWEST CW Speed (Ramp Down) ---
    case 35:
      Serial.println(F("\n--- Testing CW (Ramping Down Time) ---"));
      for (int i = 0; i < TEST_REPEATS; i++) {
        sendTestPulse(cw_limit);
        sendTestPulse(recommended_stop);
      }
      testState = 36;
      break;

    // --- STATE 36: Test SLOWEST CCW Speed (Ramp Down) ---
    case 36:
      Serial.println(F("\n--- Testing CCW (Ramping Down Time) ---"));
      for (int i = 0; i < TEST_REPEATS; i++) {
        sendTestPulse(ccw_limit);
        sendTestPulse(recommended_stop);
      }
      testState = 37;
      break;

    // --- STATE 37: Stop, Decrement Delay, and Check (Ramp Down) ---
    case 37:
      Serial.println(F("\n--- Stopping Motor (3s) ---"));
      sendTestPulse(recommended_stop); // Send silent stop
      delay(2000); // 2s + 1s from sendTestPulse = 3s total

      // --- NEW LOGIC: Check if we are done ramping down ---
      current_step_delay -= 25; // Decrease step time
      
      if (current_step_delay < 100) {
        // We are finished with the ramp-down test.
        Serial.println(F("\n\n*** Calibration complete. ***"));
        Serial.println(F("Entering Manual Control Mode."));
        Serial.print(F("Press UP button for 100ms CCW twitch ("));
        Serial.print(ccw_limit);
        Serial.println(F("µs)."));
        Serial.print(F("Press DOWN button for 100ms CW twitch ("));
        Serial.print(cw_limit);
        Serial.println(F("µs)."));
        testState = 40; // Go to Manual Mode
      } else {
        // Not done, keep ramping down
        Serial.println(F("\n\n*** Ramping test cycle complete. ***"));
        Serial.print(F("--- DECREASING STEP DELAY TO: "));
        Serial.print(current_step_delay);
        Serial.println(F("ms ---"));
        testState = 35; // Go back to the start of the ramp-down test
      }
      break;

    // --- STATE 40: Manual Control Mode (Corrected Button Logic) ---
    case 40:
      // --- Debounce Logic for UP Button (CCW Twitch) ---
      int upReading = digitalRead(UP_BUTTON_PIN);
      if (upReading != lastUpButtonState) {
        lastUpDebounceTime = millis();
      }
      if ((millis() - lastUpDebounceTime) > debounceDelay) {
        if (upReading != upButtonState) {
          upButtonState = upReading;
          if (upButtonState == LOW) { // Button is pressed (LOW because of PULLUP)
            sendManualTwitch(ccw_limit); // <<< Send one CCW twitch
          }
        }
      }
      lastUpButtonState = upReading;

      // --- Debounce Logic for DOWN Button (CW Twitch) ---
      int downReading = digitalRead(DOWN_BUTTON_PIN);
      if (downReading != lastDownButtonState) {
        lastDownDebounceTime = millis();
      }
      if ((millis() - lastDownDebounceTime) > debounceDelay) {
        if (downReading != downButtonState) {
          downButtonState = downReading;
          if (downButtonState == LOW) { // Button is pressed
            sendManualTwitch(cw_limit); // <<< Send one CW twitch
          }
        }
      }
      lastDownButtonState = downReading;
      
      break; // Stay in STATE 40 forever
  }
}
