#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266httpUpdate.h>

#define ARDUINOJSON_ENABLE_STD_STRING 1

#include <DHT.h>

#include <painlessMesh.h>

#include <PubSubClient.h>

#include <SoftwareSerial.h>

#define BLINKINV    500
#define COUNTDOWN   2
#define DHTPIN      5
#define MESHPREFIX "pharmdata"
#define MESHPASSWD "4026892842"
#define MESHPORT    1977
#define MQTTHOST    "pharmdata.ddns.net"
#define MQTTPORT    1883
#define PROG        "esp12e/bin/ESP8266_MESH_DHT22"
#define PROGNAME    "ESP8266_MESH_DHT22"
#define RESTARTINV  3600
#define RPTINV      10
#define WIFICONNECTTMO  10000

static DHT dht_g( DHTPIN, DHT22 );

static bool meshConnected_g = false;
static const char * updateAddr_g = NULL;
static float lastH_g = 0.0;
static float lastT_g = 0.0;
static painlessMesh mesh_g;
static Scheduler sched_g;
static String logTopic_g;
static String mac_g;
static String ssid_g;

static unsigned long nxtRpt_g = 0;
static unsigned long restartTime_g;

typedef struct
{
  const char * ssid;
  const char * passwd;
  const char * updateAddr;
} WiFiInfo;

WiFiInfo wifiInfo_g[] =
{
  { "lambhome", "4022890568", "chirpstack.lamb" }, 
  { "Jimmy-MiFi", "4026892842", "sheepshed.ddns.net" },
  { "sheepshed-mifi", "4026892842", "sheepshed.ddns.net" },
  { NULL }
};

static void _logBroadcast( const char * const aFrmt, ... );

/*
** This is needed as the machine is only present on the MESH.
*/
static void _recvMsg(
        uint32_t aFrom,
        String & aMsg
        )
    {
    const char * wp;

    wp = aMsg.c_str();

    if ( wp[ 0 ] == '-' )
        {
        if ( wp[ 1 ] == 'C' )
            {
            String cmd;

            for( wp += 2; *wp != '\0'; wp ++ )
                {
                if ( strchr( " \t\r\n", *wp ) != NULL )
                    {
                    wp ++;
                    break;
                    }

                cmd += *wp;
                }

            if ( cmd == "status" )
                {
                _logBroadcast( "%s (%u)%s t: %.1f, h: %.1f",
                        PROGNAME, mesh_g.getNodeId(), lastT_g, lastH_g );
                return;
                }
            
            if ( cmd == "restart" )
                {
                String grp;
                uint32_t nid = 0;

                for( ; *wp != '\0'; wp ++ )
                    {
                    if ( strchr( " /\t\r\n", *wp ) != NULL )
                        {
                        break;
                        }

                    nid *= 10;
                    nid += (*wp - '0');
                    grp += *wp;
                    }

                _logBroadcast( "%s (%u) (restart: %s)",
                        PROGNAME, mesh_g.getNodeId(), 
                        ((grp == PROGNAME) || (nid == mesh_g.getNodeId())) 
                            ? "TRUE" : "FALSE" );

                if ( (grp == PROGNAME) || (nid == mesh_g.getNodeId()) )
                    {
                    Serial.printf( "call restart\n" );
                    delay( 5000 );

                    ESP.restart();
                    }

                return;
                }

            if ( cmd == "BRIDGE" )
                {
                String bid;

                for( ; *wp != '\0'; wp ++ )
                    {
                    if ( strchr( " \t\r\n", *wp ) != NULL )
                        {
                        break;
                        }

                    bid += *wp;
                    }

                if ( !meshConnected_g )
                    {
                    restartTime_g = 0;
                    meshConnected_g = true;
                    _logBroadcast( "%s (%u) Joined. (BRIDGE: %s)",
                            PROGNAME, mesh_g.getNodeId(), bid.c_str() );
                    }

/*
                _logBroadcast( "%s (%u) BRIDGE: %s",
                        PROGNAME, mesh_g.getNodeId(), bid.c_str() );
*/

                return;
                }

            _logBroadcast( "(%s) cmd: %s, Not handled.", 
                    PROGNAME, cmd.c_str() );
            }

        return;
        }

    Serial.print( "_recvMsg:  " );
    Serial.println( aMsg );
    }

static void _logBroadcast(
        const char * const aFrmt,
        ...
        )
    {
    static char buf[ 400 ];

    String pkt;

    va_list ap;

    va_start( ap, aFrmt );
    vsnprintf( buf, sizeof( buf ), aFrmt, ap );
    va_end( ap );
      
    pkt = "-P/LOG ";
    pkt += buf;

    if ( !mesh_g.sendBroadcast( pkt ) )
        {
        Serial.println( "Attempt to broadcast failed.\r" );
        }

    }

static void _newConnection(
        uint32_t aNodeId
        )
    {
    Serial.printf( "_newConnection: " PROGNAME " (%u), aNodeId: %u\n", 
            mesh_g.getNodeId(), aNodeId );
    }

static void _connectionChange(
        )
    {
    Serial.printf( "_connectionChange: " PROGNAME " (%u)\n", mesh_g.getNodeId() );
    }

static unsigned long stTime_g;

static void _updateStart(
        )
    {
    Serial.print( "\r\n\nStart update\r\n" );
    stTime_g = millis();
    }

static void _updateEnd(
        )
    {
    unsigned long delta;

    delta = millis() - stTime_g;

    Serial.printf( "\r\nFinished: (%lu (millis))\r\n\n", delta );
    }

static void _updateProgress(
        int aCur,
        int aTot
        )
    {
    Serial.printf( "\r%d/%d(%d)", 
            aCur, aTot, (aTot != 0) ? (aCur * 100) / aTot : 0 );
    }

static void _updateError(
        int anErr
        )
    {
    Serial.printf( "[cb] _updateError Error: %d\n", anErr );
    }

static ESP8266WiFiMulti multi_g;

static WiFiClient psClient_g;

static void _cb( char * aTopic, byte * aPayload, unsigned int aPayloadLen );

static PubSubClient ps_g( MQTTHOST, MQTTPORT, _cb, psClient_g );

static String jsonTopic_g;


static void _log(
        const char * const aFrmt,
        ...
        )
    {
    static char buf[ 400 ];

    va_list ap;

    va_start( ap, aFrmt );
    vsnprintf( buf, sizeof( buf ), aFrmt, ap );
    va_end( ap );
      
    if ( ps_g.connected() )
        {
        ps_g.publish( logTopic_g.c_str(), buf );
        }
    else
        {
        Serial.println( buf );
        }
    }

static void _cb(
        char * aTopic,
        byte * aPayload,
        unsigned int aPayloadLen
        )
    {
    const char * wp;

    String payload;
    String topic;

    unsigned int idx;

    
    topic = aTopic;
    for( idx = 0; idx < aPayloadLen; idx ++ )
        {
        payload += (char) aPayload[ idx ];
        }

    wp = payload.c_str();
    if ( wp[ 0 ] == '-' )
        {
        if ( wp[ 1 ] == 'B' )
            {
            Serial1.println( wp + 2 );
            return;
            }
        }

    Serial.printf( "topic: %s\npayload: %s\n", topic.c_str(), payload.c_str() );
    }

static void _reconnect(
        )
    {
    static int attempt = 0;
    int retries;

    String clientId;
    String ssid;

    clientId = "MQTT-";
    clientId += mac_g;
    clientId += '-';
    clientId += String( attempt ++ );

    if ( ps_g.connected() )
        {
        Serial.println( "(_reconnect) - Already connected." );
        return;
        }

    for( retries = 0; (retries < 10) && (!ps_g.connected()); retries ++ )
        {
        if ( ps_g.connect( clientId.c_str() ) )
            {
            Serial.println( "MQTT Connected." );

            _log( "\nESP8266_MESH_DHT22:\n"
                    "\tTIMESTAMP: " __TIMESTAMP__ "\n"
                    "\tmac: %s\n"
                    "\tssid: %s\n"
                    , mac_g.c_str(), ssid_g.c_str() );
            }
        else
            {
            Serial.print( "failed, rc: " ); Serial.println( ps_g.state() );
            delay( 2000 );
            }
        }

    if ( !ps_g.connected() )
        {
        ESP.restart();
        }
    }

static const char * _findUpdateAddr(
        const String & aSSID
        )
    {
    WiFiInfo * wi;

    for( wi = wifiInfo_g; wi->ssid != NULL; wi ++ )
        {
        if ( aSSID == wi->ssid )
            {
            return wi->updateAddr;
            }
        }

    return NULL;
    }

static void _checkUpdate(
        )
    {
    String msg;

    WiFiClient client;

    ESP8266HTTPUpdate hu( 30000 );

    if ( updateAddr_g == NULL )
        {
        Serial.printf( "(_checkUpdate) - updateAddr_g not set.\n" );
        return;
        }

    _log( "_checkUpdate - updateAddr_g: %s, PROG: %s", updateAddr_g, PROG );

    hu.onStart( _updateStart );
    hu.onEnd( _updateEnd );
    hu.onProgress( _updateProgress );
    hu.onError( _updateError );

    t_httpUpdate_return ret = hu.update( 
            client, updateAddr_g, 80, "/update.php", PROG );

    _reconnect();

    switch( ret )
        {
        case HTTP_UPDATE_FAILED:
            msg = ESPhttpUpdate.getLastErrorString();

            Serial.println( "[update] failed." );
            Serial.printf( "   Error(%d): %s\n",
                    ESPhttpUpdate.getLastError(),
                    ESPhttpUpdate.getLastErrorString().c_str() );

            _log( "OTA failed. err: %s", msg.c_str() );

            delay( 10000 );
            ESP.restart();
            break;

        case HTTP_UPDATE_NO_UPDATES:
            _log( "[update] No Updates." );
            break;

        case HTTP_UPDATE_OK:
            _log( "[update] Ok" );
            break;

        default:
            _reconnect();
            _log( "[update] (Unexpected) ret: %d", ret );
            break;
        }

    /*
    ** give MQTT client a chance to output the message.
    */
    delay( 5000 );
    }

void setup(
        ) 
    {
    int cnt;

    Serial.begin( 115200 );

    delay( 100 );
    for( cnt = COUNTDOWN; cnt > 0; cnt -- )
        {
        Serial.print( " " ); Serial.print( cnt );
        delay( 1000 );
        }
    Serial.println( "" );

    pinMode( LED_BUILTIN, OUTPUT );

    dht_g.begin();

    pinMode( DHTPIN, INPUT_PULLUP );

    /*
    ** First attempt to connect to one of the configured wifi networks.
    */
    WiFiInfo * wi;

    WiFi.mode( WIFI_STA);
    mac_g = WiFi.macAddress();

    jsonTopic_g = "/JSON";

    logTopic_g = "/PUB/";
    logTopic_g += mac_g;
    logTopic_g += "/LOG";

    Serial.println( "Attempt Connect WiFi" );

    for( wi = wifiInfo_g; wi->ssid != NULL; wi ++ )
        {
        Serial.println( wi->ssid );
        multi_g.addAP( wi->ssid, wi->passwd );
        }

    if ( multi_g.run( WIFICONNECTTMO ) == WL_CONNECTED )
        {
        ssid_g = WiFi.SSID();

        updateAddr_g = _findUpdateAddr( ssid_g );

        Serial.printf( "ssid: %s   Connected\n",
                ssid_g.c_str() );

        _reconnect();
        _checkUpdate();
        }
    else
        {
        Serial.printf( "WIFI not connected.\n" );
        }

    /*
    ** Restart the wifi after the attempt to perform ota.
    */
    WiFi.disconnect();

    // mesh_g.setDebugMsgTypes( ERROR | STARTUP | CONNECTION ); 
    mesh_g.setDebugMsgTypes( ERROR ); 
    // mesh_g.setDebugMsgTypes( 0xFFFF );

    mesh_g.init( MESHPREFIX, MESHPASSWD, &sched_g, MESHPORT, WIFI_STA );

    mesh_g.onReceive( &_recvMsg );
    mesh_g.onNewConnection( &_newConnection );
    mesh_g.onChangedConnections( &_connectionChange );

    restartTime_g = millis() + (RESTARTINV * 1000);
    }

static bool blink_g = false;
static unsigned long nxtBlink_g = 0;

void loop(
        ) 
    {
    unsigned long now;

    mesh_g.update();

    now = millis();

    if ( now > nxtBlink_g )
        {
        nxtBlink_g = now + BLINKINV;

        digitalWrite( LED_BUILTIN, (blink_g) ? HIGH : LOW );
        blink_g = !blink_g;
        }

    if ( now > nxtRpt_g )
        {
        nxtRpt_g = now + (RPTINV * 1000);

        if ( !meshConnected_g )
            {
            return;
            }

        char buf[ 300 ];

        float h;
        float t;
    
        static unsigned int rptCnt = 0;

        t = dht_g.readTemperature( true );
        h = dht_g.readHumidity();

        Serial.printf( "t: %.1f, h: %.1f\n", t, h );

        if ( isnan( t ) )
            {
            Serial.println( "Temp is NAN\n" );
            return;
            }

        if ( isnan( h ) )
            {
            Serial.println( "Humidity is NAN\n" );
            return;
            }

/*
        _logBroadcast( "%s (%u) t: %.1f, h: %.1f", 
                PROGNAME, mesh_g.getNodeId(), t, h );
*/

        lastH_g = h;
        lastT_g = t;

        sprintf( buf, "-P/JSON {\"MAC\":\"%s\",\"RPTCNT\":%u,\"TEMP\":%.1f,\"HUMIDITY\":%.1f}",
                mac_g.c_str(), rptCnt ++, t, h );

        mesh_g.sendBroadcast( buf );
        }

    if ( (restartTime_g != 0) && (now > restartTime_g) )
        {
        _logBroadcast( "%s(%u) restartTime_g expired.", PROGNAME, 
                mesh_g.getNodeId() );
        
        ESP.restart();
        }
    }
