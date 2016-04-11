/*
   This sketch shows how to use control car with MPU6050 6 axis gyro/accelerometer,
   and send out results via UDP

   This sketch requires library include Ameba version of I2Cdev and MPU6050

   We enable MPU (Motion Processing Tech) of MPU6050 that it can trigger interrupt with correspond sample rate.
   And then gather the data from fifo and convert it to yaw/pitch/poll values.
   We only need pitch and poll, convert it to car data format and send out via UDP.

 **/

#include <WiFi.h>
#include "I2Cdev.h"
#include "MPU6050_6Axis_MotionApps20.h"
#include "Wire.h"

/* WiFi related variables*/
char ssid[] = "mycar"; //  your network SSID (name)
char pass[] = "12345678";    // your network password (use for WPA, or use as key for WEP)
IPAddress server(192,168,1,1);  // numeric IP for mycar AP mode
int status = WL_IDLE_STATUS;

WiFiClient client;
char sendbuf[12];

/* MPU6050 related variables */
MPU6050 mpu;

// MPU control/status vars
bool dmpReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer

// orientation/motion vars
Quaternion q;           // [w, x, y, z]         quaternion container
VectorFloat gravity;    // [x, y, z]            gravity vector
float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector

/* car control variable */
int carx = 0;
int cary = 0;
int prev_carx = 0;
int prev_cary = 0;
uint32_t last_send_timestamp = 0;
uint32_t timestamp = 0;

volatile bool mpuInterrupt = false;     // indicates whether MPU interrupt pin has gone high
void dmpDataReady() {
    mpuInterrupt = true;
}

void setup() {
  checkAndReconnectServer();
  initMPU6050();
}

void loop() {
  if (!dmpReady) return; // if programming failed, don't try to do anything

  safeWaitMPU6050();
  mpuIntStatus = mpu.getIntStatus();
  fifoCount = mpu.getFIFOCount();

  if ((mpuIntStatus & 0x10) || fifoCount == 1024) {
    mpu.resetFIFO();
    Serial.println(F("FIFO overflow!"));
  } else if (mpuIntStatus & 0x02 && fifoCount >= packetSize) {
    while (fifoCount >= packetSize){
      mpu.getFIFOBytes(fifoBuffer, packetSize);
      fifoCount -= packetSize;
    }

    // display Euler angles in degrees
    mpu.dmpGetQuaternion(&q, fifoBuffer);
    mpu.dmpGetGravity(&gravity, &q);
    mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);

    mapPitchRolltoXY(ypr[1], ypr[2], &carx, &cary);

    memset(sendbuf, 0, 12);
    timestamp = millis();
    if ((timestamp - last_send_timestamp) > 500 || (carx != prev_carx && cary != prev_cary)) {
      sprintf(sendbuf, "X:%dY:%d", carx, cary);
    } else if (carx != prev_carx) {
      sprintf(sendbuf, "X:%d", carx);
    } else if (cary != prev_cary) {
      sprintf(sendbuf, "Y:%d", cary);
    }
    if (strlen(sendbuf) > 0) {
      rtl_printf("%s\r\n", sendbuf);
      if (checkAndReconnectServer()) {
        safeResetMPU6050();
      }
      client.write(sendbuf, strlen(sendbuf));
      last_send_timestamp = timestamp;
      delay(10);

      // ignore previous interrupt
      mpuInterrupt = false;
    }
    prev_carx = carx;
    prev_cary = cary;
  }
}

void mapPitchRolltoXY(float pitch, float roll, int *carx, int *cary) {
  pitch = pitch * 180 / M_PI;
  roll = roll * 180 / M_PI;
  if (pitch > 24) {
    pitch = 24;
  }
  if (pitch < -24) {
    pitch = -24;
  }
  if (roll > 24) {
    roll = 24;
  }
  if (roll < -24) {
    roll = -24;
  }

  if (pitch > 12) {
    *carx = (int)(pitch-12);
  } else if (pitch < -12) {
    *carx = (int)(pitch + 12);
  } else {
    *carx = 0;
  }
  *carx = -*carx;

  if (roll > 12) {
    *cary = (int)(roll-12);
  } else if (roll < -12) {
    *cary = (int)(roll+12);
  } else {
    *cary = 0;
  }
  *cary = -*cary;
}

int checkAndReconnectServer() {
  int ret = 0;
  status = WiFi.status();
  if (status != WL_CONNECTED) {
    while (status != WL_CONNECTED) {
      Serial.print("Attempting to connect to SSID: ");
      Serial.println(ssid);
      // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
      status = WiFi.begin(ssid, pass);
      if (status == WL_CONNECTED) {
        break;
      }
      Serial.println("Reconnect to wifi...");
      delay(1000);
    }
    Serial.println("Connected to wifi");
    ret = 1;
  }
  if ( !client.connected()) {
    while (!client.connect(server, 5001)) {
      Serial.println("reconnect to server...");
      delay(1000);
    }
    Serial.println("connected to server");  
    ret = 1;
  }
  return ret;
}

void initMPU6050() {
  Wire.begin();
  Wire.setClock(400000); // 400kHz I2C clock (200kHz if CPU is 8MHz).

  Serial.println(F("Initializing I2C devices..."));
  mpu.initialize();

  Serial.println(F("Testing device connections..."));
  Serial.println(mpu.testConnection() ? F("MPU6050 connection successful") : F("MPU6050 connection failed"));

  Serial.println(F("Initializing DMP..."));
  devStatus = mpu.dmpInitialize();

  // make sure it worked (returns 0 if so)
  if (devStatus == 0) {
    // turn on the DMP, now that it's ready
    Serial.println(F("Enabling DMP..."));
    mpu.setDMPEnabled(true);

    // enable Arduino interrupt detection
    Serial.println(F("Enabling interrupt detection (Ameba D16 pin)..."));
    attachInterrupt(16, dmpDataReady, RISING);
    mpuIntStatus = mpu.getIntStatus();

    // set our DMP Ready flag so the main loop() function knows it's okay to use it
    Serial.println(F("DMP ready! Waiting for first interrupt..."));
    dmpReady = true;

    // get expected DMP packet size for later comparison
    packetSize = mpu.dmpGetFIFOPacketSize();

    mpu.setRate(5); // 1khz / (1 + 5) = 166 Hz
  } else {
    // ERROR!
    // 1 = initial memory load failed
    // 2 = DMP configuration updates failed
    // (if it's going to break, usually the code will be 1)
    Serial.print(F("DMP Initialization failed (code "));
    Serial.print(devStatus);
    Serial.println(F(")"));
  }  
}

void safeWaitMPU6050() {
  while (!mpuInterrupt) {
    os_thread_yield(); // without yield, the empty busy loop might make CPU behave un-expected
  }  
  mpuInterrupt = false;
}

void safeResetMPU6050() {
  mpuInterrupt = false;
    while (!mpuInterrupt) {
    os_thread_yield(); // without yield, the empty busy loop might make CPU behave un-expected
  }
  mpu.resetFIFO();
  mpuInterrupt = false;
}