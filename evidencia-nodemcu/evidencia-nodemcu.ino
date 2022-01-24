//Librerías necesarias
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>

//Definiciones
#define TEST
#define RETAIN true
#define NO_RETAIN false
#define TURN_ON true
#define TURN_OFF false
#define BRIGHT true
#define DARK false
#define SEND true
#define SENT false
#define BLOCK false
#define FREE true

//Estados conexión WiFi
#define START_CONNECTION 0
#define WAIT_CONNECTION 1
#define CORRECT_CONNECTION 2
#define VERIFY_CONNECTION 3
#define RESTART_CONNECTION 4

//Estados conexión broker
#define CONFIGURE_MQTT 0
#define START_MQTT 1
#define SUBSCRIBE_MQTT 2
#define NOTIFY_MQTT 3
#define VERIFY_MQTT 4
#define WAIT_MQTT 5

//Configuración de pines
#define LIGHT_PIN 34
#define BUTTON_PIN 25
#define RELAY_PIN 26

//Tiempos de interrupción
#define TIME_BUTTON_INT 500000
#define TIME_SENSOR_INT 1000000

//Configuración del ADC
#define ADC_RES 4095
#define SENSOR_SCALE 100.0
#define MIN_LIGHT_SAMPLES 5

//Tópicos
#define LOG_TOPIC "room/light/log"
#define COMMAND_TOPIC "room/light/state"
#define SWITCH_TOPIC "room/light/switch"
#define LIGHTING_TOPIC "room/light/threshold"
#define LEVEL_TOPIC "room/light/level"
#define INFO_TOPIC "room/light/info"
#define ADJUST_TOPIC "room/light/adjust"

//Configuración mqtt
#define MESSAGE_MAX_SIZE 6+1

//Definiciones de cabecera
void mqttApp();
int connectWifi(int);
void connectMqtt();
void reciveResponse(char*, byte*, unsigned int);
void attachButtonInt();
void respondeButtonInt();
void allowButtonChange();
void changeRelayState(boolean);
double senseLight();
void setThreshold(int);
void configureSensor();
void calculateLighting();

//Variables de estado
volatile boolean relay_state = false;
boolean lighting_state = false;
boolean relay_message = false;
boolean light_message = false;
boolean lighting_message = false;
boolean initial_message = false;
volatile boolean is_button_change_allowed = false;

//Variables máquinas de estado
int wifi_stage = START_CONNECTION;
int mqtt_stage = CONFIGURE_MQTT;

//Variables del sensor
double light_level_samples[MIN_LIGHT_SAMPLES] = {};
double sum_light_level = 0;
int average_light_level = 0;
int index_light_sample = 0;
int total_samples = 0;
double light_threshold_level = 50;

//Variables de interrupciones
hw_timer_t *timer_light_sensor = NULL;
hw_timer_t *timer_button_change = NULL;

//Variables de red
char ssid[] = "DESKTOP-76EBGV3 7085";
char pass[] = "7{070v1Z";
char broker_dm[] = "broker.hivemq.com";
int broker_port =  1883;
WiFiClient wifi_client;
PubSubClient mqtt_client(wifi_client);

void setup(){
  #ifdef TEST
    Serial.begin(115200);
  #endif

  //Configuración de pines
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, relay_state);

  //Activación de la interrupción del bóton pulsador.
  allowButtonChange();

  //Configuración de los timers para las interrupciones.
  //Timer del sensor de iluminación calcula la iluminación cada cieto tiempo.
  //Timer del button reactiva las pulsaciones tanto físicas como virtuales para el cambio del relay.
  timer_light_sensor = timerBegin(0, 80, true);
  timerAttachInterrupt(timer_light_sensor, &calculateLighting, true);
  timerAlarmWrite(timer_light_sensor, TIME_SENSOR_INT, true);

  timer_button_change = timerBegin(1, 80, true);
  timerAttachInterrupt(timer_button_change, &allowButtonChange, true);
  timerAlarmWrite(timer_button_change, TIME_BUTTON_INT, false);
}

void loop(){
  wifi_stage = connectWifi(wifi_stage);
  mqtt_stage = connectMqtt(mqtt_stage);
  if(wifi_stage == VERIFY_CONNECTION){
    if(mqtt_stage == VERIFY_MQTT){
      mqtt_client.loop();
      mqttApp();  
    }
  }
}

/*
 * Función que realiza publicación de tópicos de acuerdo a las variables de publicación de mensajes.
 * Si la variable asociada a la publicación de un tipo de mensaje específico indica que se debe
 * publicar el mensaje, se realiza una publicación del mensaje con el protocolo mqtt, se indica
 * que el mensaje fue enviado en la misma variable.
 * La publicación de los mensaje se realiza de esta forma ya que se encontraron problemas asociados
 * a la publicación de los mensajes dentro de la rutinas de interrupciones.
 * relay_message: mensaje asociado al estado del apagado o encendido del relay.
 * light_message: mensaje asociado al estado de la iluminación del lugar.
 * lighting_message: mensaje asociado valor promedio de la iluminación.
 * initial_message: mensaje asociado al mensaje de conexión del dispositivo con el broker.
*/
void mqttApp(){
  //Serial.println("running");
  if(relay_message){
    relay_message = SENT;
    mqtt_client.publish(SWITCH_TOPIC, relay_state?"ON":"OFF", NO_RETAIN);
    #ifdef TEST
      Serial.print("<===");
      Serial.print(relay_state?"ON":"OFF");
      Serial.print(" publicado en topico ");
      Serial.println(SWITCH_TOPIC);
    #endif
  }
  if(light_message){
    light_message = SENT;
    mqtt_client.publish(LIGHTING_TOPIC, lighting_state?"BRIGHT":"DARK", NO_RETAIN);
    #ifdef TEST
      Serial.print("<===");
      Serial.print(lighting_state?"ON":"OFF");
      Serial.print(" publicado en topico ");
      Serial.println(LIGHTING_TOPIC);
    #endif
  }
  if(lighting_message){
    lighting_message = SENT;
    char load[3];
    String(average_light_level).toCharArray(load, 3);
    mqtt_client.publish(LEVEL_TOPIC, load, NO_RETAIN);
    #ifdef TEST
      Serial.print("<===");
      Serial.print(average_light_level);
      Serial.print(" publicado en topico ");
      Serial.println(LEVEL_TOPIC);
    #endif
  }
  if(initial_message){
    initial_message = SENT;
    mqtt_client.publish(LOG_TOPIC, ":)", NO_RETAIN);
    #ifdef TEST
      Serial.print("<===");
      Serial.print(":)");
      Serial.print(" publicado en topico ");
      Serial.println(LOG_TOPIC);
    #endif
  }
}

/*
 * Función encargada de la conexión WiFi.
 * Máquina de estados para la configuración, espera, conexión, reconexión y verificación de la red
 * inalámbrica.
 * START_CONNECTION: Configura la conexión y la inicia.
 * WAIT_CONNECTION: Espera hasta lograr la conexión.
 * CORRECT_CONNECTION: Imprime un mensaje de lograda la conexión.
 * VERIFY_CONNECTION: Verifica que la conexión se encuente viva.
 * RESTART_CONNECTION: Se reconecta a la red en caso de desconexión.
*/
int connectWifi(const int stage){
  int next_stage;
  if(stage == START_CONNECTION){
    #ifdef TEST
      Serial.println("Conectando WiFi");
    #endif
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    next_stage = WAIT_CONNECTION;
  }else if(stage == WAIT_CONNECTION){
    if(WiFi.status() == WL_CONNECTED){
      #ifdef TEST
        next_stage = CORRECT_CONNECTION;
      #else
        next_stage = VERIFY_CONNECTION;
      #endif
    }else{
      next_stage = WAIT_CONNECTION;
    }
#ifdef TEST
  }else if(stage == CORRECT_CONNECTION){
    #ifdef TEST
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      Serial.println("Conectado WiFi");
    #endif
    next_stage = VERIFY_CONNECTION;
#endif
  }else if(stage == VERIFY_CONNECTION){
    if(WiFi.status() != WL_CONNECTED){
      next_stage = RESTART_CONNECTION;
    }else{
      next_stage = VERIFY_CONNECTION;
    }
  }else if(stage == RESTART_CONNECTION){
    #ifdef TEST
      Serial.println("Reconectando WiFi");
    #endif
    WiFi.reconnect();
    next_stage = WAIT_CONNECTION;
  }
  
  return next_stage;
}


/*
 * Función encargada de la conexión con el broker.
 * Máquina de estados para la configuración, conexión, subscripción y verificación de la conexión
 * con el broker.
 * CONFIGURE_MQTT: Configura los parametros de conexión con el broker.
 * WAIT_MQTT: Espera a tener una conexión WiFi exitosa para realizar la conexión con el broker.
 * START_MQTT: Intenta realizar la conexión con el broker, si es exitosa pasa al siguiente estado
 *             de subscipción y configura el sensor de iluminación, si es fallida repite este mismo
 *             estado.
 * SUBSCRIBE_MQTT: Realiza la subscripción a los diferentes tópicos que le interesa a la aplicación.
 * NOTIFY_MQTT: Indica las publicaciones que debe realizar después lograr la conexión exitosa para 
 *              dar a conocer su estado al otro cliente.
 * VERIFY_MQTT: Verifica que se encuentre conectado con el broker, en caso de fallar la conexión vuelve
 *              a internar a conectarse volviendo al estado WAIT_MQTT, además si falla desactiza la 
 *              interrupcione asociada a la medición de la iluminación.
*/
int connectMqtt(const int stage){
  int next_stage;
  if(stage == CONFIGURE_MQTT){
    #ifdef TEST
      Serial.println("Configurando MQTT");
    #endif
    mqtt_client.setServer(broker_dm, broker_port);
    mqtt_client.setCallback(reciveResponse);
    next_stage = WAIT_MQTT;
  }else if(stage == WAIT_MQTT){
    if(wifi_stage == VERIFY_CONNECTION){
      next_stage = START_MQTT;
    }else{
      next_stage = WAIT_MQTT;
    }
  }else if(stage == START_MQTT){
    #ifdef TEST
      Serial.println("Conectando MQTT");
    #endif
    if(mqtt_client.connect("")){
      configureSensor();
      next_stage = SUBSCRIBE_MQTT;
    }else{
      next_stage = START_MQTT;
    }
  }else if(stage == SUBSCRIBE_MQTT){
    #ifdef TEST
      Serial.println("Subscribiendo MQTT");
    #endif
    mqtt_client.subscribe(COMMAND_TOPIC);
    mqtt_client.subscribe(INFO_TOPIC);
    mqtt_client.subscribe(ADJUST_TOPIC);
    next_stage = NOTIFY_MQTT;
  }else if(stage == NOTIFY_MQTT){
    #ifdef TEST
      Serial.println("Notificando MQTT");
    #endif
    initial_message = SEND;
    light_message = SEND;
    relay_message = SEND;
    next_stage = VERIFY_MQTT;
  }else if(stage == VERIFY_MQTT){
    if(mqtt_client.connected()){
      next_stage = VERIFY_MQTT;
    }else{
      timerAlarmDisable(timer_light_sensor);
      next_stage = WAIT_MQTT;
    }
  }
  
  return next_stage;
}

/*
 * Función callback para la llegada de mensajes publicados.
 * Recupera el mensaje y lo almacena en un arreglo de caracteres, compara el tópico de llegada
 * con los diferentes tópicos a los que esta subscrito.
 * COMMAND_TOPIC: Mensaje que indica si se debe encender o apagar el relay.
 * ADJUST_TOPIC: Define un nuevo valor de umbral de la iluminación de acuerdo al valor recibido
 *               en el mensaje.
 * INFO_TOPIC: Si el mensaje indica GET el dispositivo publica sus estados: conexión, iluminaición y relay;
 *             si el mensaje indica VAL el dispositivo publica el valor de la iluminación.
*/
void reciveResponse(char* topic, byte* payload, unsigned int length){
  char message[MESSAGE_MAX_SIZE];
  for(int i=0; i<length && i<MESSAGE_MAX_SIZE; i++){
    message[i] = (char)payload[i];
  }
  message[length] = '\0';
  #ifdef TEST
    Serial.print("===>");
    Serial.print("El topico ");
    Serial.print(topic);
    Serial.print(" dice: ");
    Serial.println(message);
  #endif
  if(String(topic) == COMMAND_TOPIC){
    if(String(message) == "OFF"){
      changeRelayState(TURN_OFF);
    }else if(String(message) == "ON"){
      changeRelayState(TURN_ON);
    }
  }else if(String(topic) == ADJUST_TOPIC){
    int value = String(message).toInt();
    if(value != 0){
      setThreshold(value);  
    }
  }else if(String(topic) == INFO_TOPIC){
    if(String(message) == "GET"){
      initial_message = SEND;
      light_message = SEND;
      relay_message = SEND;
    }else if(String(message) == "VAL"){
      lighting_message = SEND;
    }
  }
}

/*
 * Función de asignación de interrupción al botón
 * Activa las interrupción del butón pulsador asociando los cambios de bajo a alto
 * a una funcion.
*/
void attachButtonInt(){
  attachInterrupt(BUTTON_PIN, respondeButtonInt, RISING);
}

/*
 * Función de respuesta de la interrupción del botón.
 * Rutina de interrupción del botón pulsador, indica la modificación del estado 
 * del relay al estado opuesto actual.
*/
void respondeButtonInt(){
  #ifdef TEST
    Serial.print("Buton pulsado: ");
    if(!relay_state){
      Serial.println("activar relevador");
    }else{
      Serial.println("desactivar relevador");
    }
  #endif
  changeRelayState(!relay_state);
}

/*
 * Función de activación de cambios en relay.
 * Rutina de interrupción del timer de pulsaciones, indica que permite los cambios
 * de estado en el relay y reactiva las interrupción del botón pulsador.
*/
void allowButtonChange(){
  is_button_change_allowed = FREE;
  attachButtonInt();
}

/*
 * Función para realizar un cambio en el relay.
 * Verfica si estan disponibles las modificaciones, si es encuentran disponibles verifica
 * si en realidad es un cambio de estado o se mantiene. Si es un cambio de estado pasa
 * a desactivar las interrupciones del botón y la variables asociada a permitir los cambios.
 * Asigna el nuevo estado a nivel físico y lógico, notifica un mensaje de publicación.
 * Resetea el timer para permitir nuevos cambios de estado del relay, hasta que se complete
 * dicho timer y realize su interrupción asociada los cambios no estaran permitidos.
*/
void changeRelayState(boolean state){
  if(is_button_change_allowed){
    if(state != relay_state){
      detachInterrupt(BUTTON_PIN);
      is_button_change_allowed = BLOCK;
      relay_state = state;  
      digitalWrite(RELAY_PIN, relay_state);
      relay_message = SEND;
      timerAlarmDisable(timer_button_change);
      timerWrite(timer_button_change, 0);
      timerAlarmEnable(timer_button_change);
    }
  }
}

/*
 * Función de sensado de la iluminación.
 * Realiza una conversión analógica-dígital del sensor de iluminación,
 * coloca el valor en un rango.
*/
double senseLight(){
  int adc_light_value = analogRead(LIGHT_PIN);
  return (adc_light_value * SENSOR_SCALE)/ADC_RES;
}

/*
 * Función de asignación de umbral de iluminación.
 * Asigna un nuevo umbral de iluminación.
*/
void setThreshold(int value){
  light_threshold_level = value;
  #ifdef TEST
    Serial.print("Nuevo ajuste de luz: ");
    Serial.println(light_threshold_level);
  #endif
}

/*
 * Función de configuración del sensor.
 * Coloca los valores de las mediciones del sensor a sus valores iniciales, asigna los
 * primeros valores de medición del sensor y activa el timer para la ejecución periodica
 * de las mediciones del sensor.
*/
void configureSensor(){
  index_light_sample = 0;
  total_samples = 1;
  memset(light_level_samples, 0, sizeof(light_level_samples));
  sum_light_level = senseLight();
  average_light_level = (int)sum_light_level;
  light_level_samples[index_light_sample] = sum_light_level;
  lighting_state = sum_light_level>light_threshold_level?BRIGHT:DARK;
  timerWrite(timer_light_sensor, 0);
  timerAlarmEnable(timer_light_sensor);
  #ifdef TEST
    Serial.print("El valor de iluminacion es: ");
    Serial.println(average_light_level);
  #endif
}

/*
 * Función de medición de la iluminacion.
 * Rutina de interrupción asociada al timer del sensor de iluminación
 * Calcula la iluminación en base al umbral de iluminación definido en la configuración.
 * Realiza el calculo en base al valor promedio de varias mediciones: resta la última 
 * muestra que ya no pertenece a las muestras del periodo utíl, realiza una nueva
 * medición, almecena y suma para completar la integración de las muestras del periodo,
 * calcula el valor promedio del periodo y compara respecto al umbral, verifica si exite
 * un cambio respecto al estado de la iluminación anterior, si exite cambio notifica el cambio.
 * Se ulitiza un arreglo circular para almacenar las muestras.
*/
void calculateLighting(){
  total_samples++;
  index_light_sample = (index_light_sample+1)%MIN_LIGHT_SAMPLES;
  sum_light_level = sum_light_level - light_level_samples[index_light_sample];
  light_level_samples[index_light_sample] = senseLight();
  sum_light_level = sum_light_level + light_level_samples[index_light_sample];
  average_light_level = (int)sum_light_level/(total_samples>MIN_LIGHT_SAMPLES?MIN_LIGHT_SAMPLES:total_samples);
  boolean has_lighting_changed = lighting_state xor average_light_level>light_threshold_level?BRIGHT:DARK;
  #ifdef TEST
    Serial.print("El valor de iluminacion es: ");
    Serial.println(average_light_level);
  #endif
  if(has_lighting_changed){
    lighting_state = !lighting_state;
    light_message = SEND;
    #ifdef TEST
      Serial.print("El promedio de iluminacion es: ");
      Serial.println(lighting_state?"Iluminado":"Obscuro");
    #endif
  }
}
