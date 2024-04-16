/* Doorbell app */
/* Copyright ©2019 - 2023 Adrian Kennard, Andrews & Arnold Ltd.See LICENCE file for details .GPL 3.0 */

static const char TAG[] = "Doorbell";

#include "revk.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "esp_vfs_fat.h"
#include "gfx.h"
#include "iec18004.h"
#include <hal/spi_types.h>
#include <driver/gpio.h>

#define	UPDATERATE	60

const char sd_mount[] = "/sd";

httpd_handle_t webserver = NULL;
sdmmc_card_t *card = NULL;
uint32_t pushed = 0;
uint32_t override = 0;
uint32_t last = -1;
char activename[30] = "";
char overridename[30] = "";
led_strip_handle_t strip = NULL;
volatile char led_colour = 0;
volatile char overridemsg[1000] = "";

struct
{
   uint8_t mqttinit:1;
   uint8_t wificonnect:1;
   uint8_t tasawaystate:1;
   uint8_t tasbusystate:1;
   uint8_t getimages:1;
} volatile b;

typedef struct image_s image_t;
struct image_s
{
   image_t *next;               // Next in chain
   time_t loaded;               // When loaded
   char *url;                   // Malloced Image URL
   uint8_t *data;               // Malloced image
};

image_t *cache = NULL;
image_t *idle = NULL;
image_t *active = NULL;

const char *
getidle (time_t t)
{
   const char *season = revk_season (t);
   if (*imagemoon && *season == 'M')
      return imagemoon;
   if (*imagenew && *season == 'N')
      return imagenew;
   if (*imageval && *season == 'V')
      return imageval;
   if (*imagexmas && *season == 'X')
      return imagexmas;
   if (*imageyear && *season == 'Y')
      return imageyear;
   if (*imagehall && *season == 'H')
      return imagehall;
   if (*imageeast && *season == 'E')
      return imageeast;
   return imageidle;
}

const char *
skipcolour (const char *n)
{
   if (!n || !*n)
      return n;
   if (n && *n == '*')
      n++;
   if (n && *n && n[1] == ':')
      n += 2;
   return n;
}

image_t *
getimage (const char *name)
{
   name = skipcolour (name);
   if (!name || !*name)
      return NULL;
   char *url = NULL;
   asprintf (&url, "%s/%s.mono", imageurl, name);
   if (!url)
      return NULL;
   image_t *i = NULL;
   for (i = cache; i && strcmp (i->url, url); i = i->next);
   const int size = gfx_width () * gfx_height () / 8;
   int len = 0;
   uint8_t *buf = NULL;
   esp_http_client_config_t config = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
   };
   int response = -1;
   void readcard (void)
   {
      if (card)
      {
         char *fn = NULL;
         asprintf (&fn, "%s/%s.mono", sd_mount, name);
         if (fn)
         {
            uint32_t start = uptime ();
            FILE *f = fopen (fn, "r");
            if (f)
            {
               if (!buf)
                  buf = mallocspi (size);
               if (buf)
               {
                  if (fread (buf, size, 1, f) == 1)
                  {
                     if (!i && (i = mallocspi (sizeof (*i))))
                     {
                        memset (i, 0, sizeof (*i));
                        i->url = url;
                        url = NULL;
                        i->next = cache;
                        cache = i;
                     }
                     if (i)
                     {
                        if (i->data && !memcmp (buf, i->data, size))
                           response = 0;        // No change
                        else
                        {
                           jo_t j = jo_object_alloc ();
                           jo_string (j, "read", fn);
                           jo_int (j, "time", uptime () - start);
                           revk_info ("SD", &j);
                           response = 200;      // Treat as received
                           free (i->data);
                           i->data = buf;
                           buf = NULL;
                        }
                     }
                  }
               }
               fclose (f);
            }
            free (fn);
         }
      }
   }
   if (!i)
      readcard ();
   if (*imageurl && !revk_link_down ())
   {
      esp_http_client_handle_t client = esp_http_client_init (&config);
      if (client)
      {
         if (i && i->loaded)
         {
            char when[50];
            struct tm t;
            gmtime_r (&i->loaded, &t);
            strftime (when, sizeof (when), "%a, %d %b %Y %T GMT", &t);
            esp_http_client_set_header (client, "If-Modified-Since", when);
         }
         if (!esp_http_client_open (client, 0))
         {
            if (esp_http_client_fetch_headers (client) == size)
            {
               buf = mallocspi (size);
               if (buf)
                  len = esp_http_client_read_response (client, (char *) buf, size);
            }
            if (!buf)
               esp_http_client_flush_response (client, &len);
            response = esp_http_client_get_status_code (client);
            esp_http_client_close (client);
         }
         esp_http_client_cleanup (client);
      }
   }
   if (response == 200 && len == size)
   {                            // Got new image
      jo_t j = jo_object_alloc ();
      jo_string (j, "name", name);
      if (url || i)
         jo_string (j, "url", url ? : i->url);
      jo_int (j, "len", len);
      revk_info ("image", &j);
      if (gfxinvert)
         for (int i = 0; i < size; i++)
            buf[i] ^= 0xFF;
      if (!i && (i = mallocspi (sizeof (*i))))
      {
         memset (i, 0, sizeof (*i));
         i->url = url;
         url = NULL;
         i->next = cache;
         cache = i;
      }
      if (i)
      {
         if (card && (!i->data || memcmp (i->data, buf, size)))
         {                      // Save, as changed or new
            char *fn = NULL;
            asprintf (&fn, "%s/%s.mono", sd_mount, name);
            if (fn)
            {
               uint32_t start = uptime ();
               FILE *f = fopen (fn, "w");
               if (f)
               {
                  jo_t j = jo_object_alloc ();
                  if (fwrite (buf, size, 1, f) != 1)
                     jo_string (j, "error", "write failed");
                  jo_int (j, "time", uptime () - start);
                  fclose (f);
                  jo_string (j, "write", fn);
                  revk_info ("SD", &j);
               }
               free (fn);
            }
         }
         free (i->data);
         i->data = buf;
         buf = NULL;
         i->loaded = time (0);
      }
   } else if (response != 304)
   {
      if (*imageurl)
      {
         jo_t j = jo_object_alloc ();
         jo_string (j, "name", name);
         jo_string (j, "url", url);
         if (len)
         {
            jo_int (j, "len", len);
            jo_int (j, "expect", size);
         }
         if (response)
            jo_int (j, "response", response);
         revk_error ("image", &j);
      }
      readcard ();
   }
   free (buf);
   free (url);
   return i;
}

void
image_load (const char *name, image_t * i, char c)
{                               // Load image and set LEDs (image can be prefixed with colour, else default is used)
   if (*name == '*')
   {                            // Full refresh
      gfx_refresh ();
      name++;
   }
   if (name && *name && name[1] == ':')
   {
      c = *name;
      name += 2;
   }
   led_colour = c;
   if (i && i->data)
      gfx_load (i->data);
}

void
setactive (char *value)
{
   if (!value || !strcmp (activename, value))
      return;
   strncpy (activename, value, sizeof (activename));
   active = NULL;
   if (!last)
      last = -1;                // Redisplay
   if (pushed)
      pushed = uptime () + holdtime;
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
web_text (httpd_req_t * req, const char *msg)
{
   httpd_resp_set_type (req, "text/plain;charset=utf-8");
   if (msg)
      revk_web_send (req, "%s", msg);
   httpd_resp_sendstr_chunk (req, NULL);
   return ESP_OK;
}

static esp_err_t
web_root (httpd_req_t * req)
{
   if (revk_link_down ())
      return revk_web_settings (req);   // Direct to web set up
   web_head (req, *hostname ? hostname : appname);
   revk_web_send (req, "<p><a href=/push>Ding!</a></p>");
   if (card)
      revk_web_send (req, "<p>SD card mounted</p>");
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
      i ("Full moon", imagemoon);
      i ("New moon", imagenew);
      i ("New Year", imageyear);
      i ("Easter", imageeast);
      i ("Halloween", imagehall);
      i ("Valentine", imageval);
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
   {
      strncpy (overridename, query, sizeof (overridename));
      return web_text (req, NULL);
   }
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
   return web_text (req, NULL);
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
   return web_text (req, NULL);
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
   if (!value || !*value)
      return "No value";
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
            b.tasawaystate = !jo_strcmp (j, "OFF");     // Off means we are away
         else if (!strcmp (target, tasbusy))
            b.tasbusystate = !jo_strcmp (j, "OFF");     // Off means we are busy
         setactive (b.tasawaystate ? imageaway : b.tasbusystate ? imagebusy : imagewait);
      }
   }
   if (client || !prefix || target || strcmp (prefix, prefixcommand) || !suffix)
      return NULL;              //Not for us or not a command from main MQTT
   if (!strcmp (suffix, "connect"))
   {
      b.mqttinit = 1;
      return "";
   }
   if (!strcmp (suffix, "upgrade"))
   {
      strncpy ((char *) overridemsg, "UPGRADING", sizeof (overridemsg));
      return "";
   }
   if (!strcmp (suffix, "wifi") || !strcmp (suffix, "ipv6"))
   {
      b.wificonnect = 1;
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
   if (!btn1.set)
   {
      vTaskDelete (NULL);
      return;
   }
   revk_gpio_input (btn1);
   while (1)
   {
      uint8_t l = revk_gpio_get (btn1);
      if (l)
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
   revk_boot (&app_callback);
   revk_start ();

   revk_gpio_output (relay, 0);

   revk_task ("push", push_task, NULL, 4);

   setactive (imagewait);

   if (leds)
   {
      led_strip_config_t strip_config = {
         .strip_gpio_num = (rgb.num),
         .max_leds = leds,
         .led_pixel_format = LED_PIXEL_FORMAT_GRB,      // Pixel format of your LED strip
         .led_model = LED_MODEL_WS2812, // LED strip model
         .flags.invert_out = rgb.invert,
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
   config.lru_purge_enable = true;
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
    const char *e = gfx_init (cs: gfxcs.num, sck: gfxsck.num, mosi: gfxmosi.num, dc: gfxdc.num, rst: gfxrst.num, busy: gfxbusy.num, ena: gfxena.num, flip: gfxflip, direct: 1, invert:gfxinvert);
      if (e)
      {
         ESP_LOGE (TAG, "gfx %s", e);
         jo_t j = jo_object_alloc ();
         jo_string (j, "error", "Failed to start");
         jo_string (j, "description", e);
         revk_error ("gfx", &j);
      }
   }
   if (sdmosi.set)
   {
      revk_gpio_input (sdcd);
      sdmmc_host_t host = SDSPI_HOST_DEFAULT ();
      host.max_freq_khz = SDMMC_FREQ_PROBING;
      spi_bus_config_t bus_cfg = {
         .mosi_io_num = sdmosi.num,
         .miso_io_num = sdmiso.num,
         .sclk_io_num = sdsck.num,
         .quadwp_io_num = -1,
         .quadhd_io_num = -1,
         .max_transfer_sz = 4000,
      };
      esp_err_t ret = spi_bus_initialize (host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
      if (ret != ESP_OK)
      {
         jo_t j = jo_object_alloc ();
         jo_string (j, "error", "SPI failed");
         jo_int (j, "code", ret);
         jo_int (j, "MOSI", sdmosi.num);
         jo_int (j, "MISO", sdmiso.num);
         jo_int (j, "CLK", sdsck.num);
         revk_error ("SD", &j);
      } else
      {
         esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = 1,
            .max_files = 2,
            .allocation_unit_size = 16 * 1024,
            .disk_status_check_enable = 1,
         };
         sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT ();
         //slot_config.gpio_cs = sdss.num;
         slot_config.gpio_cs = -1;
         revk_gpio_output (sdss, 0);    // Bodge for faster access when one SD card and ESP IDF V5+
         slot_config.gpio_cd = sdcd.num;
         slot_config.host_id = host.slot;
         ret = esp_vfs_fat_sdspi_mount (sd_mount, &host, &slot_config, &mount_config, &card);
         if (ret)
         {
            ESP_LOGE (TAG, "SD %d", ret);
            jo_t j = jo_object_alloc ();
            jo_string (j, "error", "Failed to mount");
            jo_int (j, "code", ret);
            revk_error ("SD", &j);
            card = NULL;
         }
         // TODO SD LED
      }
   }

   gfx_lock ();
   gfx_clear (255);             // Black
   gfx_unlock ();

   uint32_t lastrefresh = 0;
   uint8_t hour = -1;
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
      if (b.mqttinit)
      {
         ESP_LOGE (TAG, "MQTT Connected");
         b.mqttinit = 0;
         last = -1;
         tassub (tasaway);
         tassub (tasbusy);
      }
      if (b.wificonnect)
      {
         b.wificonnect = 0;
         if (startup)
         {
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
            override = up + startup;
         } else
            sleep (5);
         b.getimages = 1;
      }
      if (*overridemsg)
      {
         ESP_LOGE (TAG, "Override: %s", overridemsg);
         if (override < up)
            override = up + holdtime;
         last = 0;
         for (int n = 0; n < 3; n++)
         {
            gfx_lock ();
            gfx_message ((char *) overridemsg);
            *overridemsg = 0;
            addqr ();
            gfx_unlock ();
         }
      }
      if (*overridename)
      {                         // Special override
         ESP_LOGE (TAG, "Override: %s", overridename);
         char *t = strdup (overridename);
         *overridename = 0;
         image_t *i = getimage (t);
         if (i)
         {
            if (override < up)
               override = up + holdtime;
            last = 0;
            for (int n = 0; n < 3; n++)
            {
               gfx_lock ();
               image_load (t, i, 'B');
               addqr ();
               gfx_unlock ();
            }
         }
         free (t);
      }
      if (override && override < up)
         override = 0;
      if (!revk_link_down () && hour != t.tm_hour)
      {                         // Check new files
         hour = t.tm_hour;
         idle = getimage (basename);
         active = getimage (activename);
      }
      if (b.getimages)
      {
         b.getimages = 0;
         getimage (imageidle);  // Cache stuff
         getimage (imagewait);
         if (*tasbusy)
            getimage (imagebusy);
         if (*tasaway)
            getimage (imageaway);
         getimage (imagexmas);
         getimage (imagemoon);
         getimage (imagenew);
         getimage (imageval);
         getimage (imagehall);
         getimage (imageeast);
      }
      if (override)
         continue;
      if (pushed < up)
         pushed = 0;            // Time out
      if (pushed)
      {                         // Bell was pushed
         static uint32_t tick = 0;
         if (last || up / 5 != tick)
         {                      // Show, and reinforce image
            tick = up / 5;
            if (last)
            {
               revk_gpio_set (relay, 1);
               if (*tasbell)
               {
                  char *topic = NULL;
                  asprintf (&topic, "cmnd/%s/POWER", tasbell);
                  revk_mqtt_send_raw (topic, 0, "ON", 1);
                  free (topic);
               }
               const char *msg = b.tasawaystate ? mqttaway : b.tasbusystate ? mqttbusy : mqttbell;
               if (*msg)
                  revk_mqtt_send_str (msg);
            }
            if (last && *toot)
            {
               char *pl = NULL;
               asprintf (&pl, "@%s\nDing dong\n%s\n%4d-%02d-%02d %02d:%02d:%02d", toot, activename, t.tm_year + 1900,
                         t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
               revk_mqtt_send_raw ("toot", 0, pl, 1);
               free (pl);
            }
            if (!active)
               active = getimage (activename);
            gfx_lock ();
            // These do a gfx_clear or replace whole buffer anyway
            if (!active)
               gfx_message ("/ / / / / / /PLEASE/WAIT");
            else
               image_load (activename, active, 'B');
            addqr ();
            gfx_unlock ();
            if (last)
               revk_gpio_set (relay, 0);
            last = 0;
         }
      } else if (last != now / UPDATERATE)
      {                         // Show idle
         if (!idle)
            idle = getimage (basename);
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
      }
   }
}

void
revk_web_extra (httpd_req_t * req)
{
   revk_web_setting (req, "Base URL", "imageurl");
   revk_web_setting (req, "Idle", "imageidle");
   revk_web_setting (req, "Wait", "imagewait");
   if (*tasbusy)
      revk_web_setting (req, "Busy", "imagebusy");
   if (*tasaway)
      revk_web_setting (req, "Away", "imageaway");
   revk_web_setting (req, "Full moon", "imagemoon");
   revk_web_setting (req, "New moon", "imagenew");
   revk_web_setting (req, "New year", "imageyear");
   revk_web_setting (req, "Valentine", "imageval");
   revk_web_setting (req, "Easter", "imageeast");
   revk_web_setting (req, "Halloween", "imagehall");
   revk_web_setting (req, "Xmas", "imagexmas");
   if (*mqtthost)
   {
      revk_web_setting (req, "MQTT Bell", "mqttbell");
      if (*tasbusy)
         revk_web_setting (req, "MQTT Busy", "mqttbusy");
      if (*tasaway)
         revk_web_setting (req, "MQTT Away", "mqttaway");
   }
   revk_web_setting (req, "Image invert", "gfxinvert");
}
