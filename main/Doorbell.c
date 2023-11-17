/* Generic app */
/* Copyright ©2019 - 2022 Adrian Kennard, Andrews & Arnold Ltd.See LICENCE file for details .GPL 3.0 */
/* This has a wide range of example stuff in it that does not in itself warrant a separate project */
/* Including UART logging and debug for Daikin air-con */
/* Including display text and QR code */
/* Including SolarEdge monitor */
/* Including DEFCON mode - DEFCON/x on mqtt, or ?x on http, where x=1-5 for normal, 0 for special all on, 6-8 for all off, 9 for all off and quiet and no blink */

static const char TAG[] = "Generic";

#include "revk.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "gfx.h"
#include "iec18004.h"
#include <hal/spi_types.h>
#include <driver/gpio.h>

#ifdef	CONFIG_LWIP_DHCP_DOES_ARP_CHECK
#warning CONFIG_LWIP_DHCP_DOES_ARP_CHECK means DHCP is slow
#endif
#ifndef	CONFIG_LWIP_DHCP_RESTORE_LAST_IP
#warning CONFIG_LWIP_DHCP_RESTORE_LAST_IP may improve speed
#endif
#ifndef	CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
#warning CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP may speed boot
#endif
#if	CONFIG_BOOTLOADER_LOG_LEVEL > 0
#warning CONFIG_BOOTLOADER_LOG_LEVEL recommended to be no output
#endif

#define	MAXGPIO	36
#define BITFIELDS "-^"
#define PORT_INV 0x4000
#define PORT_PU 0x2000
#define port_mask(p) ((p)&0xFF) // 16 bit

// Dynamic

#define	settings		\
	io(gfxmosi,36)	\
	io(gfxsck,38)	\
	io(gfxcs,40)	\
	io(gfxdc,42)	\
	io(gfxrst,44)	\
	io(gfxbusy,46)	\
	io(gfxena,)	\
        u8(gfxflip,6)   \
	io(bellpush,10)	\
	u8(holdtime,30)	\
	s(imageurl)	\
	s(idlename)	\
	s(postcode)	\
	s(bellmqtt)	\
	s(bellmqttpl)	\
	s(toot)		\
	s(tasout)	\
	s(tasbusy)	\

#define u32(n,d)        uint32_t n;
#define s8(n,d) int8_t n;
#define u8(n,d) uint8_t n;
#define b(n) uint8_t n;
#define s(n) char * n;
#define io(n,d)           uint16_t n;
settings
#undef io
#undef u32
#undef s8
#undef u8
#undef b
#undef s
   httpd_handle_t webserver = NULL;
uint32_t pushed = 0;
uint32_t override = 0;
uint32_t last = -1;
char activename[20] = "Wait";
uint8_t *idle = NULL;
uint8_t *active = NULL;
SemaphoreHandle_t mutex = NULL;
char wifiinit = 0;
char mqttinit = 0;

static void
web_head (httpd_req_t * req, const char *title)
{
   revk_web_head (req, title);
   httpd_resp_sendstr_chunk (req, "<style>"     //
                             "body{font-family:sans-serif;background:#8cf;}"    //
                             "</style><body><h1>");
   if (title)
      httpd_resp_sendstr_chunk (req, title);
   httpd_resp_sendstr_chunk (req, "</h1>");
}

static esp_err_t
web_icon (httpd_req_t * req)
{                               // serve image -  maybe make more generic file serve
   extern const char start[] asm ("_binary_apple_touch_icon_png_start");
   extern const char end[] asm ("_binary_apple_touch_icon_png_end");
   httpd_resp_set_type (req, "image/png");
   httpd_resp_send (req, start, end - start);
   return ESP_OK;
}

static esp_err_t
web_root (httpd_req_t * req)
{
   if (revk_link_down ())
      return revk_web_settings (req);   // Direct to web set up
   web_head (req, *hostname ? hostname : appname);
   return revk_web_foot (req, 0, 1);
}

uint8_t *
getimage (char *name)
{
   if (!*imageurl || !name || !*name)
      return NULL;
   char *url;
   asprintf (&url, "%s/%s.mono", imageurl, name);
   if (!url)
      return NULL;
   ESP_LOGE (TAG, "Get %s", url);
   const int size = gfx_width () * gfx_height () / 8;
   uint8_t *buf = mallocspi (size);
   if (!buf)
   {
      free (url);
      return NULL;
   }
   esp_http_client_config_t config = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
   };
   esp_http_client_handle_t client = esp_http_client_init (&config);
   if (!client)
   {
      free (url);
      free (buf);
      return NULL;
   }
   int len = 0;
   if (!esp_http_client_open (client, 0))
   {
      if (esp_http_client_fetch_headers (client) >= 0)
         len = esp_http_client_read_response (client, (char *) buf, size);
      esp_http_client_close (client);
   }
   REVK_ERR_CHECK (esp_http_client_cleanup (client));
   free (url);
   if (len != size)
   {
      free (buf);
      return NULL;
   }
   ESP_LOGE (TAG, "Image loaded %s", name);
   return buf;
}

const char *
gfx_qr (const char *value, int s)
{
#ifndef	CONFIG_GFX_NONE
   unsigned int width = 0;
 uint8_t *qr = qr_encode (strlen (value), value, widthp: &width, noquiet:1);
   if (!qr)
      return "Failed to encode";
   int w = gfx_width ();
   int h = gfx_height ();
   if (!width || width > w || width > h)
   {
      free (qr);
      return "Too wide";
   }
   ESP_LOGD (TAG, "QR %d/%d %d", w, h, s);
   int ox = gfx_x ();
   int oy = gfx_y ();
   if (gfx_a () & GFX_B)
      oy -= s * width;
   if (gfx_a () & GFX_R)
      ox -= s * width;
   for (int y = 0; y < width; y++)
      for (int x = 0; x < width; x++)
         if (qr[width * y + x] & QR_TAG_BLACK)
            for (int dy = 0; dy < s; dy++)
               for (int dx = 0; dx < s; dx++)
                  gfx_pixel (ox + x * s + dx, oy + y * s + dy, 0xFF);
   free (qr);
#endif
   return NULL;
}

void
setactive (char *value)
{
   xSemaphoreTake (mutex, portMAX_DELAY);
   free (active);
   active = NULL;
   strncpy (activename, value, sizeof (activename));
   if (*activename)
      active = getimage (activename);
   if (!last)
      last = -1;                // Redisplay
   if (pushed)
      pushed = uptime ();
   xSemaphoreGive (mutex);
}

void
tassub (char *name)
{
   if (!*name)
      return;
   char *topic;
   asprintf (&topic, "stat/%s/RESULT", name);
   lwmqtt_subscribe (revk_mqtt (0), topic);
   free (topic);
   asprintf (&topic, "cmnd/%s/POWER", name);
   revk_mqtt_send_raw (topic, 0, NULL, 1);
   free (topic);
}

const char *
app_callback (int client, const char *prefix, const char *target, const char *suffix, jo_t j)
{
   char value[1000];
   int len = 0;
   *value = 0;
   if (j)
   {
      len = jo_strncpy (j, value, sizeof (value));
      if (len < 0)
         return "Expecting JSON string";
      if (len > sizeof (value))
         return "Too long";
   }
   if (client || !prefix || target || strcmp (prefix, prefixcommand) || !suffix)
      return NULL;              //Not for us or not a command from main MQTT
   if (!strcmp (suffix, "wifi"))
   {
      wifiinit = 1;
      return "";
   }
   if (!strcmp (suffix, "connect"))
   {
      mqttinit = 1;
      return "";
   }
   if (!strcmp (suffix, "message"))
   {
      xSemaphoreTake (mutex, portMAX_DELAY);
      override = uptime ();
      gfx_message (value);
      xSemaphoreGive (mutex);
      return "";
   }
   if (!strcmp (suffix, "cancel"))
   {
      override = 0;
      pushed = 0;
      return "";
   }
   if (!strcmp (suffix, "push"))
   {
      pushed = uptime ();
      return "";
   }
   if (!strcmp (suffix, "status"))
   {
      setactive (value);
      return "";
   }
   return NULL;
}

// --------------------------------------------------------------------------------
// Web
#ifdef	CONFIG_REVK_APCONFIG
#error 	Clash with CONFIG_REVK_APCONFIG set
#endif

void
push_task (void *arg)
{

   gpio_reset_pin (port_mask (bellpush));
   gpio_set_direction (port_mask (bellpush), GPIO_MODE_INPUT);
   while (1)
   {
      uint8_t l = gpio_get_level (port_mask (bellpush));
      if (!l)
         pushed = uptime ();
      usleep (10000);
   }
}

void
app_main ()
{
   mutex = xSemaphoreCreateBinary ();
   xSemaphoreGive (mutex);
   revk_boot (&app_callback);
   revk_register ("gfx", 0, sizeof (gfxcs), &gfxcs, "- ", SETTING_SET | SETTING_BITFIELD | SETTING_SECRET);     // Header
#define io(n,d)           revk_register(#n,0,sizeof(n),&n,"- "#d,SETTING_SET|SETTING_BITFIELD);
#define b(n) revk_register(#n,0,sizeof(n),&n,NULL,SETTING_BOOLEAN);
#define u32(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s8(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_SIGNED);
#define u8(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s(n) revk_register(#n,0,0,&n,NULL,0);
   settings
#undef io
#undef u32
#undef s8
#undef u8
#undef b
#undef s
      revk_start ();

   // Web interface
   httpd_config_t config = HTTPD_DEFAULT_CONFIG ();
   if (!httpd_start (&webserver, &config))
   {
      {
         httpd_uri_t uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = web_root,
            .user_ctx = NULL
         };
         REVK_ERR_CHECK (httpd_register_uri_handler (webserver, &uri));
      }
      {
         httpd_uri_t uri = {
            .uri = "/apple-touch-icon.png",
            .method = HTTP_GET,
            .handler = web_icon,
            .user_ctx = NULL
         };
         REVK_ERR_CHECK (httpd_register_uri_handler (webserver, &uri));
      }
      revk_web_settings_add (webserver);
   }

   {
    const char *e = gfx_init (cs: port_mask (gfxcs), sck: port_mask (gfxsck), mosi: port_mask (gfxmosi), dc: port_mask (gfxdc), rst: port_mask (gfxrst), busy: port_mask (gfxbusy), ena: port_mask (gfxena), flip: gfxflip, direct:1);
      if (e)
      {
         ESP_LOGE (TAG, "gfx %s", e);
         jo_t j = jo_object_alloc ();
         jo_string (j, "error", "Failed to start");
         jo_string (j, "description", e);
         revk_error ("gfx", &j);
      }
   }
   revk_task ("push", push_task, NULL, 4);
   sleep (1);
   gfx_lock ();
   gfx_clear (255);             // Black
   gfx_unlock ();
   while (1)
   {
      usleep (100000);
      time_t now = time (0);
      struct tm t;
      localtime_r (&now, &t);
      uint32_t up = uptime ();
      if (mqttinit)
      {
         ESP_LOGE (TAG, "MQTT Connected");
         mqttinit = 0;
         last = -1;
         tassub (tasout);
         tassub (tasbusy);
      }
      if (wifiinit)
      {                         // Get files
         ESP_LOGE (TAG, "Wifi Connected");
         wifiinit = 0;
         xSemaphoreTake (mutex, portMAX_DELAY);
         if (*idlename && !idle)
            idle = getimage (idlename);
         if (*activename && !active)
            active = getimage (activename);
         xSemaphoreGive (mutex);
      }
      if (override + holdtime < up)
         override = 0;
      if (override)
         continue;
      if (pushed + holdtime < up)
         pushed = 0;            // Time out
      void addqr (void)
      {
         if (*postcode)
         {
            char temp[200];
            sprintf (temp, "%4d-%02d-%02d %02d:%02d %s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, postcode);
            gfx_pos (0, gfx_height () - 1, GFX_B | GFX_L);
            gfx_qr (temp, 4);
         }
      }
      if (pushed)
      {                         // Bell was pushed
         if (last)
         {                      // Show status as was showing idle
            xSemaphoreTake (mutex, portMAX_DELAY);
            if (*activename && !active)
               active = getimage (activename);
            last = 0;
            if (*bellmqtt)
               revk_mqtt_send_raw (bellmqtt, 0, bellmqttpl, 1);
            gfx_lock ();
            gfx_clear (0);
            gfx_pos (0, 0, 0);
            if (!active)
               gfx_message ("PLEASE/WAIT");
            else
               gfx_icon2 (gfx_width (), gfx_height (), active);
            addqr ();
            gfx_unlock ();
            xSemaphoreGive (mutex);
         }
      } else if (last != now / 60)
      {                         // Show idle
         xSemaphoreTake (mutex, portMAX_DELAY);
         if (*idlename && !idle)
            idle = getimage (idlename);
         gfx_lock ();
         if (!last || !t.tm_min)
            gfx_refresh ();
         gfx_clear (0);
         gfx_pos (0, 0, 0);
         if (!idle)
            gfx_message ("RING/THE/BELL");
         else
            gfx_icon2 (gfx_width (), gfx_height (), idle);
         addqr ();
         gfx_unlock ();
         last = now / 60;
         xSemaphoreGive (mutex);
      }
   }
}
