
void publishIpConfigToHa()
{
  char topic[128];
  char stat[128];
  char myMqttMsg[512];
  char uid[128];
  
  char myStatusMsg[600];
  char cUrl[25];

  DynamicJsonDocument doc(512);
          
  Serial.println("Adding IP Diagnostic Sensor...");
  
  sprintf(topic, "homeassistant/sensor/%sI/config", deviceName.c_str());
  sprintf(stat, "stat/%s/ipaddress", deviceName.c_str());
  sprintf(uid, "%sI", deviceName.c_str());

  //Create JSON payload per HA documentation
  doc.clear();
  doc["name"] = "IP Address";
  doc["uniq_id"] = uid;
  doc["ent_cat"] = "diagnostic";
  doc["stat_t"] = stat;
  JsonObject deviceI = doc.createNestedObject("device");
  deviceI = doc.createNestedObject("device");
  deviceI["ids"] = deviceName.c_str();
  deviceI["name"] = deviceName.c_str();
  deviceI["mf"] = "IoTT";
  sprintf(myStatusMsg, "%s", BBVersion.c_str());
  deviceI["sw"] = myStatusMsg;
  sprintf(cUrl, "http://%s", WiFi.localIP().toString().c_str());
  deviceI["cu"] = cUrl;
  serializeJson(doc, myMqttMsg);

  // Pubish IP sensor
  Serial.printf("Topic: %s\n", topic);  
  Serial.println(myMqttMsg);
  if (!lnMQTTServer->mqttPublish(topic, myMqttMsg, true)) {
    Serial.println("Failed MQTT send");
  }

}

void publishMacConfigToHa()
{
  char topic[128];
  char stat[128];
  char myMqttMsg[512];
  char uid[128];

  DynamicJsonDocument doc(512);

  Serial.println("Adding MAC Diagnostic Sensor...");

  sprintf(topic, "homeassistant/sensor/%sM/config", deviceName.c_str());
  sprintf(stat, "stat/%s/macaddress", deviceName.c_str());
  sprintf(uid, "%sM", deviceName.c_str());

  //Create JSON payload per HA documentation
  doc.clear();
  doc["name"] = "MAC Address";
  doc["uniq_id"] = uid;
  doc["ent_cat"] = "diagnostic";
  doc["stat_t"] = stat;
  JsonObject deviceI = doc.createNestedObject("device");
  deviceI = doc.createNestedObject("device");
  deviceI["ids"] = deviceName.c_str();
  deviceI["name"] = deviceName.c_str();
  serializeJson(doc, myMqttMsg);

  // Pubish MAC sensor
  Serial.printf("Topic: %s\n", topic);  
  Serial.println(myMqttMsg);
  if (!lnMQTTServer->mqttPublish(topic, myMqttMsg, true)) {
    Serial.println("Failed MQTT send");
  }

}

void updateHaValues()
{
  char topic[128];
  char msg[128];
  String strMacAddr = WiFi.macAddress();

  Serial.println("Updating HA device values");  

  // Publish IP sensor value
  sprintf(topic, "stat/%s/ipaddress", deviceName.c_str());
  sprintf(msg, "%s", WiFi.localIP().toString().c_str());
  Serial.printf("Topic: %s\n", topic);  
  Serial.println(msg);
  if (!lnMQTTServer->mqttPublish(topic, msg, true)) {
    Serial.println("Failed MQTT send");
  }

  // Publish MAC sensor value
  sprintf(topic, "stat/%s/macaddress", deviceName.c_str());
  sprintf(msg, "%s", WiFi.macAddress().c_str());
  Serial.printf("Topic: %s\n", topic);  
  Serial.println(msg);
  if (!lnMQTTServer->mqttPublish(topic, msg, true)) {
    Serial.println("Failed MQTT send");
  }
  
}

void publishHaDevice()
{
  // Send HA initial data
  if (enableHA)
  {
    Serial.println("Home Assistant Inegration on");
    publishIpConfigToHa();
    publishMacConfigToHa();
    updateHaValues();
  } else
    Serial.println("Home Assistant Inegration off");

}
