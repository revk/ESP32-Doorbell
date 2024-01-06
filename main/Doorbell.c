/* Doorbell app */
/* Copyright ©2019 - 2023 Adrian Kennard, Andrews & Arnold Ltd.See LICENCE file for details .GPL 3.0 */

// TODO
// More general image cache with If-Modified-Since logic
// Maybe flash filing system for images even
// More options for LEDs, especially when we have 24 of them

static const char TAG[] = "Doorbell";

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

#define	MAXGPIO	36
#define BITFIELDS "-^"
#define PORT_INV 0x4000
#define PORT_PU 0x2000
#define port_mask(p) ((p)&0xFF) // 16 bit

const uint8_t blink[3] = { 0 }; // dummy

#define	UPDATERATE	60

// Dynamic

#define	settings		\
	io(gfxena,)	\
        io(btn2,-42)     \
        io(btn1,-41)     \
        io(gfxmosi,40)  \
        io(gfxsck,39)   \
        io(gfxcs,38)    \
        io(gfxdc,37)    \
        io(gfxrst,36)   \
        io(gfxbusy,35)  \
        io(rgb,34)      \
        io(relay,33)    \
	u8(leds,24)	\
        u8(gfxflip,6)   \
	u8(holdtime,30)	\
	u8(ledw1,0)	\
	u8(ledw2,0)	\
	u32(refresh,86400)	\
	b(gfxinvert)	\
	s(imageurl,)	\
	s(imageidle,Example)	\
	s(imagemoon,)	\
	s(imagexmas,)	\
	s(imageeast,)	\
	s(imageyear,)	\
	s(imagehall,)	\
	s(imagewait,G:Wait)	\
	s(imagebusy,Y:Busy)	\
	s(imageaway,R:Away)	\
	s(postcode,)	\
	s(toot,)		\
	s(tasbell,)	\
	s(tasaway,)	\
	s(tasbusy,)	\

#define u32(n,d)        uint32_t n;
#define s8(n,d) int8_t n;
#define u8(n,d) uint8_t n;
#define b(n) uint8_t n;
#define s(n,d) char * n;
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
char activename[30] = "";
char overridename[30] = "";
uint8_t *idle = NULL;
uint8_t *active = NULL;
SemaphoreHandle_t mutex = NULL;
char mqttinit = 0;
char tasawaystate = 0;
char tasbusystate = 0;
led_strip_handle_t strip = NULL;
volatile char led_colour = 0;
volatile char overridemsg[1000] = "";
volatile uint8_t wificonnect = 1;

const char *
getidle (time_t t)
{
#ifdef	CONFIG_REVK_LUNAR
   if (*imagemoon && (t < revk_last_moon (t) + 12 * 3600 || t > revk_next_moon (t) + 12 * 3600))
      return imagemoon;
#endif
   char season = revk_season (t);
   if (*imagexmas && season == 'X')
      return imagexmas;
   if (*imageyear && season == 'Y')
      return imageyear;
   if (*imagehall && season == 'H')
      return imagehall;
   if (*imageeast && season == 'E')
      return imageeast;
   return imageidle;
}

const char *
skipcolour (const char *n)
{
   if (n && *n && n[1] == ':')
      n += 2;
   return n;
}

uint8_t *
getimage (const char *name, uint8_t * prev)
{
   if (!*imageurl || !name || !*name || revk_link_down ())
      return prev;
   name = skipcolour (name);
   char *url;
   asprintf (&url, "%s/%s.mono", imageurl, name);
   if (!url)
      return prev;
   ESP_LOGD (TAG, "Get %s", url);
   const int size = gfx_width () * gfx_height () / 8;
   int len = 0;
   uint8_t *buf = NULL;
   esp_http_client_config_t config = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
   };
   esp_http_client_handle_t client = esp_http_client_init (&config);
   if (client)
   {
      if (!esp_http_client_open (client, 0))
      {
         if (esp_http_client_fetch_headers (client) == size)
         {
            buf = mallocspi (size);
            if (buf)
               len = esp_http_client_read_response (client, (char *) buf, size);
         }
         esp_http_client_close (client);
      }
      esp_http_client_cleanup (client);
   }
   if (len != size)
   {
      jo_t j = jo_object_alloc ();
      jo_string (j, "name", name);
      jo_string (j, "url", url);
      if (len)
      {
         jo_int (j, "len", len);
         jo_int (j, "expect", size);
      }
      revk_error ("image", &j);
      free (url);
      free (buf);
      return prev;
   }
   if (gfxinvert)
      for (int i = 0; i < size; i++)
         buf[i] ^= 0xFF;
   if (!prev || memcmp (prev, buf, len))
   {                            // New image
      jo_t j = jo_object_alloc ();
      jo_string (j, "name", name);
      jo_string (j, "url", url);
      jo_int (j, "len", len);
      revk_info ("image", &j);
   }
   free (url);
   free (prev);
   return buf;
}

void
image_load (const char *name, const uint8_t * image, char c)
{                               // Load image and set LEDs (image can be prefixed with colour, else default is used)
   if (name && *name && name[1] == ':')
      c = *name;
   led_colour = c;
   if (image)
      gfx_load (image);
}

void
setactive (char *value)
{
   if (!value || !strcmp (activename, value))
      return;
   xSemaphoreTake (mutex, portMAX_DELAY);
   strncpy (activename, value, sizeof (activename));
   free (active);
   active = NULL;
   if (!last)
      last = -1;                // Redisplay
   if (pushed)
      pushed = uptime () + holdtime;
   xSemaphoreGive (mutex);
}

static void
web_head (httpd_req_t * req, const char *title)
{
   revk_web_head (req, title);
   revk_web_send (req, "<style>"        //
                  "body{font-family:sans-serif;background:#8cf;}"       //
                  "</style><body><h1>%s</h1>", title ? : "");
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
   revk_web_send (req, "<p><a href=/push>Ding!</a></p>");
   if (*imageurl)
   {
      time_t now = time (0);
      const char *isidle = getidle (now);
      void i (const char *tag, const char *name)
      {
         if (!*name)
            return;
         const char *filename = skipcolour (name);
         uint32_t rgb = 0x808080;
         if (filename != name)
            rgb = revk_rgb (*name);
         revk_web_send (req,
                        "<figure style='display:inline-block;background:white;border:10px solid white;border-left:20px solid white;margin:5px;%s'><img wdth=240 height=400 src='%s/%s.png'><figcaption style='margin:3px;padding:3px;background:#%06lX%s'>%s%s</figcaption></figure>",
                        gfxinvert ? ";filter:invert(1)" : "", imageurl, filename, rgb, gfxinvert ? ";filter:invert(1)" : "", tag,
                        !strcmp (name, isidle) || !strcmp (name, activename) ? " (current)" : "");
      }
      revk_web_send (req, "<p>");
      i ("Idle", imageidle);
      i ("New Year", imageyear);
      i ("Full moon", imagemoon);
      i ("Easter", imageeast);
      i ("Halloween", imagehall);
      i ("Xmas", imagexmas);
      revk_web_send (req, "</p><p>");
      if (strcmp (activename, imagewait) && strcmp (activename, imagewait) && strcmp (activename, imageaway))
         i ("Active", activename);
      i ("Wait", imagewait);
      if (*tasbusy)
         i ("Busy", imagebusy);
      if (*tasaway)
         i ("Away", imageaway);
      revk_web_send (req, "</p>");
   }
   return revk_web_foot (req, 0, 1, NULL);
}

static esp_err_t
web_push (httpd_req_t * req)
{
   size_t l = httpd_req_get_url_query_len (req);
   char query[200];
   if (!*overridename && l > 0 && l < sizeof (query) && !httpd_req_get_url_query_str (req, query, sizeof (query)))
      strncpy (overridename, query, sizeof (overridename));
   else
      pushed = uptime () + holdtime;
   return web_root (req);
}

static esp_err_t
web_active (httpd_req_t * req)
{
   size_t l = httpd_req_get_url_query_len (req);
   char query[200];
   if (l > 0 && l < sizeof (query) && !httpd_req_get_url_query_str (req, query, sizeof (query)))
   {
      char *q = query;
      if (*q == '?')
         q++;
      setactive (q);
   }
   return web_root (req);
}

static esp_err_t
web_message (httpd_req_t * req)
{
   size_t l = httpd_req_get_url_query_len (req);
   char query[200];
   if (l > 0 && l < sizeof (query) && !httpd_req_get_url_query_str (req, query, sizeof (query)))
   {
      char *q = query;
      if (*q == '?')
         q++;
      strncpy ((char *) overridemsg, q, sizeof (overridemsg));
   }
   return web_root (req);
}

static void
register_uri (const httpd_uri_t * uri_struct)
{
   esp_err_t res = httpd_register_uri_handler (webserver, uri_struct);
   if (res != ESP_OK)
   {
      ESP_LOGE (TAG, "Failed to register %s, error code %d", uri_struct->uri, res);
   }
}

static void
register_get_uri (const char *uri, esp_err_t (*handler) (httpd_req_t * r))
{
   httpd_uri_t uri_struct = {
      .uri = uri,
      .method = HTTP_GET,
      .handler = handler,
   };
   register_uri (&uri_struct);
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
   gfx_pos_t ox,
     oy;
   gfx_draw (width * s, width * s, 0, 0, &ox, &oy);
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
   if (j && jo_here (j) == JO_STRING)
   {
      len = jo_strncpy (j, value, sizeof (value));
      if (len < 0)
         return "Expecting JSON string";
      if (len > sizeof (value))
         return "Too long";
   }
   if (prefix && target && suffix && j && !strcmp (prefix, "stat") && !strcmp (suffix, "RESULT"))
   {
      jo_rewind (j);
      if (jo_find (j, "POWER") == JO_STRING)
      {                         // "ON" or "OFF"
         if (!strcmp (target, tasaway))
            tasawaystate = !jo_strcmp (j, "OFF");       // Off means we are away
         else if (!strcmp (target, tasbusy))
            tasbusystate = !jo_strcmp (j, "OFF");       // Off means we are busy
         setactive (tasawaystate ? imageaway : tasbusystate ? imagebusy : imagewait);
      }
   }
   if (client || !prefix || target || strcmp (prefix, prefixcommand) || !suffix)
      return NULL;              //Not for us or not a command from main MQTT
   if (!strcmp (suffix, "connect"))
   {
      mqttinit = 1;
      return "";
   }
   if (!strcmp (suffix, "upgrade"))
   {
      strncpy ((char *) overridemsg, "UPGRADING", sizeof (overridemsg));
      return "";
   }
   if (!strcmp (suffix, "wifi") || !strcmp (suffix, "ipv6"))
   {
      wificonnect = 1;
      return "";
   }
   if (!strcmp (suffix, "message"))
   {
      strncpy ((char *) overridemsg, value, sizeof (overridemsg));
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
      if (!*overridename && *value)
         strncpy (overridename, value, sizeof (overridename));
      else
         pushed = uptime () + holdtime;
      return "";
   }
   if (!strcmp (suffix, "active"))
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

   gpio_reset_pin (port_mask (btn1));
   gpio_set_direction (port_mask (btn1), GPIO_MODE_INPUT);
   while (1)
   {
      uint8_t l = gpio_get_level (port_mask (btn1));
      if (!l)
         pushed = uptime () + holdtime;
      usleep (10000);
   }
}

void
led_task (void *arg)
{
   uint8_t or = 0,
      og = 0,
      ob = 0;
   while (1)
   {
      char c = led_colour;
      uint32_t rgb = revk_rgb (c);
      uint8_t r = (rgb >> 16),
         g = (rgb >> 8),
         b = rgb;
      if (r == or && g == og && b == ob)
      {                         // No change
         usleep (10000);
         continue;
      }
      // Fade
      for (int t = 0; t <= 0xFF; t += 0x0F)
      {
         uint8_t RI,
           R = gamma8[(t * r + (0xFF - t) * or) / 0xFF];
         uint8_t GI,
           G = gamma8[(t * g + (0xFF - t) * og) / 0xFF];
         uint8_t BI,
           B = gamma8[(t * b + (0xFF - t) * ob) / 0xFF];
         if (ledw2 > ledw1 && !r && !g && !b)
         {                      // Idle LEDs
            RI = gamma8[(t * 0x55 + (0xFF - t) * or) / 0xFF];
            GI = gamma8[(t * 0x55 + (0xFF - t) * og) / 0xFF];
            BI = gamma8[(t * 0x55 + (0xFF - t) * ob) / 0xFF];
         } else
         {                      // Other LEDs
            RI = R;
            GI = G;
            BI = B;
         }
         for (int i = 0; i < leds; i++)
            if (i >= ledw1 && i < ledw2)
               led_strip_set_pixel (strip, i, RI, GI, BI);
            else
               led_strip_set_pixel (strip, i, R, G, B);
         led_strip_refresh (strip);
         usleep (50000);
      }
      or = r;
      og = g;
      ob = b;
   }
}

void
app_main ()
{
   mutex = xSemaphoreCreateBinary ();
   xSemaphoreGive (mutex);
   revk_boot (&app_callback);
   revk_register ("gfx", 0, sizeof (gfxcs), &gfxcs, "- ", SETTING_SET | SETTING_BITFIELD | SETTING_SECRET);     // Header
   revk_register ("image", 0, 0, &imageurl, "http://ota.revk.uk/Doorbell", SETTING_SECRET);     // Header
   revk_register ("tas", 0, 0, &tasbell, NULL, SETTING_SECRET); // Header
#define io(n,d)           revk_register(#n,0,sizeof(n),&n,"- "#d,SETTING_SET|SETTING_BITFIELD);
#define b(n) revk_register(#n,0,sizeof(n),&n,NULL,SETTING_BOOLEAN);
#define u32(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s8(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_SIGNED);
#define u8(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s(n,d) revk_register(#n,0,0,&n,#d,0);
   settings
#undef io
#undef u32
#undef s8
#undef u8
#undef b
#undef s
      revk_start ();
   setactive (imagewait);

   if (leds)
   {
      led_strip_config_t strip_config = {
         .strip_gpio_num = (port_mask (rgb)),
         .max_leds = leds,
         .led_pixel_format = LED_PIXEL_FORMAT_GRB,      // Pixel format of your LED strip
         .led_model = LED_MODEL_WS2812, // LED strip model
         .flags.invert_out = ((rgb & PORT_INV) ? 1 : 0),        // whether to invert the output signal (useful when your hardware has a level inverter)
      };
      led_strip_rmt_config_t rmt_config = {
         .clk_src = RMT_CLK_SRC_DEFAULT,        // different clock source can lead to different power consumption
         .resolution_hz = 10 * 1000 * 1000,     // 10MHz
         .flags.with_dma = true,
      };
      REVK_ERR_CHECK (led_strip_new_rmt_device (&strip_config, &rmt_config, &strip));
      if (strip)
         revk_task ("led", led_task, NULL, 4);
      image_load (NULL, NULL, 'M');
   }

   // Web interface
   httpd_config_t config = HTTPD_DEFAULT_CONFIG ();
   config.max_uri_handlers = 5 + revk_num_web_handlers ();
   if (!httpd_start (&webserver, &config))
   {
      register_get_uri ("/", web_root);
      register_get_uri ("/apple-touch-icon.png", web_icon);
      register_get_uri ("/push", web_push);
      register_get_uri ("/message", web_message);
      register_get_uri ("/active", web_active);
      revk_web_settings_add (webserver);
   }
   {
    const char *e = gfx_init (cs: port_mask (gfxcs), sck: port_mask (gfxsck), mosi: port_mask (gfxmosi), dc: port_mask (gfxdc), rst: port_mask (gfxrst), busy: port_mask (gfxbusy), ena: port_mask (gfxena), flip: gfxflip, direct: 1, invert:gfxinvert);
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
   gfx_lock ();
   gfx_clear (255);             // Black
   gfx_unlock ();
   uint32_t lastrefresh = 0;
   uint8_t day = 0;
   while (1)
   {
      usleep (100000);
      time_t now = time (0);
      struct tm t;
      localtime_r (&now, &t);
      uint32_t up = uptime ();
      void addqr (void)
      {
         if (*postcode)
         {
            char temp[200];
            sprintf (temp, "%4d-%02d-%02d %02d:%02d %s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, postcode);
            gfx_pos (0, gfx_height () - 1, GFX_B | GFX_L | GFX_V);
            gfx_qr (temp, 4);
         }
      }
      const char *basename = getidle (now);
      if (!revk_link_down () && day != t.tm_mday)
      {                         // Get files
         day = t.tm_mday;
         xSemaphoreTake (mutex, portMAX_DELAY);
         idle = getimage (basename, idle);
         active = getimage (activename, active);
         xSemaphoreGive (mutex);
      }
      if (mqttinit)
      {
         ESP_LOGE (TAG, "MQTT Connected");
         mqttinit = 0;
         last = -1;
         tassub (tasaway);
         tassub (tasbusy);
      }
      if (wificonnect)
      {
         wificonnect = 0;
         wifi_ap_record_t ap = {
         };
         esp_wifi_sta_get_ap_info (&ap);
         char *p = (char *) overridemsg;
         char temp[20];
         p += sprintf (p, "[3] /[-6]%s/%s/[3]%s %s/[3] / /", appname, hostname, revk_version, revk_build_date (temp) ? : "?");
         if (sta_netif && *ap.ssid)
         {
            p += sprintf (p, "[6]WiFi/[-5]%s/[3] /[6]Channel %d/RSSI %d/[3] /", (char *) ap.ssid, ap.primary, ap.rssi);
            {
               esp_netif_ip_info_t ip;
               if (!esp_netif_get_ip_info (sta_netif, &ip) && ip.ip.addr)
                  p += sprintf (p, "[6]IPv4/[5]" IPSTR "/[3] /", IP2STR (&ip.ip));
            }
#ifdef CONFIG_LWIP_IPV6
            {
               esp_ip6_addr_t ip[LWIP_IPV6_NUM_ADDRESSES];
               int n = esp_netif_get_all_ip6 (sta_netif, ip);
               if (n)
               {
                  p += sprintf (p, "[6]IPv6/[2]");
                  char *q = p;
                  for (int i = 0; i < n; i++)
                     p += sprintf (p, IPV6STR "/", IPV62STR (ip[i]));
                  while (*q)
                  {
                     *q = toupper (*q);
                     q++;
                  }
                  p += sprintf (p, "/[3] /");
               }
            }
#endif
         }
         override = up + 10;
      }
      if (*overridemsg)
      {
         ESP_LOGE (TAG, "Override: %s", overridemsg);
         xSemaphoreTake (mutex, portMAX_DELAY);
         if (override < up)
            override = up + holdtime;
         last = 0;
         gfx_lock ();
         gfx_message ((char *) overridemsg);
         *overridemsg = 0;
         addqr ();
         gfx_unlock ();
         xSemaphoreGive (mutex);
      }
      if (*overridename)
      {                         // Special override
         ESP_LOGE (TAG, "Override: %s", overridename);
         uint8_t *image = getimage (overridename, NULL);
         if (image)
         {
            xSemaphoreTake (mutex, portMAX_DELAY);
            if (override < up)
               override = up + holdtime;
            last = 0;
            gfx_lock ();
            image_load (overridename, image, 'B');
            addqr ();
            gfx_unlock ();
            xSemaphoreGive (mutex);
            free (image);
         }
         *overridename = 0;
      }
      if (override)
      {
         if (override < up)
            override = 0;
         else
            continue;
      }
      if (pushed < up)
         pushed = 0;            // Time out
      if (pushed)
      {                         // Bell was pushed
         if (last)
         {                      // Show status as was showing idle
            xSemaphoreTake (mutex, portMAX_DELAY);
            if (!active)
               active = getimage (activename, active);
            last = 0;
            if (*tasbell)
            {
               char *topic = NULL;
               asprintf (&topic, "cmnd/%s/POWER", tasbell);
               revk_mqtt_send_raw (topic, 0, "ON", 1);
               free (topic);
            }
            if (*toot)
            {
               char *pl = NULL;
               asprintf (&pl, "@%s\nDing dong\n%s\n%4d-%02d-%02d %02d:%02d:%02d", toot, activename, t.tm_year + 1900,
                         t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
               revk_mqtt_send_raw ("toot", 0, pl, 1);
               free (pl);
            }
            gfx_lock ();
            // These do a gfx_clear or replace whole buffer anyway
            if (!active)
               gfx_message ("/ / / / / / /PLEASE/WAIT");
            else
               image_load (activename, active, 'B');
            addqr ();
            gfx_unlock ();
            xSemaphoreGive (mutex);
         }
      } else if (last != now / UPDATERATE)
      {                         // Show idle
         xSemaphoreTake (mutex, portMAX_DELAY);
         if (!idle)
            idle = getimage (basename, idle);
         gfx_lock ();
         if (!last || (refresh && lastrefresh != now / refresh))
         {
            lastrefresh = now / refresh;
            gfx_refresh ();
         }
         last = now / UPDATERATE;
         // These do a gfx_clear or replace whole buffer anyway
         if (!idle)
            gfx_message ("/ / / / / /CANWCH/Y GLOCH/ / /RING/THE/BELL");
         else
            image_load (basename, idle, 'K');
         addqr ();
         gfx_pos (gfx_width () - 2, gfx_height () - 2, GFX_R | GFX_B);  // Yes slightly in from edge
#if	UPDATERATE >= 60
         gfx_7seg (2, "%02d:%02d", t.tm_hour, t.tm_min);
#else
         gfx_7seg (2, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
#endif
         gfx_unlock ();
         if (!active)
            active = getimage (activename, active);     // Just in case
         xSemaphoreGive (mutex);
      }
   }
}
