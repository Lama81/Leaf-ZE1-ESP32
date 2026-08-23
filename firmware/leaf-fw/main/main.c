#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/twai.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "nvs.h"
#include <stdatomic.h>

static const char *TAG = "LEAF_CAN";

// Version affichee sur la page web (a droite du titre) et /status - pour
// savoir a distance quel firmware tourne sans avoir besoin de l'USB. Genere
// automatiquement a chaque "idf.py build" (date/heure de compilation) -
// impossible a oublier de mettre a jour, contrairement a une chaine fixe.
#define FW_VERSION __DATE__ " " __TIME__

// ---- CAN pins EV-CAN (dashboard, ecoute) ----
#define EVCAN_TX_GPIO GPIO_NUM_32
#define EVCAN_RX_GPIO GPIO_NUM_33

// ---- CAN pins CAR-CAN (climate + lock/unlock, transmission) ----
#define CARCAN_TX_GPIO GPIO_NUM_26
#define CARCAN_RX_GPIO GPIO_NUM_14

// alias pour compatibilite avec le code existant (ecoute = EV-CAN)
#define CAN_TX_GPIO EVCAN_TX_GPIO
#define CAN_RX_GPIO EVCAN_RX_GPIO

// ---- WiFi AP ----
// Identifiants dans wifi_secrets.h (non suivi par git, voir .gitignore) -
// copie wifi_secrets.h.example et renseigne tes propres identifiants avant
// de flasher. Ne jamais commit de vrai mot de passe.
#include "wifi_secrets.h"

// ---- UART vers Boron (relais commandes LTE) ----
#define BORON_UART_NUM UART_NUM_1
#define BORON_UART_TX_GPIO GPIO_NUM_19
#define BORON_UART_RX_GPIO GPIO_NUM_21
#define BORON_UART_BAUD 9600

// ---- Deep sleep ----
// GPIO34 (D8 cote Boron) = reveil EXT0 sur niveau haut, pilote par le Boron
// avant d'envoyer une commande UART. IMPORTANT: GPIO34-39 (ESP32 original)
// n'ont PAS de pull-down interne - une resistance pull-down externe sur
// GPIO34 est necessaire pour un reveil fiable (sinon niveau flottant tant
// que le Boron ne conduit pas la ligne = reveils intempestifs possibles).
#define WAKE_GPIO GPIO_NUM_34
#define DEEP_SLEEP_TIMER_US (6ULL * 3600ULL * 1000000ULL)   // reveil periodique 6h
#define INACTIVITY_TIMEOUT_US (5LL * 60 * 1000000LL)        // redort apres 5min sans activite
#define TIMER_WAKE_LISTEN_US (3LL * 1000000LL)               // fenetre d'ecoute EV-CAN au reveil timer

typedef struct {
    char text[160];
} can_msg_t;

static QueueHandle_t can_queue;
static SemaphoreHandle_t twai_mutex;

// Pousse un evenement (climate ON/OFF, lock/unlock, etc.) vers le dashboard
// web avec l'uptime en minutes:secondes - visible dans le log en direct de
// la page, sans avoir besoin du moniteur serie USB.
static void push_web_log(const char *msg)
{
    int64_t up = esp_timer_get_time() / 1000000LL;
    can_msg_t item;
    snprintf(item.text, sizeof(item.text), "[%lldm%02llds] %s", up / 60, up % 60, msg);
    xQueueSend(can_queue, &item, 0);
}
static volatile bool transmitting = false;
static volatile bool monitor_verbose = false;  // false = seulement IDs HVAC/batterie utiles

// Signale l'intention de prendre twai_mutex *avant* meme de le demander -
// can_task boucle en prenant/relachant ce mutex a chaque frame recue sur un
// bus actif (voir can_task), laissant des fenetres de quelques microsecondes
// seulement pour qu'un xSemaphoreTake concurrent s'y glisse. Sur bus actif,
// une commande lock/unlock/heat peut ainsi se faire "affamer" jusqu'a son
// propre timeout (8000ms cote take_twai_mutex_or_fail) sans jamais obtenir
// le mutex. Ce flag fait sortir can_task de sa boucle de prise/liberation
// des la demande, au lieu d'attendre qu'il gagne la course.
static volatile bool bus_switch_requested = false;

// Derniere valeur connue (mise a jour passivement par le monitoring EV-CAN
// continu) - exposee a la demande via UART/STATUS pour le Boron, jamais
// streamee en continu vers le cellulaire (cf. contrainte data minimale).
static volatile float g_last_soc = -1.0f;       // 0x55B, -1 = pas encore vu
static volatile int g_last_soh = -1;            // 0x5BC, -1 = pas encore vu
static volatile int g_last_vehicle_on = -1;     // 0x11A, -1 = pas encore vu

// Odometre (0x5C5, CAR-CAN) - contrairement a soc/soh/vehicle_on ci-dessus,
// pas mis a jour passivement (CAR-CAN n'est jamais ecoute en continu, voir
// CLAUDE.md section concurrence). Rafraichi uniquement sur demande explicite
// par do_read_odometer_sequence(). -1 = jamais lu depuis le boot.
static volatile int32_t g_last_odometer_km = -1; // suppose km (marche canadien) - unite non confirmee, voir commentaire dans do_read_odometer_sequence()

// Etat reel du radio WiFi, mis a jour a chaque WIFI_ON/WIFI_OFF et inclus
// dans STATUS - le dashboard web n'avait avant que sa propre supposition
// locale (dernier clic), fausse des que l'etat change ailleurs (autre
// appareil, reboot). true au demarrage: wifi_init_softap() active l'AP au boot.
static volatile bool g_wifi_on = true;

// ---- Guard climate qui repart seul ----
// Cause reelle trouvee et corrigee dans do_heat_sequence() : il manquait la
// trame de cloture AUTO_DISABLE_CLIMATE_CONTROL (0x56E, 46 08 32 00) qu'OVMS
// envoie systematiquement ~1 sec apres la fin des repetitions ENABLE. Sans
// elle le BCM ne refermait jamais son cycle d'activation interne. Confirme
// sur le terrain : plus aucune reprise spontanee depuis l'ajout de la trame.
//
// Pas de watchdog logiciel : le timer natif du vehicule (15-20 min) ferme
// deja la session climate de lui-meme sans redemarrage (confirme sur le
// terrain). g_climate_active sert seulement a la persistance NVS (recuperation
// post-crash) et au diagnostic /status.
static volatile bool g_climate_active = false;

// ---- Persistance etat climate (NVS) ----
// Objectif: si l'ESP32 reset/crash (brownout, panic, etc.) pendant une session
// climate active, les flags en RAM (g_climate_active) sont perdus au reboot et
// les guards 1/2 ne peuvent plus rattraper la transition ON->OFF du vehicule.
// On persiste donc l'etat sur NVS pour pouvoir renvoyer un OFF de securite au
// demarrage si le dernier etat connu etait "climate actif".
#define NVS_NAMESPACE "leafcan"
#define NVS_KEY_CLIMATE "climate_on"

static void climate_state_persist(bool active)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_CLIMATE, active ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool climate_state_load(void)
{
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_CLIMATE, &val);
        nvs_close(h);
    }
    return val != 0;
}

// ---- Persistance preference WiFi (NVS) ----
// wifi_init_softap() demarre toujours l'AP au boot, y compris apres un
// reveil EXT0 - sans ca, une commande WIFI_OFF envoyee avant que l'ESP32
// s'endorme (deep sleep) serait silencieusement annulee au reveil suivant
// (le WiFi reviendrait ON tout seul, sans que l'utilisateur l'ait redemande).
// Cle distincte de NVS_KEY_CLIMATE, meme namespace. Absence de cle = ON par
// defaut (comportement du tout premier boot, avant tout WIFI_OFF explicite).
#define NVS_KEY_WIFI "wifi_on"

static void wifi_state_persist(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_WIFI, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool wifi_state_load(void)
{
    nvs_handle_t h;
    uint8_t val = 1;  // defaut ON si jamais persiste (premier boot)
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_WIFI, &val);
        nvs_close(h);
    }
    return val != 0;
}

// ---- Persistance telemetrie (NVS) ----
// g_last_soc/soh/vehicle_on sont en RAM -> perdus a chaque deep sleep. On les
// persiste pour que STATUS reponde avec les dernieres valeurs connues des le
// reveil (avant meme qu'une nouvelle trame EV-CAN soit vue), et pour que le
// reveil periodique TIMER (silencieux, sans uart_task) puisse les mettre a
// jour sans jamais demarrer WiFi/webserver.
#define NVS_KEY_SOC "last_soc"   // stocke en dixiemes de %, i32 (pas de type float natif)
#define NVS_KEY_SOH "last_soh"
#define NVS_KEY_VEH "last_veh"
#define NVS_KEY_ODO "last_odo"   // odometre - voir do_read_odometer_sequence(), pas mis a jour passivement comme les autres

static void telemetry_state_persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_SOC, (int32_t)(g_last_soc * 10.0f));
        nvs_set_i32(h, NVS_KEY_SOH, g_last_soh);
        nvs_set_i32(h, NVS_KEY_VEH, g_last_vehicle_on);
        nvs_set_i32(h, NVS_KEY_ODO, g_last_odometer_km);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void telemetry_state_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, NVS_KEY_SOC, &v) == ESP_OK) g_last_soc = v / 10.0f;
        if (nvs_get_i32(h, NVS_KEY_SOH, &v) == ESP_OK) g_last_soh = v;
        if (nvs_get_i32(h, NVS_KEY_VEH, &v) == ESP_OK) g_last_vehicle_on = v;
        if (nvs_get_i32(h, NVS_KEY_ODO, &v) == ESP_OK) g_last_odometer_km = v;
        nvs_close(h);
    }
}

// ---- Suivi d'activite (pour le retour en deep sleep apres inactivite) ----
// _Atomic, pas juste volatile: ecrit depuis plusieurs taches (handlers HTTP,
// uart_task) et lu dans app_main - un int64_t n'est pas accede atomiquement
// sur ESP32 (32-bit), une lecture/ecriture dechiree donnerait une valeur
// aberrante et un deep sleep intempestif (ou manque) au mauvais moment.
static _Atomic int64_t g_last_activity_us = 0;
static void touch_activity(void)
{
    g_last_activity_us = esp_timer_get_time();
}

// ---- Diagnostic reset/reboot ----
// Permet de confirmer a distance (sans USB) si l'ESP32 a plante/redemarre
// (brownout, panic, watchdog...) entre deux visites de la page monitor.
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}
static char g_boot_reason[16] = "?";

// ---------------------------------------------------------------------
// WiFi Access Point
// ---------------------------------------------------------------------
static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .password = WIFI_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP demarre. SSID:%s IP:192.168.4.1", WIFI_SSID);
}

// ---------------------------------------------------------------------
// Decodage batterie - source: dalathegreat/leaf_can_bus_messages EV-can_ZE1.dbc
// Lecture seule, aucune transmission
// ---------------------------------------------------------------------
static void decode_battery_frame(uint32_t id, uint8_t *data, char *out, size_t out_size)
{
    if (id == 0x5BC) {
        // LB_Remain_Capacity_GIDS: start bit 7, len 10, motorola -> byte0<<2 | byte1>>6
        int gids = (data[0] << 2) | (data[1] >> 6);
        // LB_Capacity_Deterioration_Rate (SOH): start bit 33, len 7, intel -> byte4>>1
        int soh = (data[4] >> 1) & 0x7F;
        g_last_soh = soh;
        snprintf(out, out_size, " | GIDS:%d SOH:%d%%", gids, soh);

    } else if (id == 0x55B) {
        // LB_SOC: start bit 7, len 10, motorola, unit 0.1% -> byte0<<2 | byte1>>6
        int soc_raw = (data[0] << 2) | (data[1] >> 6);
        g_last_soc = soc_raw / 10.0f;
        snprintf(out, out_size, " | SOC:%.1f%%", soc_raw / 10.0);

    } else if (id == 0x1DB) {
        // LB_Total_Voltage: start bit 23, len 10, 0.5V/bit -> byte2<<2 | byte1>>6
        int volt_raw = (data[2] << 2) | (data[1] >> 6);
        float voltage = volt_raw * 0.5f;
        // LB_Current: start bit 7, len 11 signed, 0.5A/bit -> byte0<<3 | byte1>>5
        int cur_raw = (data[0] << 3) | (data[1] >> 5);
        if (cur_raw & 0x400) cur_raw -= 0x800;  // sign-extend 11 bits
        float current = cur_raw * 0.5f;
        snprintf(out, out_size, " | Pack:%.1fV %.1fA", voltage, current);

    } else if (id == 0x1DC) {
        // Limites de puissance charge/decharge - format Motorola complexe,
        // calcul de bits non verifie de facon independante. Raw hex seulement
        // pour l'instant, a decoder plus tard avec validation empirique.
        out[0] = '\0';

    } else if (id == 0x54C) {
        // FanVoltage: byte5, 0.05V/bit - seul indicateur fiable pour ON/OFF reel
        // (le bit CC_ClimateControlStatus seul n'est pas fiable, confirme par OVMS)
        bool really_off = (data[5] == 0x00 || data[5] == 0xF8);
        float ambient = data[6] * 0.5f - 40.5f;
        snprintf(out, out_size, " | Climate:%s Exterieur:%.1fC",
                 really_off ? "OFF" : "ON", ambient);

    } else if (id == 0x1D4) {
        // ChargeStatus: byte6 (bits48-55) - PEU FIABLE sur ce vehicule (signale
        // constant "en charge" peu importe l'etat reel). L'alternative testee
        // (relais AC/QC, trame 0x390, source OVMS AZE0) n'a jamais ete vue sur
        // ce bus non plus - aucune source fiable trouvee jusqu'ici. Reconnu
        // mais volontairement non decode, meme traitement que 0x1DC.
        out[0] = '\0';

    } else if (id == 0x11A) {
        // CarOnOffStatus: bits13-15, intel, byte1 bits5-7 -> (byte1>>5)&0x07
        int status = (data[1] >> 5) & 0x07;
        if (status == 8) g_last_vehicle_on = 1;
        else if (status == 4) g_last_vehicle_on = 0;
        snprintf(out, out_size, " | Vehicule:%s", status == 8 ? "ON" : (status == 4 ? "OFF" : "?"));

    } else {
        out[0] = '\0';
    }
}

// ---------------------------------------------------------------------
// Transmission - wake-up + climate ON (source: OVMS, sequence documentee)
// ---------------------------------------------------------------------

// Prise du mutex TWAI avec delai borne au lieu de portMAX_DELAY. Un gel a
// l'intérieur d'une sequence de commande (crash partiel, etat inattendu)
// devient une erreur visible et bornee dans le temps, au lieu d'un blocage
// silencieux permanent qui exige un reboot physique pour s'en sortir - le
// "page gelee" du 2026-07-29 n'avait laisse aucune trace exploitable.
#define TWAI_MUTEX_TIMEOUT_MS 8000
static bool take_twai_mutex_or_fail(const char *op_name)
{
    // Fait reculer can_task de sa boucle prise/liberation avant meme d'entrer
    // en competition pour le mutex - voir le commentaire sur bus_switch_requested.
    bus_switch_requested = true;
    bool got = xSemaphoreTake(twai_mutex, pdMS_TO_TICKS(TWAI_MUTEX_TIMEOUT_MS)) == pdTRUE;
    bus_switch_requested = false;

    if (!got) {
        ESP_LOGE(TAG, "Mutex TWAI indisponible apres %dms (%s) - abandon", TWAI_MUTEX_TIMEOUT_MS, op_name);
        char msg[100];
        snprintf(msg, sizeof(msg), "ERREUR: bus CAN occupe/bloque (%s) - commande annulee, reessaie", op_name);
        push_web_log(msg);
        return false;
    }
    return true;
}

static void can_send_frame(uint32_t id, uint8_t *data, uint8_t len)
{
    twai_message_t msg = {0};
    msg.identifier = id;
    msg.data_length_code = len;
    for (int i = 0; i < len; i++) msg.data[i] = data[i];

    esp_err_t r = twai_transmit(&msg, pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "TX 0x%03lX -> %s", (unsigned long)id, r == ESP_OK ? "OK" : "FAIL");
}

// Verifie si le controleur TWAI est tombe en bus-off (ex: trames non-acquittees
// en boucle parce que le BCM n'est pas encore reveille) - permet d'abandonner
// une sequence de repetitions au lieu de continuer a taper dans le vide.
static bool twai_is_bus_off(void)
{
    twai_status_info_t s;
    return (twai_get_status_info(&s) == ESP_OK && s.state == TWAI_STATE_BUS_OFF);
}

// Wake-up CAR-CAN, confirme dans vehicle_nissanleaf.cpp (CommandWakeupAZE0_2, model year >=2016)
static void carcan_wakeup(void)
{
    uint8_t d1[1] = {0x00};
    can_send_frame(0x68c, d1, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t d4[4] = {0x46, 0x08, 0x00, 0x00};
    can_send_frame(0x56e, d4, 4);
}

// Nombre de repetitions - REMOTE_COMMAND_REPEAT_COUNT confirme dans vehicle_nissanleaf.cpp
// (OVMS) = 24, envoye toutes les 100ms (~2.4s), meme trames de reveil que les notres
// (CommandWakeupAZE0_2, verifie ligne par ligne dans le source). Pas de raison d'en
// envoyer plus - plus de repetitions non-acquittees = plus de risque de bus-off avant
// meme que le BCM soit reveille (voir twai_is_bus_off() ci-dessus, cause probable du
// gel climate du 2026-07-29 avec l'ancien 120).
#define CARCAN_REPEAT_COUNT_HEAT 24   // ~2.4 sec - aligne sur OVMS REMOTE_COMMAND_REPEAT_COUNT
#define CARCAN_REPEAT_COUNT_LOCK 15   // ~1.5 sec - deja fiable en pratique, laisse tel quel

// temp_byte: (temp_celsius - 16) * 2   ex: 22C -> 0x0C, 25C -> 0x12
static bool do_heat_off_sequence(void)
{
    if (!take_twai_mutex_or_fail("heat_off")) return false;
    transmitting = true;

    ESP_LOGI(TAG, "Bascule sur CAR-CAN pour DISABLE climate");
    twai_stop();
    twai_driver_uninstall();

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CARCAN_TX_GPIO, CARCAN_RX_GPIO, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK && twai_start() == ESP_OK) {
        carcan_wakeup();
        // Laisse le BCM le temps de sortir de veille avant de commencer les
        // repetitions - 20ms etait probablement trop court sur bus froid
        // (voir gel climate du 2026-07-29, trames non-acquittees -> bus-off).
        vTaskDelay(pdMS_TO_TICKS(250));

        // Payload confirme dans vehicle_nissanleaf.cpp, case DISABLE_CLIMATE_CONTROL
        uint8_t data[4] = {0x56, 0x00, 0x01, 0x00};
        for (int i = 0; i < CARCAN_REPEAT_COUNT_LOCK; i++) {
            can_send_frame(0x56E, data, 4);
            vTaskDelay(pdMS_TO_TICKS(100));
            if (twai_is_bus_off()) {
                ESP_LOGW(TAG, "BUS-OFF pendant sequence (frame %d/%d) - abandon repetitions", i + 1, CARCAN_REPEAT_COUNT_LOCK);
                push_web_log("BUS-OFF pendant sequence - abandon repetitions");
                break;
            }
        }
        ESP_LOGI(TAG, "Climate OFF envoye sur CAR-CAN");
        push_web_log("Climate OFF envoye");
    } else {
        ESP_LOGE(TAG, "Echec passage CAR-CAN NORMAL");
        push_web_log("ERREUR: passage CAR-CAN echoue - commande non envoyee");
    }

    twai_stop();
    twai_driver_uninstall();

    twai_general_config_t g_config2 = TWAI_GENERAL_CONFIG_DEFAULT(EVCAN_TX_GPIO, EVCAN_RX_GPIO, TWAI_MODE_LISTEN_ONLY);
    if ((twai_driver_install(&g_config2, &t_config, &f_config) != ESP_OK) || (twai_start() != ESP_OK)) {
        ESP_LOGE(TAG, "Echec retour en ecoute EV-CAN - monitoring probablement mort, reboot recommande");
        push_web_log("ERREUR: retour EV-CAN echoue - reboot recommande");
    }

    transmitting = false;
    xSemaphoreGive(twai_mutex);

    // Session fermee proprement (manuelle ou arret naturel de la voiture).
    g_climate_active = false;
    climate_state_persist(false);
    return true;
}

static esp_err_t heat_off_handler(httpd_req_t *req)
{
    touch_activity();
    bool ok = do_heat_off_sequence();
    const char *resp = ok ? "OK - arret climate envoye" : "ERREUR - bus CAN occupe, reessaie dans quelques secondes";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// temp_byte: (temp_celsius - 16) * 2   ex: 22C -> 0x0C, 25C -> 0x12
static bool do_heat_sequence(uint8_t temp_byte)
{
    if (!take_twai_mutex_or_fail("heat_on")) return false;
    transmitting = true;
    bool aborted = false;

    ESP_LOGI(TAG, "Bascule sur CAR-CAN pour transmission (GPIO26/14)");
    twai_stop();
    twai_driver_uninstall();

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CARCAN_TX_GPIO, CARCAN_RX_GPIO, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK && twai_start() == ESP_OK) {
        carcan_wakeup();
        // Laisse le BCM le temps de sortir de veille avant de commencer les
        // repetitions - 20ms etait probablement trop court sur bus froid
        // (voir gel climate du 2026-07-29, trames non-acquittees -> bus-off).
        vTaskDelay(pdMS_TO_TICKS(250));

        uint8_t data[4] = {0x4E, 0x08, temp_byte, 0x00};
        for (int i = 0; i < CARCAN_REPEAT_COUNT_HEAT; i++) {
            can_send_frame(0x56E, data, 4);
            vTaskDelay(pdMS_TO_TICKS(100));
            if (twai_is_bus_off()) {
                ESP_LOGW(TAG, "BUS-OFF pendant sequence climate ON (frame %d/%d) - abandon repetitions", i + 1, CARCAN_REPEAT_COUNT_HEAT);
                push_web_log("Climate ON: BUS-OFF pendant sequence - abandon");
                aborted = true;
                break;
            }
        }

        if (!aborted) {
            ESP_LOGI(TAG, "Climate ON envoye sur CAR-CAN (temp_byte=0x%02X, %dx @ 100ms)", temp_byte, CARCAN_REPEAT_COUNT_HEAT);
            push_web_log("Climate ON envoye");

            // Trame de cloture (source: vehicle_nissanleaf.cpp OVMS, RemoteCommandTimer
            // + CcDisableTimer). OVMS envoie TOUJOURS cette trame ~1 sec apres la fin
            // des repetitions ENABLE, meme si l'utilisateur n'a jamais demande d'arret
            // manuel. Sans elle le BCM ne referme jamais proprement son cycle
            // d'activation interne, d'ou la reprise spontanee du climate plus tard.
            // CONFIRME SUR LE TERRAIN comme le vrai fix (testé avec succès).
            vTaskDelay(pdMS_TO_TICKS(1000));
            uint8_t auto_disable[4] = {0x46, 0x08, 0x32, 0x00};
            can_send_frame(0x56E, auto_disable, 4);
            ESP_LOGI(TAG, "Trame de cloture AUTO_DISABLE envoyee (46 08 32 00)");
            push_web_log("Trame de cloture AUTO_DISABLE envoyee");
        }
    } else {
        ESP_LOGE(TAG, "Echec passage CAR-CAN NORMAL");
        push_web_log("ERREUR: passage CAR-CAN echoue - commande non envoyee");
        aborted = true;
    }

    twai_stop();
    twai_driver_uninstall();

    // retour en ecoute EV-CAN
    twai_general_config_t g_config2 = TWAI_GENERAL_CONFIG_DEFAULT(EVCAN_TX_GPIO, EVCAN_RX_GPIO, TWAI_MODE_LISTEN_ONLY);
    esp_err_t r_evcan = (twai_driver_install(&g_config2, &t_config, &f_config) == ESP_OK) ? twai_start() : ESP_FAIL;
    if (r_evcan == ESP_OK) {
        ESP_LOGI(TAG, "Retour en ecoute EV-CAN");
    } else {
        ESP_LOGE(TAG, "Echec retour en ecoute EV-CAN - monitoring probablement mort, reboot recommande");
        push_web_log("ERREUR: retour EV-CAN echoue - reboot recommande");
    }

    transmitting = false;
    xSemaphoreGive(twai_mutex);

    // Session climate ouverte seulement si la sequence a vraiment ete envoyee -
    // le timer natif du vehicule (15-20 min, confirme sur le terrain, ne
    // redemarre pas) ferme la session lui-meme.
    if (!aborted) {
        g_climate_active = true;
        climate_state_persist(true);
    }
    return !aborted;
}

// Lock/unlock - payloads confirmes dans vehicle_nissanleaf.cpp
static bool do_lock_sequence(bool lock)
{
    if (!take_twai_mutex_or_fail(lock ? "lock" : "unlock")) return false;
    transmitting = true;
    bool aborted = false;

    ESP_LOGI(TAG, "Bascule sur CAR-CAN pour %s", lock ? "LOCK" : "UNLOCK");
    twai_stop();
    twai_driver_uninstall();

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CARCAN_TX_GPIO, CARCAN_RX_GPIO, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK && twai_start() == ESP_OK) {
        carcan_wakeup();
        // Laisse le BCM le temps de sortir de veille avant de commencer les
        // repetitions - 20ms etait probablement trop court sur bus froid
        // (voir gel climate du 2026-07-29, trames non-acquittees -> bus-off).
        vTaskDelay(pdMS_TO_TICKS(250));

        uint8_t data[4];
        if (lock) {
            data[0] = 0x60; data[1] = 0x80; data[2] = 0x00; data[3] = 0x00;
        } else {
            data[0] = 0x11; data[1] = 0x00; data[2] = 0x00; data[3] = 0x00;
        }
        for (int i = 0; i < CARCAN_REPEAT_COUNT_LOCK; i++) {
            can_send_frame(0x56E, data, 4);
            vTaskDelay(pdMS_TO_TICKS(100));
            if (twai_is_bus_off()) {
                ESP_LOGW(TAG, "BUS-OFF pendant sequence (frame %d/%d) - abandon repetitions", i + 1, CARCAN_REPEAT_COUNT_LOCK);
                push_web_log("BUS-OFF pendant sequence - abandon repetitions");
                break;
            }
        }
        ESP_LOGI(TAG, "%s envoye sur CAR-CAN", lock ? "LOCK" : "UNLOCK");
        push_web_log(lock ? "LOCK envoye" : "UNLOCK envoye");
    } else {
        ESP_LOGE(TAG, "Echec passage CAR-CAN NORMAL");
        push_web_log("ERREUR: passage CAR-CAN echoue - commande non envoyee");
        aborted = true;
    }

    twai_stop();
    twai_driver_uninstall();

    twai_general_config_t g_config2 = TWAI_GENERAL_CONFIG_DEFAULT(EVCAN_TX_GPIO, EVCAN_RX_GPIO, TWAI_MODE_LISTEN_ONLY);
    if ((twai_driver_install(&g_config2, &t_config, &f_config) != ESP_OK) || (twai_start() != ESP_OK)) {
        ESP_LOGE(TAG, "Echec retour en ecoute EV-CAN - monitoring probablement mort, reboot recommande");
        push_web_log("ERREUR: retour EV-CAN echoue - reboot recommande");
    }

    transmitting = false;
    xSemaphoreGive(twai_mutex);
    return !aborted;
}

// Lecture ponctuelle de l'odometre (0x5C5, CAR-CAN) - a la demande seulement,
// jamais en continu. Contrairement a do_lock_sequence(), c'est une operation
// purement d'ecoute (aucune trame de commande envoyee a part le reveil), donc
// on peut se permettre une fenetre plus longue puisque l'utilisateur attend
// deja un resultat explicite. Meme essai deja tente une fois dans une fenetre
// de 300ms post-lock/unlock (voir memoire projet_carcan_passive_telemetry_idea) :
// rien capte. Hypothese: le combine d'instruments ne diffuse peut-etre que
// vehicule reellement eveille (contact accessoire/marche) - une fenetre plus
// longue avec reveils repetes peut aider si c'est juste une histoire de delai,
// mais pourrait tout autant echouer si le module a besoin du contact reel.
// Bit layout source: OVMS vehicle_nissanleaf.cpp IncomingFrameCan2, case 0x5c5.
#define ODOMETER_LISTEN_WINDOW_US (3000 * 1000) // 3s au total
#define ODOMETER_WAKEUP_INTERVAL_US (500 * 1000) // renvoie le reveil toutes les 500ms

static bool do_read_odometer_sequence(void)
{
    if (!take_twai_mutex_or_fail("odometer")) return false;
    transmitting = true;
    bool captured = false;

    ESP_LOGI(TAG, "Bascule sur CAR-CAN pour lecture odometre");
    twai_stop();
    twai_driver_uninstall();

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CARCAN_TX_GPIO, CARCAN_RX_GPIO, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK && twai_start() == ESP_OK) {
        carcan_wakeup();
        vTaskDelay(pdMS_TO_TICKS(250));

        int64_t deadline = esp_timer_get_time() + ODOMETER_LISTEN_WINDOW_US;
        int64_t next_wakeup = esp_timer_get_time() + ODOMETER_WAKEUP_INTERVAL_US;
        while (esp_timer_get_time() < deadline) {
            if (twai_is_bus_off()) {
                ESP_LOGW(TAG, "BUS-OFF pendant lecture odometre - abandon");
                push_web_log("BUS-OFF pendant lecture odometre - abandon");
                break;
            }
            if (esp_timer_get_time() >= next_wakeup) {
                carcan_wakeup();
                next_wakeup = esp_timer_get_time() + ODOMETER_WAKEUP_INTERVAL_US;
            }
            twai_message_t rx;
            if (twai_receive(&rx, pdMS_TO_TICKS(100)) == ESP_OK &&
                rx.identifier == 0x5C5 && rx.data_length_code >= 4) {
                g_last_odometer_km = ((int32_t)rx.data[1] << 16) | ((int32_t)rx.data[2] << 8) | rx.data[3];
                telemetry_state_persist(); // immediat - ne pas attendre le prochain cycle periodique
                ESP_LOGI(TAG, "0x5C5 recu: odometre=%ld", (long)g_last_odometer_km);
                char log[48];
                snprintf(log, sizeof(log), "Odometre lu: %ld km (suppose)", (long)g_last_odometer_km);
                push_web_log(log);
                captured = true;
                break;
            }
        }
        if (!captured) {
            push_web_log("Odometre: aucune trame 0x5C5 recue (module probablement endormi)");
        }
    } else {
        ESP_LOGE(TAG, "Echec passage CAR-CAN NORMAL (odometre)");
        push_web_log("ERREUR: passage CAR-CAN echoue - lecture odometre annulee");
    }

    twai_stop();
    twai_driver_uninstall();

    twai_general_config_t g_config2 = TWAI_GENERAL_CONFIG_DEFAULT(EVCAN_TX_GPIO, EVCAN_RX_GPIO, TWAI_MODE_LISTEN_ONLY);
    if ((twai_driver_install(&g_config2, &t_config, &f_config) != ESP_OK) || (twai_start() != ESP_OK)) {
        ESP_LOGE(TAG, "Echec retour en ecoute EV-CAN - monitoring probablement mort, reboot recommande");
        push_web_log("ERREUR: retour EV-CAN echoue - reboot recommande");
    }

    transmitting = false;
    xSemaphoreGive(twai_mutex);
    return captured;
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    touch_activity();
    const char *resp = "OK - redemarrage en cours";
    httpd_resp_send(req, resp, strlen(resp));
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t odometer_handler(httpd_req_t *req)
{
    touch_activity();
    bool ok = do_read_odometer_sequence();
    char resp[64];
    if (ok) {
        snprintf(resp, sizeof(resp), "ODOMETER km:%ld", (long)g_last_odometer_km);
    } else {
        snprintf(resp, sizeof(resp), "ERREUR - odometre non capte (voir logs)");
    }
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t lock_handler(httpd_req_t *req)
{
    touch_activity();
    bool ok = do_lock_sequence(true);
    const char *resp = ok ? "OK - verrouillage envoye" : "ERREUR - bus CAN occupe, reessaie dans quelques secondes";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t unlock_handler(httpd_req_t *req)
{
    touch_activity();
    bool ok = do_lock_sequence(false);
    const char *resp = ok ? "OK - deverrouillage envoye" : "ERREUR - bus CAN occupe, reessaie dans quelques secondes";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t heat_handler(httpd_req_t *req)
{
    touch_activity();
    char query[64];
    float temp = 22.0f;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char temp_str[8];
        if (httpd_query_key_value(query, "temp", temp_str, sizeof(temp_str)) == ESP_OK) {
            temp = atof(temp_str);
        }
    }

    if (temp < 16.0f) temp = 16.0f;
    if (temp > 32.0f) temp = 32.0f;

    uint8_t temp_byte = (uint8_t)((temp - 16.0f) * 2.0f + 0.5f);  // arrondi
    ESP_LOGI(TAG, "Requete web: chauffer a %.1f C (byte 0x%02X)", temp, temp_byte);

    bool ok = do_heat_sequence(temp_byte);

    char resp[64];
    if (ok) {
        snprintf(resp, sizeof(resp), "OK - sequence envoyee pour %.1f C", temp);
    } else {
        snprintf(resp, sizeof(resp), "ERREUR - bus CAN occupe, reessaie dans quelques secondes");
    }
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t monitor_mode_handler(httpd_req_t *req)
{
    touch_activity();
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char mode[8];
        if (httpd_query_key_value(query, "mode", mode, sizeof(mode)) == ESP_OK) {
            monitor_verbose = (strcmp(mode, "all") == 0);
        }
    }
    const char *resp = monitor_verbose ? "verbose" : "filtre";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    touch_activity();
    char resp[256];
    snprintf(resp, sizeof(resp),
             "fw_version:%s boot_reason:%s uptime_s:%lld heap_free:%lu heap_min_free:%lu climate_active:%d",
             FW_VERSION,
             g_boot_reason,
             esp_timer_get_time() / 1000000LL,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             g_climate_active ? 1 : 0);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// ---------------------------------------------------------------------
// Page principale (monitor CAN live)
// ---------------------------------------------------------------------
static const char *html_page =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1'>"
    "<title>LeafCAN Monitor</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{background:#111;color:#0f0;font-family:monospace;margin:0;padding:0;font-size:16px}"
    "#toolbar{position:sticky;top:0;background:#111;padding:10px;border-bottom:1px solid #333;z-index:1000}"
    "#log{white-space:pre-wrap;font-size:12px;padding:10px;height:60vh;overflow-y:auto;overflow-x:hidden;word-break:break-word}"
    "h1{color:#fff;font-size:16px;margin:0 0 8px 0}"
    "#status{color:#888;font-size:12px;margin-bottom:8px}"
    "a{color:#6cf}"
    "button{background:#2a6;color:#fff;border:none;padding:12px 16px;font-size:15px;border-radius:6px;margin-right:6px;margin-bottom:8px;min-height:44px;touch-action:manipulation}"
    "button.off{background:#555}"
    "#dash{display:flex;flex-wrap:wrap;gap:8px;padding:10px}"
    "#dash{border-bottom:1px solid #333}"
    ".dcard{background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:8px 14px;flex:1 1 42%;min-width:110px}"
    ".dlabel{color:#888;font-size:11px;text-transform:uppercase}"
    ".dval{color:#6f6;font-size:18px;font-weight:bold;margin-top:2px}"
    "input{background:#222;color:#eee;border:1px solid #444;padding:10px;border-radius:4px;margin-right:6px;font-size:15px}"
    "input[type=range]{width:100%;max-width:100%;height:32px;margin-top:6px}"
    "#filter{width:100%;max-width:280px}"
    "@media (max-width:480px){button{flex:1 1 44%}#toolbar div{display:flex;flex-wrap:wrap}}"
    "</style></head><body>"
    "<div id='toolbar'>"
    "<h1>Leaf EV-CAN Monitor <span style='color:#888;font-size:11px;font-weight:normal'>" FW_VERSION "</span></h1>"
    "<div id='status'><span id='statusText'>connexion...</span> <span id='canDot' title='Trafic CAN' style='display:inline-block;width:10px;height:10px;border-radius:50%;background:#555;margin-left:6px;vertical-align:middle'></span> <span id='canDotLabel' style='color:#666'>CAN --</span></div>"
    "<p><a href='/update' onclick='es.close()'>-&gt; Mettre a jour le firmware (OTA)</a></p>"
    "<div>"
    "<button onclick=\"sendCommand('/heat?temp='+document.getElementById('tempSlider').value, this, 13)\">Chauffer</button>"
    "<button onclick=\"sendCommand('/heat_off', this, 2)\" style='background:#666'>Annuler chauffage</button>"
    "<div style='margin:6px 0'>Temp: <b id='tempVal'>22.0</b>&deg;C</div>"
    "<input type='range' id='tempSlider' min='16' max='32' step='0.5' value='22' "
    "oninput=\"document.getElementById('tempVal').textContent=this.value\"><br>"
    "<button onclick=\"sendCommand('/unlock', this, 2)\" style='background:#26a'>D&eacute;barrer</button>"
    "<button onclick=\"sendCommand('/lock', this, 2)\" style='background:#a62'>Barrer</button>"
    "<button onclick=\"sendCommand('/odometer', this, 4)\" style='background:#666'>Odom&egrave;tre</button>"
    "<button onclick=\"if(confirm('Redemarrer l ESP32 ?'))sendCommand('/reboot', this, 6)\" style='background:#833'>Red&eacute;marrer ESP32</button>"
    "<button id='btnLog' onclick='toggleLog()' style='background:#555'>Log: masqu&eacute;</button>"
    "</div>"
    "</div>"
    "<div id='dash'>"
    "<div class='dcard'><div class='dlabel'>Batterie SOC</div><div class='dval' id='d_soc'>--</div></div>"
    "<div class='dcard'><div class='dlabel'>SOH</div><div class='dval' id='d_soh'>--</div></div>"
    "<div class='dcard'><div class='dlabel'>Climatisation</div><div class='dval' id='d_cc'>--</div></div>"
    "<div class='dcard'><div class='dlabel'>Exterieur</div><div class='dval' id='d_amb'>--</div></div>"
    "<div class='dcard'><div class='dlabel'>Vehicule</div><div class='dval' id='d_veh'>--</div></div>"
    "<div class='dcard'><div class='dlabel'>Odometre</div><div class='dval' id='d_odo'>--</div></div>"
    "</div>"
    "<div id='logSection' style='display:none'>"
    "<div id='log'></div>"
    "<div style='padding:10px'>"
    "<button id='btnPause' onclick='togglePause()'>Pause</button>"
    "<button id='btnScroll' onclick='toggleScroll()'>Auto-scroll: ON</button>"
    "<button id='btnMode' onclick='toggleMode()'>Mode: Filtre</button>"
    "<input id='filter' placeholder='Filtrer par ID (ex: 54C)' oninput='applyFilter()'>"
    "</div>"
    "</div>"
    "<script>"
    "var log=document.getElementById('log');"
    "var st=document.getElementById('statusText');"
    "var paused=false, autoScroll=true, verbose=false, logVisible=false;"
    "var lines=[];"
    "function togglePause(){paused=!paused;document.getElementById('btnPause').textContent=paused?'Reprendre':'Pause';}"
    "function toggleScroll(){autoScroll=!autoScroll;document.getElementById('btnScroll').textContent='Auto-scroll: '+(autoScroll?'ON':'OFF');}"
    "function toggleLog(){"
    "logVisible=!logVisible;"
    "document.getElementById('logSection').style.display=logVisible?'block':'none';"
    "document.getElementById('btnLog').textContent='Log: '+(logVisible?'affich\\u00e9':'masqu\\u00e9');"
    "if(logVisible)render();"
    "}"
    "var canDotTimer;"
    "function pulseCan(){"
    "var d=document.getElementById('canDot'),l=document.getElementById('canDotLabel');"
    "d.style.background='#2f2';l.textContent='CAN LIVE';l.style.color='#6f6';"
    "clearTimeout(canDotTimer);"
    "canDotTimer=setTimeout(function(){d.style.background='#555';l.textContent='CAN silence';l.style.color='#666';},5000);"
    "}"
    "function toggleMode(){"
    "verbose=!verbose;"
    "fetch('/monitor?mode='+(verbose?'all':'filtre'));"
    "document.getElementById('btnMode').textContent='Mode: '+(verbose?'Complet':'Filtre');"
    "}"
    "function applyFilter(){render();}"
    "function render(){"
    "var f=document.getElementById('filter').value.toUpperCase();"
    "var shown=f?lines.filter(l=>l.toUpperCase().includes(f)):lines;"
    "log.textContent=shown.slice(-500).join('\\n');"
    "if(autoScroll)log.scrollTop=log.scrollHeight;"
    "}"
    "var es;"
    "function connectSSE(){"
    "es=new EventSource('/events');"
    "es.onopen=function(){st.textContent='connecte';};"
    "es.onerror=function(){st.textContent='deconnecte - reconnexion...';};"
    "es.onmessage=function(e){"
    "if(paused)return;"
    "if(e.data.indexOf('ID:0x')!==-1)pulseCan();"
    "lines.push(e.data);"
    "if(lines.length>1000)lines=lines.slice(-1000);"
    "updateDash(e.data);"
    "if(logVisible)render();"
    "};"
    "}"
    "connectSSE();"
    "window.addEventListener('pagehide',function(){if(es)es.close();});"
    "function sendCommand(url, btn, expectSec){"
    "var orig=btn?btn.textContent:null;"
    "var remain=expectSec||2;"
    "var tick;"
    "if(btn){btn.disabled=true;btn.textContent='Envoi... '+remain+'s';btn.style.opacity='0.6';"
    "tick=setInterval(function(){remain--;if(remain>=0){btn.textContent='Envoi... '+remain+'s';}else{btn.textContent='Envoi... +'+(-remain)+'s';}},1000);}"
    "st.textContent='envoi en cours...';"
    "es.close();"
    "fetch(url).then(r=>r.text()).then(function(t){"
    "alert(t);"
    "connectSSE();"
    "}).catch(function(e){"
    "alert('Erreur: '+e);"
    "connectSSE();"
    "}).finally(function(){"
    "if(tick)clearInterval(tick);"
    "if(btn){btn.disabled=false;btn.textContent=orig;btn.style.opacity='1';}"
    "});"
    "}"
    "var lastUpdate={};"
    "var dashIds=['d_soc','d_soh','d_cc','d_amb','d_veh','d_odo'];"
    "function setField(id,val){"
    "var el=document.getElementById(id);"
    "el.textContent=val;"
    "el.style.opacity='1';"
    "lastUpdate[id]=Date.now();"
    "}"
    "function updateDash(line){"
    "var m;"
    "if(m=line.match(/SOC:([\\d.]+)%/))setField('d_soc',m[1]+'%');"
    "if(m=line.match(/SOH:(\\d+)%/))setField('d_soh',m[1]+'%');"
    "if(m=line.match(/Climate:(\\S+)/))setField('d_cc',m[1]);"
    "if(m=line.match(/Exterieur:([\\d.-]+C)/))setField('d_amb',m[1]);"
    "if(m=line.match(/Vehicule:(\\S+)/))setField('d_veh',m[1]);"
    "if(m=line.match(/Odometre lu: (\\d+) km/))setField('d_odo',m[1]+' km');"
    "}"
    "setInterval(function(){"
    "var now=Date.now();"
    "dashIds.forEach(function(id){"
    "var t=lastUpdate[id];"
    "var el=document.getElementById(id);"
    "if(t && (now-t)>20000){el.style.opacity='0.35';}"
    "});"
    "},5000);"
    "</script></body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, html_page, strlen(html_page));
}

static esp_err_t events_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    can_msg_t item;
    char buf[200];

    while (1) {
        // Tant qu'une page monitor reste ouverte sur /events, on repousse le
        // deep sleep - quelqu'un est probablement en train de regarder.
        touch_activity();
        if (xQueueReceive(can_queue, &item, pdMS_TO_TICKS(5000)) == pdTRUE) {
            int len = snprintf(buf, sizeof(buf), "data: %s\n\n", item.text);
            if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) break;
        } else {
            const char *hb = ": ping\n\n";
            if (httpd_resp_send_chunk(req, hb, strlen(hb)) != ESP_OK) break;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ---------------------------------------------------------------------
// Page OTA - upload du .bin depuis le navigateur
// ---------------------------------------------------------------------
static const char *update_page =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1'>"
    "<title>LeafCAN OTA</title>"
    "<style>"
    "body{background:#111;color:#eee;font-family:sans-serif;padding:20px}"
    "h1{color:#fff;font-size:16px}"
    "#result{margin-top:15px;color:#6cf;white-space:pre-wrap}"
    "button{background:#2a2;color:#fff;border:none;padding:12px 18px;font-size:15px;border-radius:4px;min-height:44px}"
    "input[type=file]{font-size:15px;padding:6px 0}"
    "progress{width:100%;margin-top:10px;height:20px}"
    "</style></head><body>"
    "<h1>LeafCAN - Mise a jour firmware (OTA)</h1>"
    "<p>Selectionne le fichier .bin genere par 'idf.py build' (build/leaf-fw.bin)</p>"
    "<input type='file' id='f' accept='.bin'><br><br>"
    "<button onclick='upload()'>Flasher</button>"
    "<progress id='p' value='0' max='100' style='display:none'></progress>"
    "<div id='result'></div>"
    "<script>"
    "function upload(){"
    "var f=document.getElementById('f').files[0];"
    "if(!f){alert('Choisis un fichier .bin');return;}"
    "var r=document.getElementById('result');"
    "var p=document.getElementById('p');"
    "p.style.display='block';"
    "r.textContent='Envoi en cours...';"
    "var xhr=new XMLHttpRequest();"
    "xhr.open('POST','/update',true);"
    "xhr.upload.onprogress=function(e){"
    "if(e.lengthComputable){p.value=(e.loaded/e.total)*100;}"
    "};"
    "xhr.onload=function(){"
    "r.textContent=xhr.responseText;"
    "if(xhr.status==200){r.textContent+='\\n\\nRedemarrage... reconnecte-toi au WiFi dans 10s.';}"
    "};"
    "xhr.onerror=function(){r.textContent='Erreur de transfert';};"
    "xhr.send(f);"
    "}"
    "</script></body></html>";

static esp_err_t update_page_handler(httpd_req_t *req)
{
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, update_page, strlen(update_page));
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    touch_activity();
    char buf[1024];
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Pas de partition OTA disponible");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA -> ecriture dans partition '%s' offset 0x%lx",
             update_partition->label, (unsigned long)update_partition->address);

    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int total_written = 0;

    while (remaining > 0) {
        touch_activity();  // upload lent (WiFi faible) ne doit pas se faire couper par le deep sleep
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);

        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Erreur reception OTA");
            esp_ota_abort(ota_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        remaining -= received;
        total_written += received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        const char *msg = (err == ESP_ERR_OTA_VALIDATE_FAILED)
            ? "ERREUR: image invalide (mauvais .bin ou corrompu)"
            : "ERREUR: echec finalisation OTA";
        httpd_resp_send(req, msg, strlen(msg));
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA reussi - %d octets ecrits. Redemarrage...", total_written);
    char resp[128];
    snprintf(resp, sizeof(resp), "OK - %d octets flashes avec succes.", total_written);
    httpd_resp_send(req, resp, strlen(resp));

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_open_sockets = 10;
    config.stack_size = 8192;
    config.recv_wait_timeout = 10;
    // Defaut ESP-IDF = 8. On enregistre 12 URI (/,/events,/update GETx2,/heat,
    // /heat_off,/monitor,/lock,/unlock,/status,/odometer,/reboot) - au-dela
    // du max, le dernier handler enregistre echoue silencieusement -> 404
    // "Nothing matches the given URI" au clic (voir episode odometer 2026-08-14).
    // Marge ajoutee pour futurs endpoints.
    config.max_uri_handlers = 14;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri     = { .uri = "/",        .method = HTTP_GET,  .handler = root_handler };
        httpd_uri_t events_uri   = { .uri = "/events",  .method = HTTP_GET,  .handler = events_handler };
        httpd_uri_t update_get   = { .uri = "/update",  .method = HTTP_GET,  .handler = update_page_handler };
        httpd_uri_t update_post  = { .uri = "/update",  .method = HTTP_POST, .handler = update_post_handler };
        httpd_uri_t heat_uri     = { .uri = "/heat",    .method = HTTP_GET,  .handler = heat_handler };
        httpd_uri_t heat_off_uri = { .uri = "/heat_off",.method = HTTP_GET,  .handler = heat_off_handler };
        httpd_uri_t monitor_uri  = { .uri = "/monitor", .method = HTTP_GET,  .handler = monitor_mode_handler };
        httpd_uri_t lock_uri     = { .uri = "/lock",    .method = HTTP_GET,  .handler = lock_handler };
        httpd_uri_t unlock_uri   = { .uri = "/unlock",  .method = HTTP_GET,  .handler = unlock_handler };
        httpd_uri_t status_uri   = { .uri = "/status",  .method = HTTP_GET,  .handler = status_handler };
        httpd_uri_t odometer_uri = { .uri = "/odometer",.method = HTTP_GET,  .handler = odometer_handler };
        httpd_uri_t reboot_uri   = { .uri = "/reboot",  .method = HTTP_GET,  .handler = reboot_handler };

        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &events_uri);
        httpd_register_uri_handler(server, &update_get);
        httpd_register_uri_handler(server, &update_post);
        httpd_register_uri_handler(server, &heat_uri);
        httpd_register_uri_handler(server, &heat_off_uri);
        httpd_register_uri_handler(server, &monitor_uri);
        httpd_register_uri_handler(server, &lock_uri);
        httpd_register_uri_handler(server, &unlock_uri);
        httpd_register_uri_handler(server, &status_uri);
        httpd_register_uri_handler(server, &odometer_uri);
        httpd_register_uri_handler(server, &reboot_uri);

        ESP_LOGI(TAG, "Serveur web demarre (avec OTA sur /update)");
    } else {
        ESP_LOGE(TAG, "Echec demarrage serveur web");
    }
    return server;
}

// ---------------------------------------------------------------------
// Tache CAN
// ---------------------------------------------------------------------
static void can_task(void *arg)
{
    twai_general_config_t g_config =
        TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO, TWAI_MODE_LISTEN_ONLY);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
        ESP_LOGE(TAG, "TWAI install failed");
        push_web_log("ERREUR FATALE: TWAI install echoue au demarrage - monitoring mort, reboot requis");
        vTaskDelete(NULL);
        return;
    }
    if (twai_start() != ESP_OK) {
        ESP_LOGE(TAG, "TWAI start failed");
        push_web_log("ERREUR FATALE: TWAI start echoue au demarrage - monitoring mort, reboot requis");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "CAN logger actif - 500kbps listen-only (GPIO32/33)");

    uint32_t frame_count = 0;
    int64_t last_busoff_check_us = 0;

    while (1) {
        if (transmitting || bus_switch_requested) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        xSemaphoreTake(twai_mutex, portMAX_DELAY);
        twai_message_t message;
        esp_err_t rx_err = twai_receive(&message, pdMS_TO_TICKS(1000));
        xSemaphoreGive(twai_mutex);
        // Fenetre explicite pour le scheduler : sur bus charge, take/give
        // immediatement suivi d'un nouveau take ne garantit pas que la tache
        // en attente (sequence lock/unlock, recovery climate au boot) recoive
        // le mutex avant que can_task le reprenne lui-meme (SMP dual-core,
        // pas de bascule forcee entre coeurs). Cause identifiee des echecs
        // "bus occupe/bloque" systematiques du 2026-07-29 (mutex jamais libre
        // assez longtemps pour qu'un waiter le gagne, meme sans HTTP implique -
        // vu au boot entre app_main et can_task uniquement).
        vTaskDelay(pdMS_TO_TICKS(1));
        if (rx_err == ESP_OK) {
            frame_count++;
            can_msg_t item;
            int pos = snprintf(item.text, sizeof(item.text),
                                "[%lu] ID:0x%03lX DLC:%d DATA:",
                                (unsigned long)frame_count,
                                (unsigned long)message.identifier,
                                message.data_length_code);
            for (int i = 0; i < message.data_length_code && pos < (int)sizeof(item.text) - 4; i++) {
                pos += snprintf(item.text + pos, sizeof(item.text) - pos, " %02X", message.data[i]);
            }
            // >=7 suffit: le decodeur ne lit jamais au-dela de data[6], et
            // twai_message_t.data fait toujours 8 octets (>=8 ignorait a
            // tort de vraies trames DLC=7, ex: 0x54C climat/temp exterieure).
            if (message.data_length_code >= 7) {
                char decoded[48];
                decode_battery_frame(message.identifier, message.data, decoded, sizeof(decoded));
                if (decoded[0] != '\0' && pos < (int)sizeof(item.text) - 1) {
                    strncat(item.text, decoded, sizeof(item.text) - pos - 1);
                }
            }

            // filtre serveur: en mode non-verbose, ne pousse que les IDs utiles
            bool interesting = (message.identifier == 0x54C || message.identifier == 0x54F ||
                                 message.identifier == 0x54A || message.identifier == 0x54B ||
                                 message.identifier == 0x55B || message.identifier == 0x5BC ||
                                 message.identifier == 0x1DB || message.identifier == 0x1D4 ||
                                 message.identifier == 0x11A);
            if (monitor_verbose || interesting) {
                xQueueSend(can_queue, &item, 0);
            }
            printf("%s\n", item.text);
        }

        // Verif bus-off limitee a 1x/sec : elle n'a pas besoin de tourner a
        // chaque itération (potentiellement des centaines de fois/sec sur bus
        // charge) - ca ne fait qu'ajouter des cycles take/give inutiles au
        // meme mutex sans benefice (un bus-off reel persiste sur bien plus
        // d'une seconde).
        int64_t now_us = esp_timer_get_time();
        if (!transmitting && (now_us - last_busoff_check_us) >= 1000000LL) {
            last_busoff_check_us = now_us;
            xSemaphoreTake(twai_mutex, portMAX_DELAY);
            twai_status_info_t status;
            twai_get_status_info(&status);
            if (status.state == TWAI_STATE_BUS_OFF) {
                // twai_initiate_recovery()+twai_start() seul ne suffisait pas de
                // facon fiable apres plusieurs heures (bus reste coince, corrige
                // seulement par le cycle uninstall/reinstall complet declenche par
                // une commande lock/unlock) - on fait donc directement le meme
                // reinstall complet ici plutot que de compter sur la recovery.
                ESP_LOGW(TAG, "BUS OFF sur EV-CAN - reinstall complet du driver");
                push_web_log("EV-CAN BUS-OFF detecte - reinstall driver");
                twai_stop();
                twai_driver_uninstall();
                twai_general_config_t g_config_r = TWAI_GENERAL_CONFIG_DEFAULT(EVCAN_TX_GPIO, EVCAN_RX_GPIO, TWAI_MODE_LISTEN_ONLY);
                twai_timing_config_t t_config_r = TWAI_TIMING_CONFIG_500KBITS();
                twai_filter_config_t f_config_r = TWAI_FILTER_CONFIG_ACCEPT_ALL();
                if ((twai_driver_install(&g_config_r, &t_config_r, &f_config_r) != ESP_OK) || (twai_start() != ESP_OK)) {
                    ESP_LOGE(TAG, "Echec reinstall EV-CAN apres BUS OFF");
                    push_web_log("ERREUR: reinstall EV-CAN echoue apres BUS-OFF");
                }
            }
            xSemaphoreGive(twai_mutex);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// ---------------------------------------------------------------------
// UART vers Boron - relais des commandes recues du Particle Cloud (LTE)
// Meme fonctions que le dashboard web (do_lock_sequence, do_heat_sequence,
// do_heat_off_sequence) - protocole texte simple, une commande par ligne,
// terminee par '\n'. Reponse "OK"/"ERR" (ou "STATUS ..." / "PONG").
// ---------------------------------------------------------------------
static void uart_send_line(const char *s)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s\n", s);
    uart_write_bytes(BORON_UART_NUM, buf, n);

    // Visible dans le monitor web (/) - permet de confirmer que l'ESP32 tente
    // bien d'envoyer une reponse au Boron, sans avoir besoin du moniteur USB.
    char log[150];
    snprintf(log, sizeof(log), "UART->Boron: '%s'", s);
    push_web_log(log);
}

static void uart_handle_command(char *cmd)
{
    // trim espaces/CR residuels
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len - 1] == '\r' || cmd[len - 1] == ' ')) {
        cmd[--len] = '\0';
    }
    if (len == 0) return;

    touch_activity();
    ESP_LOGI(TAG, "UART Boron -> commande: '%s'", cmd);
    char log[180];
    snprintf(log, sizeof(log), "UART<-Boron: '%s'", cmd);
    push_web_log(log);

    if (strcmp(cmd, "PING") == 0) {
        uart_send_line("PONG");

    } else if (strcmp(cmd, "LOCK") == 0) {
        uart_send_line(do_lock_sequence(true) ? "OK" : "ERR");

    } else if (strcmp(cmd, "UNLOCK") == 0) {
        uart_send_line(do_lock_sequence(false) ? "OK" : "ERR");

    } else if (strcmp(cmd, "HEAT_OFF") == 0) {
        uart_send_line(do_heat_off_sequence() ? "OK" : "ERR");

    } else if (strncmp(cmd, "HEAT", 4) == 0) {
        float temp = 22.0f;
        sscanf(cmd + 4, "%f", &temp);
        if (temp < 16.0f) temp = 16.0f;
        if (temp > 32.0f) temp = 32.0f;
        uint8_t temp_byte = (uint8_t)((temp - 16.0f) * 2.0f + 0.5f);  // arrondi
        uart_send_line(do_heat_sequence(temp_byte) ? "OK" : "ERR");

    } else if (strcmp(cmd, "STATUS") == 0) {
        // Pas d'indicateur de charge - aucune trame trouvee jusqu'ici (0x1D4
        // byte6, 0x390) n'est fiable sur ce vehicule. SOC+SOH suffisent.
        char soc_str[8], soh_str[8];
        if (g_last_soc >= 0.0f) snprintf(soc_str, sizeof(soc_str), "%.1f", g_last_soc);
        else snprintf(soc_str, sizeof(soc_str), "?");
        if (g_last_soh >= 0) snprintf(soh_str, sizeof(soh_str), "%d", g_last_soh);
        else snprintf(soh_str, sizeof(soh_str), "?");

        const char *vehicle_str = g_last_vehicle_on == 1 ? "ON" : (g_last_vehicle_on == 0 ? "OFF" : "?");

        // odo: derniere valeur connue (persistee NVS, voir do_read_odometer_
        // sequence) - pas une lecture fraiche, juste ce qu'on a garde en
        // memoire depuis la derniere fois qu'une lecture ODOMETER a reussi.
        char odo_str[12];
        if (g_last_odometer_km >= 0) snprintf(odo_str, sizeof(odo_str), "%ld", (long)g_last_odometer_km);
        else snprintf(odo_str, sizeof(odo_str), "?");

        char resp[144];
        snprintf(resp, sizeof(resp), "STATUS soc:%s soh:%s climate:%d vehicle:%s wifi:%d odo:%s",
                 soc_str, soh_str, g_climate_active ? 1 : 0, vehicle_str, g_wifi_on ? 1 : 0, odo_str);
        uart_send_line(resp);

    } else if (strcmp(cmd, "REBOOT") == 0) {
        // Reboot demande a distance - utile quand le monitoring EV-CAN est
        // mort (voir messages "reboot recommande" apres un echec de reinstall
        // TWAI post BUS-OFF) et qu'aucun acces USB n'est possible. Envoie OK
        // avant de redemarrer pour que le Boron ne timeout pas inutilement.
        uart_send_line("OK");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();

    } else if (strcmp(cmd, "ODOMETER") == 0) {
        bool ok = do_read_odometer_sequence();
        char resp[48];
        if (ok) snprintf(resp, sizeof(resp), "ODOMETER km:%ld", (long)g_last_odometer_km);
        else snprintf(resp, sizeof(resp), "ODOMETER km:?");
        uart_send_line(resp);

    } else if (strcmp(cmd, "WIFI_ON") == 0) {
        // Reactive juste le radio WiFi (config AP deja faite au boot par
        // wifi_init_softap) - pas besoin de tout reinitialiser.
        bool ok = esp_wifi_start() == ESP_OK;
        if (ok) { g_wifi_on = true; wifi_state_persist(true); }
        uart_send_line(ok ? "OK" : "ERR");

    } else if (strcmp(cmd, "WIFI_OFF") == 0) {
        // Coupe juste le radio - httpd/can_task/uart_task continuent de
        // tourner, simplement injoignables tant que le WiFi est coupe.
        bool ok = esp_wifi_stop() == ESP_OK;
        if (ok) { g_wifi_on = false; wifi_state_persist(false); }
        uart_send_line(ok ? "OK" : "ERR");

    } else {
        uart_send_line("ERR unknown_cmd");
    }
}

static void uart_task(void *arg)
{
    uart_config_t uart_config = {
        .baud_rate = BORON_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e1 = uart_param_config(BORON_UART_NUM, &uart_config);
    esp_err_t e2 = uart_set_pin(BORON_UART_NUM, BORON_UART_TX_GPIO, BORON_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    esp_err_t e3 = uart_driver_install(BORON_UART_NUM, 256, 256, 0, NULL, 0);

    ESP_LOGI(TAG, "UART Boron pret sur GPIO%d(TX)/GPIO%d(RX) @ %d bauds",
             BORON_UART_TX_GPIO, BORON_UART_RX_GPIO, BORON_UART_BAUD);

    // Visible depuis le dashboard web sans acces USB - confirme au boot que
    // uart_task a bien demarre (et sans erreur d'install driver/pins).
    char boot_log[100];
    snprintf(boot_log, sizeof(boot_log),
             "UART Boron pret GPIO%d(TX)/GPIO%d(RX) @%dbps (cfg:%d pin:%d install:%d)",
             BORON_UART_TX_GPIO, BORON_UART_RX_GPIO, BORON_UART_BAUD, e1, e2, e3);
    push_web_log(boot_log);

    char line[64];
    size_t pos = 0;

    while (1) {
        uint8_t byte;
        int n = uart_read_bytes(BORON_UART_NUM, &byte, 1, portMAX_DELAY);
        if (n <= 0) continue;

        if (byte == '\n') {
            line[pos] = '\0';
            uart_handle_command(line);
            pos = 0;
        } else {
            // Log du 1er octet de chaque nouvelle ligne, meme si elle ne se
            // termine jamais par '\n' (mismatch baudrate/bruit) - sans ca on
            // ne verrait jamais rien passer sur le fil cote monitor web.
            if (pos == 0) {
                char log[40];
                snprintf(log, sizeof(log), "UART<-Boron: 1er octet 0x%02X recu", byte);
                push_web_log(log);
            }
            if (pos < sizeof(line) - 1) {
                line[pos++] = (char)byte;
            } else {
                // ligne trop longue (bruit/desync) - on l'abandonne plutot que
                // de deborder ou de traiter une commande tronquee
                pos = 0;
            }
        }
    }
}

// Configure les deux sources de reveil (EXT0 Boron + timer 6h) et coupe le
// courant. N'a pas de valeur de retour: esp_deep_sleep_start() ne revient
// jamais, un reveil = un reboot complet (repart au tout debut d'app_main).
static void enter_deep_sleep(void)
{
    // Meme protocole que toute autre operation TWAI (take_twai_mutex_or_fail)
    // - sans ca, un twai_driver_uninstall() ici pourrait tomber en pleine
    // course avec can_task (twai_receive() en cours) ou une sequence lock/
    // heat en train de basculer sur CAR-CAN, et planter le driver ou laisser
    // le bus dans un etat incoherent. Si le bus est occupe, on renonce et on
    // reessaiera au prochain passage de la boucle d'inactivite (10s plus tard).
    if (!take_twai_mutex_or_fail("deep_sleep")) {
        ESP_LOGW(TAG, "Deep sleep differe - bus TWAI occupe, nouvel essai au prochain cycle");
        return;
    }

    ESP_LOGW(TAG, "Deep sleep: reveil EXT0 GPIO%d ou timer %lldh",
             WAKE_GPIO, DEEP_SLEEP_TIMER_US / 3600000000LL);
    push_web_log("Inactivite detectee - deep sleep dans 2s");
    telemetry_state_persist();
    vTaskDelay(pdMS_TO_TICKS(2000));  // laisse partir le dernier message SSE

    esp_wifi_stop();
    twai_stop();
    twai_driver_uninstall();

    esp_sleep_enable_ext0_wakeup(WAKE_GPIO, 1);
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_TIMER_US);
    esp_deep_sleep_start();
}

// Reveil periodique silencieux (timer 6h) : pas de WiFi, pas de webserver,
// pas de uart_task - juste une fenetre d'ecoute EV-CAN courte pour rafraichir
// SOC/SOH en NVS, puis retour immediat en deep sleep. Ne revient jamais.
static void timer_wake_refresh_and_sleep(void)
{
    telemetry_state_load();

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(EVCAN_TX_GPIO, EVCAN_RX_GPIO, TWAI_MODE_LISTEN_ONLY);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK && twai_start() == ESP_OK) {
        int64_t listen_start = esp_timer_get_time();
        while (esp_timer_get_time() - listen_start < TIMER_WAKE_LISTEN_US) {
            twai_message_t message;
            if (twai_receive(&message, pdMS_TO_TICKS(200)) == ESP_OK && message.data_length_code >= 7) {
                char dummy[48];
                decode_battery_frame(message.identifier, message.data, dummy, sizeof(dummy));
            }
        }
        twai_stop();
        twai_driver_uninstall();
    } else {
        ESP_LOGE(TAG, "Reveil timer: echec install TWAI pour ecoute courte");
    }

    telemetry_state_persist();

    esp_sleep_enable_ext0_wakeup(WAKE_GPIO, 1);
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_TIMER_US);
    esp_deep_sleep_start();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Demarrage LeafCAN WiFi Monitor + OTA");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();

    // Reveil periodique (6h) : chemin silencieux, ne demarre jamais WiFi ni
    // le webserver - voir project_deep_sleep_plan (decision explicite).
    if (wake_cause == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Reveil timer periodique - ecoute EV-CAN courte puis redort");
        timer_wake_refresh_and_sleep();
        return;  // jamais atteint
    }

    // Boot normal (power-on/USB) ou reveil EXT0 (Boron) : demarrage complet,
    // identique dans les deux cas.
    ESP_LOGI(TAG, "Cause de reveil: %s",
             wake_cause == ESP_SLEEP_WAKEUP_EXT0 ? "EXT0 (Boron)" : "boot normal");

    // Confirme que la partition qui vient de booter est valide
    // (evite un rollback si jamais l'app crash tot apres une future MAJ OTA)
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Partition active: %s", running->label);

    // Diagnostic reboot/crash - visible via /status et pousse sur le flux web
    // des que la page monitor se reconnecte (utile sans acces USB au vehicule).
    strncpy(g_boot_reason, reset_reason_str(esp_reset_reason()), sizeof(g_boot_reason) - 1);
    ESP_LOGW(TAG, "Raison du dernier reset: %s", g_boot_reason);

    can_queue = xQueueCreate(30, sizeof(can_msg_t));
    twai_mutex = xSemaphoreCreateMutex();

    can_msg_t boot_item;
    snprintf(boot_item.text, sizeof(boot_item.text), "BOOT reset_reason:%s", g_boot_reason);
    xQueueSend(can_queue, &boot_item, 0);

    // Dernieres valeurs connues (avant deep sleep) - evite un STATUS a "?"
    // juste apres un reveil EXT0, en attendant une nouvelle trame EV-CAN.
    telemetry_state_load();

    // uart_task en premier, avant WiFi/webserver : au reveil EXT0, le Boron
    // peut envoyer sa commande UART des que le pin de reveil est vu, sans
    // attendre que l'AP WiFi (esp_wifi_init/start, pas instantane) soit
    // monte. Sans ca, les octets envoyes trop tot par le Boron sont perdus
    // (uart_driver_install pas encore fait) - timeout cote Boron/webapp.
    xTaskCreate(uart_task, "uart_task", 4096, NULL, 5, NULL);

    wifi_init_softap();
    start_webserver();

    // Reapplique la preference WiFi persistee (voir wifi_state_persist) -
    // wifi_init_softap() vient de tout allumer inconditionnellement, ce qui
    // annulerait sinon un WIFI_OFF demande avant le dernier deep sleep.
    if (!wifi_state_load()) {
        ESP_LOGI(TAG, "Preference WiFi = OFF (persistee) - coupure du radio apres boot");
        esp_wifi_stop();
        g_wifi_on = false;
    } else {
        g_wifi_on = true;
    }

    xTaskCreate(can_task, "can_task", 4096, NULL, 5, NULL);

    // Recuperation apres reboot inattendu (crash/brownout) pendant une session
    // climate active : l'etat RAM (g_climate_active) est perdu au reset. On
    // relit le dernier etat connu en NVS et on renvoie un OFF de securite si
    // besoin. Delai pour laisser can_task installer le driver TWAI en premier.
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (climate_state_load()) {
        ESP_LOGW(TAG, "Etat climate actif retrouve apres redemarrage - OFF de securite envoye");
        can_msg_t recov_item;
        snprintf(recov_item.text, sizeof(recov_item.text), "BOOT recovery: climate actif au dernier etat connu, OFF envoye");
        xQueueSend(can_queue, &recov_item, 0);
        do_heat_off_sequence();
    }

    // Reveil eveille (boot normal ou EXT0) : reste actif tant qu'il y a de
    // l'activite (requete HTTP, commande UART, page monitor ouverte), puis
    // retourne en deep sleep apres INACTIVITY_TIMEOUT_US sans rien.
    touch_activity();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if ((esp_timer_get_time() - g_last_activity_us) > INACTIVITY_TIMEOUT_US) {
            enter_deep_sleep();
        }
    }
}
