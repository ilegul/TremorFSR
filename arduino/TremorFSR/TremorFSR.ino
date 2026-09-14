/*
  tremor_fsr.ino
  Arduino side: sample three FSRs and send them to the PC.

  The board does no analysis. The interval between samples 
  stays constant. The sampling theorem assumes regular sampling. Irregular
  sampling would corrupt the spectrum computed on the PC.

  Wiring (Arduino Due, 3.3 V):

    FSR1 -> A0     FSR2 -> A1     FSR3 -> A2
      each FSR between 3.3 V and its analog pin,
      one resistor from that pin to GND, per channel:

          A0 : 200 kohm      A1 : 1 kohm      A2 : 15 kohm

    RGB LED, common cathode:
      R -> D9, G -> D10, B -> D11, each through 220 ohm
      common -> GND

  The three resistors differ because the three sensors do. Each FSR
  forms a divider with its resistor,

      ADC = 1023 * R / (R + R_fsr)

  and the sensitivity of that divider, d(ADC)/d(R_fsr), is largest
  when R equals the sensor resistance at the pressure of interest:
  differentiating shows the maximum falls exactly at R = R_fsr. The
  three sensors are of different size and make. Under the same
  finger they sit at very different resistances, so a single value
  cannot suit all three. Measured under working pressure:

      A0 : ~200 kohm       A1 : ~1 kohm       A2 : ~16 kohm

  which the resistors above match. With 10 kohm on all three
  the readings spanned a factor of 19 between channels, one of them
  never leaving the bottom of the scale; matched, they span a factor
  of 1.3 and all three contribute comparably to the aggregated index.

  The index itself does not require this: it is a ratio of
  powers taken from one channel, so a per-sensor gain cancels out.
  The matching improves signal-to-noise. It cannot be done in
  software: amplifying a signal that spans three ADC counts amplifies
  the quantisation noise with it.

  IMPORTANT: the Due is not 5 V tolerant. Power the FSR dividers from
  3.3 V, never from 5 V.

  This sketch targets the Due specifically, not Arduino in general:
  analogReadResolution() and analogWriteResolution() below exist on the
  SAM based boards and not on the AVR ones and the 3.3 V supply of the
  dividers is a property of this board. On an Uno or a Mega those two
  calls have to be dropped and the dividers moved to 5 V.

  Protocol, board -> PC (plain ASCII, one line per sample):

      sample_id,time_ms,fsr1,fsr2,fsr3

  Protocol, PC -> board:

      LED,GREEN     normal
      LED,YELLOW    pre alert
      LED,RED       alert
      LED,OFF

  Serial speed and why it is 250000.

  The SAM3X generates the baud rate as MCK / (16 * CD) with CD an
  integer and MCK = 84 MHz. The core computes CD by integer division.
  Only the rates for which 5250000 / baud is an integer come out exact:

      requested   CD    actual      error
       1000000     5    1050000     +5 %
        500000    10     525000     +5 %
        250000    21     250000      0 %

  A UART tolerates about 2-3 % of mismatch, so 500000 and 1000000 are
  unusable on this board even though they are exact on the ATmega16U2
  that bridges the Programming Port to USB. 250000 is exact on both
  sides and is therefore the highest rate this link supports.

  That fixes the sampling rate too. At 250000 baud the line carries
  25 kB/s; one sample is about 30 characters, so 1000 Hz would need
  30 kB/s and would not fit. At 500 Hz the stream is 15 kB/s, which
  leaves a margin. 500 Hz is above what the
  4-10 Hz band of interest requires: the Nyquist limit becomes 250 Hz.

  On an 8N1 line each character costs 10 bits, not 8, so the usable
  byte rate is the baud rate divided by ten.
*/

const int FSR1_PIN = A0;
const int FSR2_PIN = A1;
const int FSR3_PIN = A2;

const int LED_R_PIN = 9;
const int LED_G_PIN = 10;
const int LED_B_PIN = 11;

/* must match the speed the host opens the port at */
const unsigned long BAUD_RATE = 250000;

/* 500 Hz -> one sample every 2000 microseconds */
const unsigned long SAMPLE_PERIOD_US = 2000;

unsigned long last_sample_us = 0;

/* counts the samples produced; identifies them on the wire */
unsigned long sample_id = 0;


/* the RGB LED is common cathode: a higher value is a brighter channel */
void set_led(int r, int g, int b) {
  analogWrite(LED_R_PIN, r);
  analogWrite(LED_G_PIN, g);
  analogWrite(LED_B_PIN, b);
}


void execute_command(String command) {
  command.trim();
  command.toUpperCase();

  if      (command == "LED,GREEN")  set_led(0, 255, 0);
  else if (command == "LED,YELLOW") set_led(255, 255, 0);
  else if (command == "LED,RED")    set_led(255, 0, 0);
  else if (command == "LED,OFF")    set_led(0, 0, 0);
}


/*
  Commands arrive rarely, only when the state changes, but reading
  one must never take longer than the interval between two samples.
  readStringUntil() would wait for its own timeout, which is longer
  than the 2 ms period, so the characters already in the buffer are
  taken instead and the line is assembled across calls.
*/
char command_buffer[16];
byte command_length = 0;

void read_pc_commands() {
  while (Serial.available() > 0) {
    const char c = Serial.read();

    if (c == '\n') {
      command_buffer[command_length] = '\0';
      execute_command(String(command_buffer));
      command_length = 0;
    }
    else if (command_length >= sizeof(command_buffer) - 1) {
      command_length = 0;   /* longer than any command: discard */
    }
    else if (c != '\r') {
      command_buffer[command_length++] = c;
    }
  }
}


void send_sample() {
  int fsr1 = analogRead(FSR1_PIN);
  int fsr2 = analogRead(FSR2_PIN);
  int fsr3 = analogRead(FSR3_PIN);

  /*
    The identifier counts the samples the board has produced, so a gap
    in it means exactly that many samples did not reach the host. The
    timestamp cannot serve that purpose on its own: the millisecond
    resolution of millis() is too coarse to distinguish a lost sample
    from ordinary jitter in the sampling interval, so a gap in it would
    not be evidence of a loss. The timestamp is still sent because it places
    each sample in time.
  */
  Serial.print(sample_id);
  Serial.print(',');
  Serial.print(millis());
  Serial.print(',');
  Serial.print(fsr1);
  Serial.print(',');
  Serial.print(fsr2);
  Serial.print(',');
  Serial.println(fsr3);

  sample_id++;
}


void setup() {
  Serial.begin(BAUD_RATE);

  analogReadResolution(10);   /* 0 .. 1023, same as an Uno */
  analogWriteResolution(8);   /* 0 .. 255                  */

  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);

  set_led(0, 0, 0);

  /*
    Identify the firmware on the link. If this line is readable, the
    board is running this sketch and both ends agree on the baud rate;
    if it is not, they do not. It begins with '#', which both readers
    on the PC side ignore, so it does not affect the protocol.
  */
  delay(50);
  Serial.print("# TremorFSR ready at ");
  Serial.print(BAUD_RATE);
  Serial.println(" baud");

  last_sample_us = micros();
}


void loop() {
  read_pc_commands();

  /*
    The unsigned subtraction keeps working when micros() wraps around
    (about every 70 minutes): the difference wraps the same way.
  */
  unsigned long now = micros();
  if (now - last_sample_us >= SAMPLE_PERIOD_US) {
    last_sample_us += SAMPLE_PERIOD_US;
    send_sample();
  }
}
