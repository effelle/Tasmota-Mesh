/*
  xdrv_56_tasmesh.ino - Mesh support for Tasmota using ESP-Now

  Copyright (C) 2020  Christian Baars, Federico Leoni and and Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

  --------------------------------------------------------------------------------------------
  Version yyyymmdd  Action    Description
  --------------------------------------------------------------------------------------------
  0.9.0.0 20200927  started - from scratch
  0.9.4.1 20210503  edit      add some minor tweak for channel management

*/


#ifdef USE_TASMESH

/*********************************************************************************************\
* Build a mesh of nodes using ESP-Now
* Connect it through an ESP32-broker to WLAN
\*********************************************************************************************/

#define XDRV_56             56

/*********************************************************************************************\
 * constants
\*********************************************************************************************/

const char S_JSON_MESH_COMMAND_NVALUE[] PROGMEM = "{\"" D_CMND_MESH "%s\":%d}";
const char S_JSON_MESH_COMMAND[] PROGMEM        = "{\"" D_CMND_MESH "%s\"}";
const char kMESH_Commands[] PROGMEM             = "Broker|Node|Peer|Channel";

/*********************************************************************************************\
 * Callbacks
\*********************************************************************************************/

#ifdef ESP32
void CB_MESHDataSent(const uint8_t *MAC,  esp_now_send_status_t sendStatus);
void CB_MESHDataSent(const uint8_t *MAC,  esp_now_send_status_t sendStatus) {
  char _destMAC[18];
  ToHex_P(MAC,6,_destMAC,18,':');
  AddLog_P(LOG_LEVEL_DEBUG, PSTR(">>> %s"),_destMAC);
}

void CB_MESHDataReceived(const uint8_t *MAC, const uint8_t *packet, int len) {
  static bool _locked = false;
  if(_locked) return;
  _locked = true;
  char _srcMAC[18];
  ToHex_P(MAC,6,_srcMAC,18,':');
  AddLog_P(LOG_LEVEL_DEBUG, PSTR("<<< %s"),_srcMAC);
  mesh_packet_t *_recvPacket = (mesh_packet_t*)packet;
  if(_recvPacket->type == PACKET_TYPE_REGISTER_NODE || _recvPacket->type == PACKET_TYPE_REFRESH_NODE ){
    if(MESHcheckPeerList((const uint8_t *)MAC) == false){
      MESHencryptPayload(_recvPacket,0); //decrypt it and check
      if(memcmp(_recvPacket->payload,MESH.broker,6) == 0){
        MESHaddPeer((uint8_t*)MAC);
        AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: received topic: %s"), (char*)_recvPacket->payload + 6);
        // AddLogBuffer(LOG_LEVEL_INFO,(uint8_t *)&MESH.packetToConsume.front().payload,MESH.packetToConsume.front().chunkSize+5);
        for(auto &_peer : MESH.peers){
          if(memcmp(_peer.MAC,_recvPacket->sender,6)==0){
            strcpy(_peer.topic,(char*)_recvPacket->payload + 6);
            MESHsubscribe((char*)&_peer.topic);
            _locked = false;
            return;
          }
        }
      }
      else {
        AddLog_P(LOG_LEVEL_DEBUG, PSTR("peer %s denied !!!"),_srcMAC);
        char _cryptMAC[18];
        ToHex_P(_recvPacket->payload,6,_cryptMAC,18,':');
        AddLog_P(LOG_LEVEL_DEBUG, PSTR("wrong MAC: %s"),_cryptMAC);
        _locked = false;
        return;
      }
    }
    else {
      if(_recvPacket->type == PACKET_TYPE_REGISTER_NODE){
        MESH.flags.nodeWantsTimeASAP = 1; //this could happen after wake from deepsleep on battery powered device
      }
      else MESH.flags.nodeWantsTime = 1;
    }
  }
  MESH.lmfap = millis();
  if (MESHcheckPeerList(MAC) == true){
    AddLog_P(LOG_LEVEL_DEBUG, PSTR("packet from %s to queue"),_srcMAC);
    MESH.packetToConsume.push(*_recvPacket);
  }
  _locked = false;
}

#else //ESP8266
void CB_MESHDataSent( uint8_t *MAC, uint8_t sendStatus) {
  char _destMAC[18];
  ToHex_P(MAC,6,_destMAC,18,':');
  AddLog_P(LOG_LEVEL_DEBUG, PSTR(">>> %s"),_destMAC);
}

void CB_MESHDataReceived(uint8_t *MAC, uint8_t *packet, uint8_t len) {
      MESH.lmfap = millis(); //any peer
      if(memcmp(MAC,MESH.broker,6)==0) MESH.lastMessageFromBroker = millis(); //directly from the broker
      mesh_packet_t *_recvPacket = (mesh_packet_t*)packet;
      switch(_recvPacket->type){
        case PACKET_TYPE_TIME:
          Rtc.utc_time = _recvPacket->senderTime;
          Rtc.user_time_entry = true;
          MESH.lastMessageFromBroker = millis();
          if(MESH.flags.nodeGotTime == 0){
            RtcSync();
            TasmotaGlobal.rules_flag.system_boot  = 1; // for now we consider the node booted and let trigger system#boot on RULES
          }
          MESH.flags.nodeGotTime = 1;
          //Wifi.retry = 0;
          // Response_P(PSTR("{\"%s\":{\"Time\":1}}"), D_CMND_MESH); //got the time, now we can publish some sensor data
          // XdrvRulesProcess();
          break;
        case PACKET_TYPE_PEERLIST:
          MESH.packetToConsume.push(*_recvPacket);
          return;
          break;
        default:
          // nothing for now;
          break;
      }
      if(memcmp(_recvPacket->receiver,MESH.sendPacket.sender,6)!=0){ //MESH.sendPacket.sender simply stores the MAC of the node
        if(MESH.role == ROLE_NODE_SMALL) return; // a 'small node' does not perform mesh functions
        AddLog_P(LOG_LEVEL_DEBUG, PSTR("packet to resend ..."));
        MESH.packetToResend.push(*_recvPacket);
        return;
      }
      else{
        if(_recvPacket->type == PACKET_TYPE_WANTTOPIC){
          MESH.flags.brokerNeedsTopic = 1;
          AddLog_P(LOG_LEVEL_DEBUG, PSTR("broker needs topic ..."));
          return; //nothing left to be done
        }
        // for(auto &_message : MESH.packetsAlreadyReceived){
        //   if(memcmp(_recvPacket,_message,15==0)){
        //     AddLog_P(LOG_LEVEL_INFO, PSTR("packet already received"));
        //     return;
        //   }
        // }
        // MESH.packetsAlreadyReceived.push_back((mesh_packet_header_t*) _recvPacket);
        // AddLog_P(LOG_LEVEL_DEBUG, PSTR("packet to consume ..."));
        MESH.packetToConsume.push(*_recvPacket);
      }

}
#endif //ESP32

/*********************************************************************************************\
 * init driver
\*********************************************************************************************/

void MESHInit(void) {
  MESH.role == ROLE_NONE;
  AddLog_P(LOG_LEVEL_INFO, PSTR("TAS-MESH initialized: %u"),Settings.tele_period);
  MESH.packetsAlreadyReceived.reserve(5);
  MESH.peers.reserve(10);
  MESH.multiPackets.reserve(2);

  MESH.sendPacket.counter = 0;
  MESH.sendPacket.chunks = 1;
  MESH.sendPacket.chunk = 0;
  MESH.sendPacket.type = PACKET_TYPE_TIME;
  MESH.sendPacket.TTL = 2;
}

void MESHdeInit(){
#ifdef ESP8266 // only ESP8266, ESP32 as a broker should not use deepsleep
  AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: stopping"));
  // TODO: degister from the broker, so he can stop MQTT-proxy
  esp_now_deinit();
#endif //ESP8266
}

/*********************************************************************************************\
 * MQTT proxy functions
\*********************************************************************************************/
#ifdef ESP32
/**
 * @brief Subscribes as a proxy
 *
 * @param topic - received from the referring node
 */
void MESHsubscribe(char *topic){
  char stopic[TOPSZ];
  GetTopic_P(stopic, CMND, topic, PSTR("#"));
  MqttSubscribe(stopic);
}

void MESHunsubscribe(char *topic){
  char stopic[TOPSZ];
  GetTopic_P(stopic, CMND, topic, PSTR("#"));
  MqttUnsubscribe(stopic);
}

void MESHconnectMQTT(void){
  for(auto &_peer : MESH.peers){
    AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: reconnect topic: %s"),_peer.topic);
    if(_peer.topic[0]!=0){
      MESHsubscribe(_peer.topic);
    }
  }
}

/**
 * @brief Intercepts mqtt message, that the broker (ESP32) subscribes to as a proxy for a node.
 *        Is called from xdrv_02_mqtt.ino. Will send the message in the payload via ESP-NOW.
 *
 * @param _topic
 * @param _data
 * @param data_len
 * @return true
 * @return false
 */
bool MESHinterceptMQTTonBroker(char* _topic, uint8_t* _data, unsigned int data_len){
  char stopic[TOPSZ];
  // AddLog_P(LOG_LEVEL_DEBUG, PSTR("MESH: Intercept topic: %s"),_topic);
  for(auto &_peer : MESH.peers){
    GetTopic_P(stopic, CMND, _peer.topic, PSTR("")); //cmnd/topic/
    if(strlen(_topic)!= strlen(_topic)) return false; // prevent false result when _topic is the leading substring of stopic
    if(memcmp(_topic, stopic,strlen(stopic)) == 0){
      MESH.sendPacket.chunkSize = strlen(_topic)+1;
      memcpy(MESH.sendPacket.receiver,_peer.MAC,6);
      memcpy(MESH.sendPacket.payload,_topic,MESH.sendPacket.chunkSize);
      memcpy(MESH.sendPacket.payload+MESH.sendPacket.chunkSize,_data,data_len);
      MESH.sendPacket.chunkSize += data_len;
      MESH.sendPacket.chunks = 1;
      AddLog_P(LOG_LEVEL_DEBUG, PSTR("MESH: Intercept payload: %s"),MESH.sendPacket.payload);
      MESH.sendPacket.type = PACKET_TYPE_MQTT;
      MESH.sendPacket.senderTime = Rtc.utc_time;
      MESHsendPacket(&MESH.sendPacket);
      // int result = esp_now_send(MESH.sendPacket.receiver, (uint8_t *)&MESH.sendPacket, (sizeof(MESH.sendPacket))-(MESH_PAYLOAD_SIZE-MESH.sendPacket.chunkSize));
      //send to Node
      return true;
    }
  }
  return false;
}

#else //ESP8266
void MESHreceiveMQTT(mesh_packet_t *_packet);
void MESHreceiveMQTT(mesh_packet_t *_packet){
  uint32_t _slength = strlen((char*)_packet->payload);
  if(_packet->chunks==1){ //single chunk message
    MqttDataHandler((char*)_packet->payload, (uint8_t*)(_packet->payload)+_slength+1, (_packet->chunkSize)-_slength);
  }
  else{
    AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: multiple chunks: %u"),_packet->chunks);
    // TODO: reconstruct message in buffer or only handle short messages
  }
}
#endif //ESP32

/**
 * @brief Redirects the mqtt message on the node just before it would have been sended to 
 *        the broker via ESP-NOW
 *
 * @param _topic
 * @param _data
 * @param _retained - currently unused
 * @return true
 * @return false
 */
bool MESHrouteMQTTtoMESH(const char* _topic, char* _data, bool _retained){
  size_t _bytesLeft = strlen(_topic)+strlen(_data)+2;
  MESH.sendPacket.counter++;
  MESH.sendPacket.chunk = 0;
  MESH.sendPacket.chunks = ((_bytesLeft+2)/MESH_PAYLOAD_SIZE)+1;
  memcpy(MESH.sendPacket.receiver,MESH.broker,6);
  MESH.sendPacket.type = PACKET_TYPE_MQTT;
  MESH.sendPacket.chunkSize = MESH_PAYLOAD_SIZE;
  MESH.sendPacket.peerIndex = 0;
  // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: chunks: %u, counter: %u"),MESH.sendPacket.chunks,MESH.sendPacket.counter);
  size_t _topicSize = strlen(_topic)+1;
  size_t _offsetData = 0;
  while(_bytesLeft>0){
    size_t _byteLeftInChunk = MESH_PAYLOAD_SIZE;
    // MESH.sendPacket.chunkSize = MESH_PAYLOAD_SIZE;
    if(MESH.sendPacket.chunk == 0){
      memcpy(MESH.sendPacket.payload,_topic,_topicSize);
      MESH.sendPacket.chunkSize = _topicSize;
      _bytesLeft -= _topicSize;
      _byteLeftInChunk -= _topicSize;
      // AddLog_P(LOG_LEVEL_INFO, PSTR("topic in payload %s"),(char*)MESH.sendPacket.payload);
      // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: after topic -> chunk:%u, pre-size: %u"),MESH.sendPacket.chunk,MESH.sendPacket.chunkSize);
    }
    if(_byteLeftInChunk>0){
      if(_byteLeftInChunk>_bytesLeft){
      //  AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: only last chunk bL:%u bLiC:%u oSD:%u"),_bytesLeft,_byteLeftInChunk,_offsetData);
        _byteLeftInChunk = _bytesLeft;
        // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: only last chunk after correction -> chunk:%u, pre-size: %u"),MESH.sendPacket.chunk,MESH.sendPacket.chunkSize);
      }
      if(MESH.sendPacket.chunk>0) _topicSize = 0;
      // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: %u"),_offsetPayload);
      memcpy(MESH.sendPacket.payload + _topicSize, _data + _offsetData,_byteLeftInChunk);
      // AddLog_P(LOG_LEVEL_INFO, PSTR("data in payload %s"),(char*)MESH.sendPacket.payload + _offsetPayload);
      _offsetData += _byteLeftInChunk;
      _bytesLeft -= _byteLeftInChunk;
    }
    MESH.sendPacket.chunkSize += _byteLeftInChunk;
    MESH.packetToResend.push(MESH.sendPacket);
    // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: chunk:%u, size: %u"),MESH.sendPacket.chunk,MESH.sendPacket.chunkSize);
    // AddLogBuffer(LOG_LEVEL_INFO, (uint8_t*)MESH.sendPacket.payload, MESH.sendPacket.chunkSize);

    if(MESH.sendPacket.chunk==MESH.sendPacket.chunks){
      // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: too many chunks: %u"),MESH.sendPacket.chunk+1);
    }
    MESH.sendPacket.chunk++;
    MESH.sendPacket.chunkSize = 0;
  }
  //send to pipeline
  return true;
}

/**
 * @brief The node sends its mqtt topic to the broker
 *
 */
void MESHregisterNode(uint8_t mode){
  memcpy(MESH.sendPacket.receiver,MESH.broker,6); // first 6 bytes -> MAC of broker
  strcpy((char*)MESH.sendPacket.payload+6,TasmotaGlobal.mqtt_topic); // remaining bytes -> topic of node
  AddLog_P(LOG_LEVEL_DEBUG, PSTR("MESH: register node with topic: %s"),(char*)MESH.sendPacket.payload+6);
  MESH.sendPacket.TTL = 2;
  MESH.sendPacket.chunks = 1;
  MESH.sendPacket.chunk = 0;
  MESH.sendPacket.chunkSize = strlen(TasmotaGlobal.mqtt_topic) + 1 + 6;
  memcpy(MESH.sendPacket.payload,MESH.broker,6);
  if(mode==0) MESH.sendPacket.type = PACKET_TYPE_REGISTER_NODE;
  else MESH.sendPacket.type = PACKET_TYPE_REFRESH_NODE;
  MESH.sendPacket.type = PACKET_TYPE_REGISTER_NODE;
  MESHsendPacket(&MESH.sendPacket);
  // int result = esp_now_send(MESH.sendPacket.receiver, (uint8_t *)&MESH.sendPacket, (sizeof(MESH.sendPacket))-(MESH_PAYLOAD_SIZE-MESH.sendPacket.chunkSize-1));
}

/*********************************************************************************************\
 * generic functions
\*********************************************************************************************/

void MESHstartNode(int32_t _channel, uint8_t _role){ //we need a running broker with a known channel at that moment
#ifdef ESP8266 // for now only ESP8266, might be added for the ESP32 later
  MESH.channel = _channel;
  WiFi.mode(WIFI_STA);
  WiFi.begin("","",MESH.channel, nullptr, false); //fake connection attempt to set channel
  wifi_promiscuous_enable(1);
  wifi_set_channel(MESH.channel);
  wifi_promiscuous_enable(0);
  WiFi.disconnect();
  Settings.flag4.network_wifi = 0; // the "old" wifi off command
  TasmotaGlobal.global_state.wifi_down = 1;
  if (esp_now_init() != 0) {
    return;
  }
  // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: Node initialized, channel: %u"),wifi_get_channel()); //check if we succesfully set the
  Response_P(PSTR("{\"%s\":{\"Node\":1,\"Channel\":%u,\"Role\":%u}}"), D_CMND_MESH, wifi_get_channel(), _role);
  XdrvRulesProcess(0);

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(CB_MESHDataSent);
  esp_now_register_recv_cb(CB_MESHDataReceived);

  MESHsetKey(MESH.key);
  memcpy(MESH.sendPacket.receiver,MESH.broker,6);
  WiFi.macAddress(MESH.sendPacket.sender);
  MESHaddPeer(MESH.broker); //must always be peer 0!! -return code -7 for peer list full
  MESHcountPeers();
  if(_role == 0){
    MESH.role = ROLE_NODE_SMALL;
  }
  else {
    MESH.role = ROLE_NODE_FULL;
  }
  MESHregisterNode(0);
#endif //ESP8266
}

void MESHstartBroker(){ // must be called after WiFi is initialized!! Rule - on system#boot do meshbroker endon
#ifdef ESP32
  WiFi.mode(WIFI_AP_STA);
  AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: Broker MAC: %s"),WiFi.softAPmacAddress().c_str());
  WiFi.softAPmacAddress(MESH.broker); //set MESH.broker to the needed MAC
  uint32_t _channel = WiFi.channel();

  if (esp_now_init() != 0) {
    return;
  }
  Response_P(PSTR("{\"%s\":{\"Broker\":1,\"Channel\":%u}}"), D_CMND_MESH, _channel);
  XdrvRulesProcess(0);
  // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: Broker initialized on channel: %u"), _channel);
  esp_now_register_send_cb(CB_MESHDataSent);
  esp_now_register_recv_cb(CB_MESHDataReceived);
  MESHsetKey(MESH.key);
  MESHcountPeers();
  memcpy(MESH.sendPacket.sender,MESH.broker,6);
  MESH.role = ROLE_BROKER;
#endif //ESP32
}

/*********************************************************************************************\
 * main loops
\*********************************************************************************************/
#ifdef ESP32

void MESHevery50MSecond(){
  // if(MESH.packetToResend.size()>0){
  //   // pass the packets
  // }
  if(MESH.packetToConsume.size()>0){
    // AddLog_P(LOG_LEVEL_DEBUG, PSTR("_"));
    // AddLogBuffer(LOG_LEVEL_DEBUG,(uint8_t *)&MESH.packetToConsume.front(), 15);
    for(auto &_headerBytes : MESH.packetsAlreadyReceived){
      // AddLog_P(LOG_LEVEL_DEBUG, PSTR("."));
      // AddLogBuffer(LOG_LEVEL_DEBUG,(uint8_t *)_headerBytes.raw, 15);
      if(memcmp(MESH.packetToConsume.front().sender,_headerBytes.raw,15)==0){
        MESH.packetToConsume.pop();
        return;
      }
  }
  mesh_first_header_bytes _bytes;
  memcpy(_bytes.raw,&MESH.packetToConsume.front(),15);
  MESH.packetsAlreadyReceived.push_back(_bytes);
  // AddLog_P(LOG_LEVEL_DEBUG, PSTR("..."));
  // AddLogBuffer(LOG_LEVEL_DEBUG,(uint8_t *)_bytes.raw, 15);

  if(MESH.packetsAlreadyReceived.size()>3) MESH.packetsAlreadyReceived.erase(MESH.packetsAlreadyReceived.begin());

    // do something on the node
    // AddLogBuffer(LOG_LEVEL_DEBUG,(uint8_t *)&MESH.packetToConsume.front(), 30);

    MESHencryptPayload(&MESH.packetToConsume.front(),0);
    switch(MESH.packetToConsume.front().type){
      // case PACKET_TYPE_REGISTER_NODE:
      //   AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: received topic: %s"), (char*)MESH.packetToConsume.front().payload + 6);
      //   // AddLogBuffer(LOG_LEVEL_INFO,(uint8_t *)&MESH.packetToConsume.front().payload,MESH.packetToConsume.front().chunkSize+5);
      //   for(auto &_peer : MESH.peers){
      //     if(memcmp(_peer.MAC,MESH.packetToConsume.front().sender,6)==0){
      //       strcpy(_peer.topic,(char*)MESH.packetToConsume.front().payload+6);
      //       MESHsubscribe((char*)&_peer.topic);
      //     }
      //   }
      //   break;
      case PACKET_TYPE_PEERLIST:
        for(uint32_t i=0;i<MESH.packetToConsume.front().chunkSize;i+=6){
          if(memcmp(MESH.packetToConsume.front().payload+i,MESH.sendPacket.sender,6)==0) continue; //do not add myself
          if(MESHcheckPeerList(MESH.packetToConsume.front().payload+i) == false) MESHaddPeer(MESH.packetToConsume.front().payload+i);
        }
        break;
      case  PACKET_TYPE_MQTT: // redirected MQTT from node in packet [char* _space_ char*]
        {
        // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: received node output: %s"), (char*)MESH.packetToConsume.front().payload);
        if(MESH.packetToConsume.front().chunks>1){
          bool _foundMultiPacket = false;
          for(auto &_packet_combined : MESH.multiPackets){
            // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: append to multipacket"));
            if(memcmp(_packet_combined.header.sender,MESH.packetToConsume.front().sender,12)==0){
              if(_packet_combined.header.counter == MESH.packetToConsume.front().counter){
                memcpy(_packet_combined.raw+(MESH.packetToConsume.front().chunk * MESH_PAYLOAD_SIZE),MESH.packetToConsume.front().payload,MESH.packetToConsume.front().chunkSize);
                bitSet(_packet_combined.receivedChunks,MESH.packetToConsume.front().chunk);
                _foundMultiPacket = true;
                // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: recChunks= %u"),_packet_combined.receivedChunks);
              }
            }
            uint32_t _temp = (1 << (uint8_t)MESH.packetToConsume.front().chunks)-1 ; //example: 1+2+4 == (2^3)-1
            // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: _temp: %u = %u"),_temp,_packet_combined.receivedChunks);
            if(_packet_combined.receivedChunks==_temp){
              char * _data = (char*)_packet_combined.raw + strlen((char*)_packet_combined.raw) + 1;
              MqttClient.publish((char*)_packet_combined.raw, _data);
              AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: combined done: %s = %s"),(char*)_packet_combined.raw,_data);
              // AddLogBuffer(LOG_LEVEL_INFO,(uint8_t*)_packet_combined.raw,50);
            }
          }
          if(!_foundMultiPacket){
            mesh_packet_combined_t _packet;
            memcpy(_packet.header.sender,MESH.packetToConsume.front().sender,sizeof(_packet.header));
            memcpy(_packet.raw+(MESH.packetToConsume.front().chunk*MESH_PAYLOAD_SIZE),MESH.packetToConsume.front().payload,MESH.packetToConsume.front().chunkSize);
            _packet.receivedChunks = 0;
            bitSet(_packet.receivedChunks,MESH.packetToConsume.front().chunk);
            MESH.multiPackets.push_back(_packet);
            // AddLog_P(LOG_LEVEL_INFO, PSTR("new multipacket with chunks: %u"),_packet.header.chunks);
          }
          // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: no support yet for multiple chunks: %u"),MESH.packetToConsume.front().chunks);
          break;
        }
        // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: chunk: %u size: %u"), MESH.packetToConsume.front().chunk, MESH.packetToConsume.front().chunkSize);
        // if (MESH.packetToConsume.front().chunk==0) AddLogBuffer(LOG_LEVEL_INFO,(uint8_t *)&MESH.packetToConsume.front().payload,MESH.packetToConsume.front().chunkSize);
        char * _data = (char*)MESH.packetToConsume.front().payload + strlen((char*)MESH.packetToConsume.front().payload)+1;
        MqttClient.publish((char*)MESH.packetToConsume.front().payload, _data);
        AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: topic: %s output: %s"), (char*)MESH.packetToConsume.front().payload, _data);
        uint32_t idx = 0;
        for(auto &_peer : MESH.peers){
          if(memcmp(_peer.MAC,MESH.packetToConsume.front().sender,6)==0){
            _peer.lastMessageFromPeer = millis();
            MESH.lastTeleMsgs[idx]  = std::string(_data);
            break;
          }
          idx++;
        }
        // AddLogBuffer(LOG_LEVEL_INFO,(uint8_t *)&MESH.packetToConsume.front().payload,MESH.packetToConsume.front().chunkSize);
        yield();  // #3313
        }
        break;
      default:
        AddLogBuffer(LOG_LEVEL_INFO,(uint8_t *)&MESH.packetToConsume.front(),MESH.packetToConsume.front().chunkSize+5);
      break;
    }
    MESH.packetToConsume.pop();
    // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: consumed one packet: %u"), (char*)MESH.packetToConsume.size());
  }
}

void MESHEverySecond(){
  static uint32_t _second = 0;
  _second++;
  // send a time packet every x seconds
  if (MESH.flags.nodeWantsTimeASAP){
    MESHsendTime();
    MESH.flags.nodeWantsTimeASAP = 0;
    return;
  }
  if(_second%5==0) {
    if(MESH.flags.nodeWantsTime == 1 || _second%30==0){ //every 5 seconds on demand or every 30 seconds anyway
      MESHsendTime();
      MESH.flags.nodeWantsTime = 0;
      return;
    }
  }
  uint32_t _peerNumber = _second%45;
  if(_peerNumber<MESH.peers.size()) {
    if(MESH.peers[_peerNumber].topic[0]==0){
      AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: broker wants topic from peer: %u"), _peerNumber);
      MESHdemandTopic(_peerNumber);
    }
  }
  if(MESH.multiPackets.size()>3){
    AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: multi packets in buffer: %u"),MESH.multiPackets.size());
    MESH.multiPackets.erase(MESH.multiPackets.begin());
  }
}

#else //ESP8266
void MESHevery50MSecond(){
  if(MESH.packetToResend.size()>0){
    AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: next packet to resend of type: %u, TTL: %u"),MESH.packetToResend.front().type,MESH.packetToResend.front().TTL);
    if (MESH.packetToResend.front().TTL>0){
      MESH.packetToResend.front().TTL--;
      if(memcmp(MESH.packetToResend.front().sender,MESH.broker,6) != 0){ //do not send back the packet to the broker
        MESHsendPacket(&MESH.packetToResend.front());
      }
    }
    else{
      MESH.packetToResend.pop();
    }
    // pass the packets
  }

  if(MESH.packetToConsume.size()>0){
    MESHencryptPayload(&MESH.packetToConsume.front(),0);
    switch(MESH.packetToConsume.front().type){
      case PACKET_TYPE_MQTT:
        if(memcmp(MESH.packetToConsume.front().sender,MESH.sendPacket.sender,6)==0){
          //discard echo
          break;
        }
        // AddLog_P(LOG_LEVEL_INFO, PSTR("MESH: node received topic: %s"), (char*)MESH.packetToConsume.front().payload);
        MESHreceiveMQTT(&MESH.packetToConsume.front());
        break;
      case PACKET_TYPE_PEERLIST:
        for(uint32_t i=0;i<MESH.packetToConsume.front().chunkSize;i+=6){
          if(memcmp(MESH.packetToConsume.front().payload+i,MESH.sendPacket.sender,6)==0) continue; //do not add myself
          if(MESHcheckPeerList(MESH.packetToConsume.front().payload+i) == false) MESHaddPeer(MESH.packetToConsume.front().payload+i);
        }
        break;
      default:
        break;
    }
    MESH.packetToConsume.pop();
  }
}


void MESHEverySecond(){
  if (MESH.role > ROLE_BROKER){
    if(MESH.flags.brokerNeedsTopic == 1){
      AddLog_P(LOG_LEVEL_DEBUG, PSTR("broker wants topic"));
      MESHregisterNode(1); //refresh info
      MESH.flags.brokerNeedsTopic = 0;
    }
    if(millis()-MESH.lastMessageFromBroker>31000){
      AddLog_P(LOG_LEVEL_DEBUG, PSTR("broker not seen for >30 secs"));
      MESHregisterNode(1); //refresh info
    }
    if(millis()-MESH.lastMessageFromBroker>70000){
      AddLog_P(LOG_LEVEL_DEBUG, PSTR("broker not seen for 70 secs, try to re-launch wifi"));
      MESH.role = ROLE_NONE;
      Settings.flag4.network_wifi = 1; // the "old" wifi on command -> reconnect
      TasmotaGlobal.global_state.wifi_down = 0;
      WifiBegin(3, MESH.channel);
    }
  }
}
#endif //ESP8266

/*********************************************************************************************\
 * presentation
\*********************************************************************************************/
void MESHshow(bool json){
  if (json) {
  if(MESH.role == ROLE_BROKER){
    ResponseAppend_P(PSTR(",\"MESH\":{\"channel\":%u"),MESH.channel);
    ResponseAppend_P(PSTR(",\"nodes\":%u"),MESH.peers.size());
    if(MESH.peers.size()>0){
      ResponseAppend_P(PSTR(",\"MAC\":["));
      for(auto &_peer : MESH.peers){
        char _MAC[18];
        ToHex_P(_peer.MAC,6,_MAC,18,':');
        ResponseAppend_P(PSTR("\"%s\","),_MAC);
      }
      TasmotaGlobal.mqtt_data[strlen(TasmotaGlobal.mqtt_data)-1] = 0; // delete last ','
      ResponseAppend_P(PSTR("]"));
    }
    ResponseJsonEnd();
    }
  } else {
#ifdef ESP32 //web UI only on the the broker = ESP32
    if(MESH.role == ROLE_BROKER){
      WSContentSend_PD(PSTR("TAS-MESH:<br>"));
      WSContentSend_PD(PSTR("Broker MAC: %s <br>"),WiFi.softAPmacAddress().c_str());
      WSContentSend_PD(PSTR("Broker Channel: %u <hr>"),WiFi.channel());
      uint32_t idx = 0;
      for(auto &_peer : MESH.peers){
        char _MAC[18];
        ToHex_P(_peer.MAC,6,_MAC,18,':');
        WSContentSend_PD(PSTR("Node MAC: %s <br>"),_MAC);
        WSContentSend_PD(PSTR("Node last message: %u msecs ago<br>"),millis()-_peer.lastMessageFromPeer);
        WSContentSend_PD(PSTR("Node MQTT topic: %s <br>"),_peer.topic);
        if(MESH.lastTeleMsgs.size()>idx){
          char json_buffer[MESH.lastTeleMsgs[idx].length()+1];
          strcpy(json_buffer,(char*)MESH.lastTeleMsgs[idx].c_str());
          JsonParser parser(json_buffer);
          JsonParserObject root = parser.getRootObject();
          for (auto key : root) {
            JsonParserObject subObj = key.getValue().getObject();
            if(subObj){
              WSContentSend_PD(PSTR("<ul>%s:"),key.getStr());
              for (auto subkey : subObj) {
                WSContentSend_PD(PSTR("<ul>%s: %s</ul>"),subkey.getStr(), subkey.getValue().getStr());
              }
              WSContentSend_PD(PSTR("</ul>"));
            }
            else{
              WSContentSend_PD(PSTR("<ul>%s: %s</ul>"),key.getStr(), key.getValue().getStr());
            }
          }
          AddLog_P(LOG_LEVEL_INFO,PSTR("teleJSON: %s"),(char*)MESH.lastTeleMsgs[idx].c_str());
          // AddLog_P(LOG_LEVEL_INFO,PSTR("stringsize: %u"),MESH.lastTeleMsgs[idx].length());
        }
        else {
          // AddLog_P(LOG_LEVEL_INFO,PSTR("telemsgSize: %u"),MESH.lastTeleMsgs.size());
        }
        WSContentSend_PD(PSTR("<hr>"));
        idx++;
      }
    }
#endif //ESP32
  }
}

/*********************************************************************************************\
 * check the MESH commands
\*********************************************************************************************/

bool MESHCmd(void) {
  char command[CMDSZ];
  bool serviced = true;
  uint8_t disp_len = strlen(D_CMND_MESH);

  if (!strncasecmp_P(XdrvMailbox.topic, PSTR(D_CMND_MESH), disp_len)) {  // prefix
    int command_code = GetCommandCode(command, sizeof(command), XdrvMailbox.topic + disp_len, kMESH_Commands);

    switch (command_code) {
      case CMND_MESH_BROKER:
        MESH.channel = WiFi.channel(); // The Broker gets the channel from the router, no need to declare it with MESHCHANNEL (will be mandatory set it when ETH will be implemented)
        MESHstartBroker();
        Response_P(S_JSON_MESH_COMMAND_NVALUE, command, MESH.channel);
        break; 
      case CMND_MESH_NODE:
        if (XdrvMailbox.data_len > 0) {
          MESHHexStringToBytes(XdrvMailbox.data, MESH.broker);
          if(XdrvMailbox.index != 0) XdrvMailbox.index = 1;     // Everything not 0 is a full node
          // meshnode FA:KE:AD:DR:ES:S1
          bool broker = false;
          char EspSsid[11];
          String mac_address = XdrvMailbox.data;
          snprintf_P(EspSsid, sizeof(EspSsid), PSTR("ESP_%s"), mac_address.substring(6).c_str());
          int32_t getWiFiChannel(const char *EspSsid);
          if (int32_t ch = WiFi.scanNetworks()) {
            for (uint8_t i = 0; i < ch; i++) {
              if (!strcmp(EspSsid, WiFi.SSID(i).c_str())) {     
                MESH.channel = WiFi.channel(i);
                broker = true;
                AddLog_P(LOG_LEVEL_INFO, PSTR("MES: Successfully connected to Mesh Broker using MAC: %s as %s on channel %d"), XdrvMailbox.data, EspSsid, MESH.channel);
                MESHstartNode(MESH.channel, XdrvMailbox.index);
                Response_P(S_JSON_MESH_COMMAND_NVALUE, command, MESH.channel);
              } 
            }
          }
          if (!broker) {
            AddLog_P(LOG_LEVEL_INFO, PSTR("MES: No Mesh Broker found using MAC %s"), XdrvMailbox.data);
          }          
        }
        break;
      case CMND_MESH_CHANNEL:
        if (XdrvMailbox.data_len > 0) {
          AddLog_P(LOG_LEVEL_DEBUG,PSTR("channel: %u"), XdrvMailbox.payload);
          MESH.channel = XdrvMailbox.payload;
        }
        Response_P(S_JSON_MESH_COMMAND_NVALUE, command, MESH.channel);
        break;
      case CMND_MESH_PEER:
        if (XdrvMailbox.data_len > 0) {
          uint8_t _MAC[6];
          MESHHexStringToBytes(XdrvMailbox.data,_MAC);
          AddLog_P(LOG_LEVEL_DEBUG,PSTR("MAC-string: %s"), XdrvMailbox.data);
          AddLogBuffer(LOG_LEVEL_INFO,(uint8_t *)_MAC,6);
          MESHaddPeer(_MAC);
          MESHcountPeers();
        }
        break;
      default:
        // else for Unknown command
        serviced = false;
      break;
    }
  } else {
    return false;
  }
  return serviced;
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xdrv56(uint8_t function)
{
  bool result = false;

  switch (function) {
    case FUNC_PRE_INIT:
      MESHInit();                              // TODO: save state
      break;
    case FUNC_INIT:
#ifdef ESP8266
      Settings.flag4.network_wifi = 1;
      TasmotaGlobal.global_state.wifi_down = 0;
#endif //ESP8266
      break;
    case FUNC_EVERY_50_MSECOND:
      MESHevery50MSecond();
      break;
    case FUNC_EVERY_SECOND:
      MESHEverySecond();
      break;
    case FUNC_COMMAND:
      result = MESHCmd();
      break;
    case FUNC_WEB_SENSOR:
#ifdef USE_WEBSERVER
      MESHshow(0);
#endif
      break;
    case FUNC_JSON_APPEND:
      MESHshow(1);
      break;
#ifdef ESP32
    case FUNC_MQTT_SUBSCRIBE:
      MESHconnectMQTT();
      break;
#endif //ESP32
    case FUNC_SHOW_SENSOR:
      MESHsendPeerList(); // we sync this to the Teleperiod with a delay
      break;
#ifdef USE_DEEPSLEEP
      case FUNC_SAVE_BEFORE_RESTART:
        MESHdeInit();
        break;
#endif // USE_DEEPSLEEP
  }
return result;
}

#endif  // USE_TASMESH
