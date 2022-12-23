#include "doorbell.h"

/*
https://lastminuteengineers.com/esp32-deep-sleep-wakeup-sources/
https://lastminuteengineers.com/esp32-sleep-modes-power-consumption/

https://github.com/moxl-420/ESP32cam-doorbell
https://github.com/ldab/ESP32-CAM-MQTT

https://randomnerdtutorials.com/esp32-cam-ov2640-camera-settings/

https://diyi0t.com/best-battery-for-esp32/
*/

WiFiClient wificlient;
HTTPClient httpclient;

// You can save data in the ESP32’s RTC memory (16kB? SRAM) which is not erased during deep sleep. However, it is erased when the ESP32 is reset.
// To use the data after reboot, store it in RTC memory by defining a global variable with RTC_DATA_ATTR attribute.
RTC_DATA_ATTR int bootCount = 0;

// Time
const char* ntpServer = NTP_SERVER;
RTC_DATA_ATTR bool time_is_set = false;

#ifndef DISABLE_SERIAL_OUTPUT


/*
Method to print the reason by which ESP32
has been awaken from sleep
*/
void print_wakeup_reason(){
  esp_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_sleep_get_wakeup_cause();

  Sprint("Wakeup caused by ");
  switch(wakeup_reason)
  {
    case ESP_SLEEP_WAKEUP_EXT0 : Sprintln("external signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1 : {
      Sprintln("external signal using RTC_CNTL"); 
      uint64_t ext1_status = esp_sleep_get_ext1_wakeup_status();
      int gpio_pin = log(ext1_status)/log(2);
      Sprint("\twake on GPIO #"); Sprintln(gpio_pin);
      break; }
    case ESP_SLEEP_WAKEUP_TIMER : Sprintln("timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD : Sprintln("touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP : Sprintln("ULP program"); break;
    default : Sprintln(wakeup_reason); break;
  }
}
#endif


void get_ESP32_info(){
  #ifndef DISABLE_SERIAL_OUTPUT
  Serial.printf("ESP32 Chip model = %s Rev %d\n", ESP.getChipModel(), ESP.getChipRevision()); 
  Serial.printf("This chip has %d cores\n", ESP.getChipCores());

  // The chip ID is essentially its MAC address(length: 6 bytes)
  // unique deviceId, it contains the MAC address in reverse order
  uint64_t chipid = ESP.getEfuseMac(); 
  char deviceId[25];
  snprintf(deviceId, 23, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  Serial.print("Chip ID: "); Serial.println(deviceId);
  #endif
}


//Go to sleep now, wake up with touch interrupt
void deep_sleep()
{
  sleepFlash();

  // Configure one or more GPIO as wakeup source
  esp_sleep_enable_ext1_wakeup(BUTTON_PIN_BITMASK,ESP_EXT1_WAKEUP_ANY_HIGH);

  Sprintln("Awake for " + String( millis() ) + "ms. Back to sleep...");
  #ifndef DISABLE_SERIAL_OUTPUT
  Serial.flush();
  #endif

  // Powering off peripherals can not be done if using pull-up/down on GPIO
  //esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF); // 2.8mA

  esp_deep_sleep_start();
}


void onNetworkDisconnect() {
  Sprintln("Disconnected from network");
  deep_sleep();
}

/*
  https://developers.home-assistant.io/docs/api/rest/
*/

/*
  homeassistant/switch/doorbell/ringer ON
*/
/*
void notifyDoorbellMQTT(){
  Sprintln("RING! RING! [API->MQTT]");
  String url = String(HA_BASE_URL)+"/services/mqtt/publish";
  String payload = "{\"topic\":\"homeassistant/switch/doorbell/ringer\", \"payload\": \"ON\", \"retain\":\"False\"}";
  int rc = postJson(HA_ACCESS_TOKEN, url.c_str(), payload.c_str());
}
*/

/*
void notifyDoorbell(){
  Sprintln("RING! RING!");
  String url = String(HA_BASE_URL)+"/states/switch.doorbell_ringer";
  String payload = "{\"state\": \"on\", \"attributes\":{\"friendly_name\":\"Doorbell Ringer\", \"icon\":\"mdi:doorbell-video\"}}";
  int rc = postJson(HA_ACCESS_TOKEN, url.c_str(), payload.c_str());
}
*/

bool displayDoorbellSnapshot(String snapshot_filename){
  Sprintln("Updating doorbell snapshot...");  
  String url = String(HA_BASE_URL)+"/services/local_file/update_file_path";
  String payload = "{ \"entity_id\": \""+String(CAMERA_ENTITY_ID)+"\", \"file_path\": \""+String(MEDIA_DIRS_VAL)+"/"+snapshot_filename+"\"}";
  return postJson(HA_ACCESS_TOKEN, url.c_str(), payload.c_str());
}

/*
  Take a picture and upload it to Home Assistant via the media-browser.
  Location of snapshot determined by MEDIA_DIRS_KEY.  
*/
bool uploadDoorbellPicture(String snapshot_filename){
  if(initCamera()){
    // do not use flash since the flash LED cannot be fully turned off when in deep sleep
    initFlash(ENABLE_FLASH);

    // take picture    
    flashOn();
    camera_fb_t *cam_frame_buf = esp_camera_fb_get();
    flashOff();

    // send picture
    uint8_t* pic_buf = cam_frame_buf->buf;
    size_t length = cam_frame_buf->len;

    Sprintln("Uploading snapshot "+snapshot_filename);
    Sprint("Image size (bytes) = "); Sprintln(length);

    String url = String(HA_BASE_URL)+"/media_source/local_source/upload";
    return postBinary(HA_ACCESS_TOKEN, url.c_str(), MEDIA_DIRS_KEY, snapshot_filename, pic_buf, length);
  }
  else
  {
    return false;
  }
}

void onNetworkConnect(){
  
  char snapshot_filename[40];

  if(!time_is_set){
    Sprintln("Setting clock...");
    //configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);  
    // Just set UTC time since timezone info is lost during deep sleep (getLocalTime() was returning UTC)
    configTime(0, 0, ntpServer);  
  }

  // set timezone before querying time
  int rcode = setenv("TZ", TIMEZONE, 1);
  tzset();  

  struct tm timeinfo;
  if(getLocalTime(&timeinfo)){
    time_is_set = true;
    Sprintln(asctime(&timeinfo));
   
    strftime(snapshot_filename, 40, "doorbell_snapshot_%Y%m%dT%H%M%S.jpg", &timeinfo);    
    //snapshot_filename = "doorbell_snapshot_yyyymmddThhmmss.jpg";
  }
  else{
    strcpy(snapshot_filename, "doorbell_snapshot.jpg");
  }

  if(uploadDoorbellPicture(snapshot_filename)){
    if(!displayDoorbellSnapshot(snapshot_filename)){
      Sprintln("Failed to display latest snapshot after successful upload!");
    }
  }

  WiFi.disconnect(); // --> onNetworkDisconnect() --> deep_sleep()  
}

void WiFiEvent(WiFiEvent_t event) {
    Sprint("[WiFi-event] event: "); Sprintln(event);
    switch(event) {

    // 0  SYSTEM_EVENT_WIFI_READY = 0,           /*!< ESP32 WiFi ready */
    // 1  SYSTEM_EVENT_SCAN_DONE,                /*!< ESP32 finish scanning AP */
    // 2  SYSTEM_EVENT_STA_START,                /*!< ESP32 station start */
    // 3  SYSTEM_EVENT_STA_STOP,                 /*!< ESP32 station stop */
    // 4  SYSTEM_EVENT_STA_CONNECTED,            /*!< ESP32 station connected to AP */
    // 5  SYSTEM_EVENT_STA_DISCONNECTED,         /*!< ESP32 station disconnected from AP */
    // 6  SYSTEM_EVENT_STA_AUTHMODE_CHANGE,      /*!< the auth mode of AP connected by ESP32 station changed */
    // 7  SYSTEM_EVENT_STA_GOT_IP,               /*!< ESP32 station got IP from connected AP */
    // 8  SYSTEM_EVENT_STA_LOST_IP,              /*!< ESP32 station lost IP and the IP is reset to 0 */
    // 9  SYSTEM_EVENT_STA_BSS_RSSI_LOW,         /*!< ESP32 station connected BSS rssi goes below threshold */
    // 10 SYSTEM_EVENT_STA_WPS_ER_SUCCESS,       /*!< ESP32 station wps succeeds in enrollee mode */
    // 11 SYSTEM_EVENT_STA_WPS_ER_FAILED,        /*!< ESP32 station wps fails in enrollee mode */
    // 12 SYSTEM_EVENT_STA_WPS_ER_TIMEOUT,       /*!< ESP32 station wps timeout in enrollee mode */
    // 13 SYSTEM_EVENT_STA_WPS_ER_PIN,           /*!< ESP32 station wps pin code in enrollee mode */
    // 14 SYSTEM_EVENT_STA_WPS_ER_PBC_OVERLAP,   /*!< ESP32 station wps overlap in enrollee mode */
    // 15 SYSTEM_EVENT_AP_START,                 /*!< ESP32 soft-AP start */
    // 16 SYSTEM_EVENT_AP_STOP,                  /*!< ESP32 soft-AP stop */
    // 17 SYSTEM_EVENT_AP_STACONNECTED,          /*!< a station connected to ESP32 soft-AP */
    // 18 SYSTEM_EVENT_AP_STADISCONNECTED,       /*!< a station disconnected from ESP32 soft-AP */
    // 19 SYSTEM_EVENT_AP_STAIPASSIGNED,         /*!< ESP32 soft-AP assign an IP to a connected station */
    // 20 SYSTEM_EVENT_AP_PROBEREQRECVED,        /*!< Receive probe request packet in soft-AP interface */
    // 21 SYSTEM_EVENT_ACTION_TX_STATUS,         /*!< Receive status of Action frame transmitted */
    // 22 SYSTEM_EVENT_ROC_DONE,                 /*!< Indicates the completion of Remain-on-Channel operation status */
    // 23 SYSTEM_EVENT_STA_BEACON_TIMEOUT,       /*!< ESP32 station beacon timeout */
    // 24 SYSTEM_EVENT_FTM_REPORT,               /*!< Receive report of FTM procedure */
    // 25 SYSTEM_EVENT_GOT_IP6,                  /*!< ESP32 station or ap or ethernet interface v6IP addr is preferred */
    // 26 SYSTEM_EVENT_ETH_START,                /*!< ESP32 ethernet start */
    // 27 SYSTEM_EVENT_ETH_STOP,                 /*!< ESP32 ethernet stop */
    // 28 SYSTEM_EVENT_ETH_CONNECTED,            /*!< ESP32 ethernet phy link up */
    // 29 SYSTEM_EVENT_ETH_DISCONNECTED,         /*!< ESP32 ethernet phy link down */
    // 30 SYSTEM_EVENT_ETH_GOT_IP,               /*!< ESP32 ethernet got IP from connected AP */
    // 31 SYSTEM_EVENT_ETH_LOST_IP,              /*!< ESP32 ethernet lost IP and the IP is reset to 0 */
    // 32 SYSTEM_EVENT_MAX                       /*!< Number of members in this enum */
      
    case SYSTEM_EVENT_STA_GOT_IP:
        #ifndef DISABLE_SERIAL_OUTPUT
        printNetworkDetails();
        #endif
        onNetworkConnect();
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        onNetworkDisconnect();
        break;
    }
}


void setup(){

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector

  #ifndef DISABLE_SERIAL_OUTPUT
  Serial.begin(9600);
  //Serial.setDebugOutput(true);
  delay(100); //Take some time to open up the Serial Monitor
  #endif

  //get_ESP32_info();

  //Increment boot number and print it every reboot
  ++bootCount;
  Sprintln("Boot number: " + String(bootCount));

  #ifndef DISABLE_SERIAL_OUTPUT
  //Print the wakeup reason for ESP32 and touchpad too
  print_wakeup_reason();
  #endif
  
  WiFi.onEvent(WiFiEvent);

  connectWifi(LOCAL_ENV_WIFI_SSID, LOCAL_ENV_WIFI_PASSWORD); // --> WiFiEvent --> onNetworkConnect() --> [uploadDoorbellPicture() + displayDoorbellSnapshot() + Wifi.disconnect()] --> WiFiEvent --> onNetworkDisconnect() --> deep_sleep()

  // This line never reached
}

void loop(){
  // This method will never be called
}