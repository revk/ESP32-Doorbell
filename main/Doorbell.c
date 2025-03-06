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
#include <driver/sdmmc_host.h>
#include <driver/uart.h>
#include "gfx.h"
#include "iec18004.h"
#include <hal/spi_types.h>
#include <driver/gpio.h>
#include <lwpng.h>

#define	UPDATERATE	60

#define	NFCUART	1
#define NFCBUF  280

const char sd_mount[] = "/sd";

httpd_handle_t webserver = NULL;
sdmmc_card_t *card = NULL;
uint32_t pushed = 0;
uint32_t override = 0;
uint32_t last = -1;
char activename[30] = "";
char overridename[30] = "";
led_strip_handle_t strip = NULL;
volatile char led_colour[20] = { 0 };

volatile char overridemsg[1000] = "";

static SemaphoreHandle_t epd_mutex = NULL;

struct
{
   uint8_t mqttinit:1;
   uint8_t wificonnect:1;
   uint8_t tasawaystate:1;
   uint8_t tasbusystate:1;
   uint8_t getimages:1;
   uint8_t btn:1;
} volatile b;

typedef struct file_s
{
   struct file_s *next;         // Next file in chain
   char *url;                   // URL as passed to download
   uint32_t cache;              // Cache until this uptime
   time_t changed;              // Last changed
   uint32_t size;               // File size
   uint32_t w;                  // PNG width
   uint32_t h;                  // PNG height
   uint8_t *data;               // File data
   uint8_t new:1;               // New file
   uint8_t card:1;              // We have tried card
   uint8_t json:1;              // Is JSON
} file_t;

uint8_t nfcled = 0;
uint8_t nfcledoverride = 0;

file_t *cache = NULL;
file_t *idle = NULL;
file_t *active = NULL;

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
   if (*n == '*')
      n++;                      // Full refresh
   const char *c = n;
   while (*c && isalpha ((int) (unsigned char) *c))
      c++;                      // Colours
   if (*c == ':')
      n = c + 1;                // Yep, colours (end in :)
   return n;
}

file_t *files = NULL;

file_t *
find_file (char *url)
{
   file_t *i;
   for (i = files; i && strcmp (i->url, url); i = i->next);
   if (!i)
   {
      i = mallocspi (sizeof (*i));
      if (i)
      {
         memset (i, 0, sizeof (*i));
         i->url = strdup (url);
         i->next = files;
         files = i;
      }
   }
   return i;
}

void
check_file (file_t * i)
{
   if (!i || !i->data || !i->size)
      return;
   i->changed = time (0);
   const char *e1 = lwpng_get_info (i->size, i->data, &i->w, &i->h);
   if (!e1)
   {
      i->json = 0;              // PNG
      i->new = 1;
      ESP_LOGE (TAG, "Image %s len %lu width %lu height %lu", i->url, i->size, i->w, i->h);
   } else
   {                            // Not a png
      jo_t j = jo_parse_mem (i->data, i->size);
      jo_skip (j);
      const char *e2 = jo_error (j, NULL);
      jo_free (&j);
      if (!e2)
      {                         // Valid JSON
         i->json = 1;
         i->new = 1;
         i->w = i->h = 0;
         ESP_LOGE (TAG, "JSON %s len %lu", i->url, i->size);
      } else
      {                         // Not sensible
         free (i->data);
         i->data = NULL;
         i->size = 0;
         i->w = i->h = 0;
         i->changed = 0;
         ESP_LOGE (TAG, "Unknown %s error %s %s", i->url, e1 ? : "", e2 ? : "");
      }
   }
}

file_t *
download (char *url)
{
   file_t *i = find_file (url);
   if (!i)
      return i;
   url = strdup (i->url);       // Use as is
   ESP_LOGD (TAG, "Get %s", url);
   int32_t len = 0;
   uint8_t *buf = NULL;
   esp_http_client_config_t config = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .timeout_ms = 20000,
   };
   int response = -1;
   if (i->cache > uptime ())
      response = (i->data ? 304 : 404); // Cached
   else if (!revk_link_down () && (!strncasecmp (url, "http://", 7) || !strncasecmp (url, "https://", 8)))
   {
      i->cache = uptime () + imagecache;
      esp_http_client_handle_t client = esp_http_client_init (&config);
      if (client)
      {
         if (i->changed)
         {
            char when[50];
            struct tm t;
            gmtime_r (&i->changed, &t);
            strftime (when, sizeof (when), "%a, %d %b %Y %T GMT", &t);
            esp_http_client_set_header (client, "If-Modified-Since", when);
         }
         if (!esp_http_client_open (client, 0))
         {
            len = esp_http_client_fetch_headers (client);
            ESP_LOGD (TAG, "%s Len %ld", url, len);
            if (!len)
            {                   // Dynamic, FFS
               size_t l;
               FILE *o = open_memstream ((char **) &buf, &l);
               if (o)
               {
                  char temp[64];
                  while ((len = esp_http_client_read (client, temp, sizeof (temp))) > 0)
                     fwrite (temp, len, 1, o);
                  fclose (o);
                  len = l;
               }
               if (!buf)
                  len = 0;
            } else
            {
               buf = mallocspi (len);
               if (buf)
                  len = esp_http_client_read_response (client, (char *) buf, len);
            }
            response = esp_http_client_get_status_code (client);
            if (response != 200 && response != 304)
               ESP_LOGE (TAG, "Bad response %s (%d)", url, response);
            esp_http_client_close (client);
         }
         esp_http_client_cleanup (client);
      }
      ESP_LOGD (TAG, "Got %s %d", url, response);
   }
   if (response != 304)
   {
      if (response != 200)
      {                         // Failed
         jo_t j = jo_object_alloc ();
         jo_string (j, "url", url);
         if (response && response != -1)
            jo_int (j, "response", response);
         if (len == -ESP_ERR_HTTP_EAGAIN)
            jo_string (j, "error", "timeout");
         else if (len)
            jo_int (j, "len", len);
         revk_error ("image", &j);
      }
      if (buf)
      {
         if (i->data && i->size == len && !memcmp (buf, i->data, len))
         {
            free (buf);
            response = 0;       // No change
         } else
         {                      // Change
            free (i->data);
            i->data = buf;
            i->size = len;
            check_file (i);
         }
         buf = NULL;
      }
   }
   if (card)
   {                            // SD
      char *s = strrchr (url, '/');
      if (!s)
         s = url;
      if (s)
      {
         char *fn = NULL;
         if (*s == '/')
            s++;
         asprintf (&fn, "%s/%s", sd_mount, s);
         char *q = fn + sizeof (sd_mount);
         while (*q && isalnum ((int) (uint8_t) * q))
            q++;
         if (*q == '.')
         {
            q++;
            while (*q && isalnum ((int) (uint8_t) * q))
               q++;
         }
         *q = 0;
         if (i->data && response == 200)
         {                      // Save to card
            FILE *f = fopen (fn, "w");
            if (f)
            {
               jo_t j = jo_object_alloc ();
               if (fwrite (i->data, i->size, 1, f) != 1)
                  jo_string (j, "error", "write failed");
               fclose (f);
               jo_string (j, "write", fn);
               revk_info ("SD", &j);
               ESP_LOGE (TAG, "Write %s %lu", fn, i->size);
            } else
               ESP_LOGE (TAG, "Write fail %s", fn);
         } else if (!i->card && (!i->data || (response && response != 304 && response != -1)))
         {                      // Load from card
            i->card = 1;        // card tried, no need to try again
            FILE *f = fopen (fn, "r");
            if (f)
            {
               struct stat s;
               fstat (fileno (f), &s);
               free (buf);
               buf = mallocspi (s.st_size);
               if (buf)
               {
                  if (fread (buf, s.st_size, 1, f) == 1)
                  {
                     if (i->data && i->size == s.st_size && !memcmp (buf, i->data, i->size))
                     {
                        free (buf);
                        response = 0;   // No change
                     } else
                     {
                        ESP_LOGE (TAG, "Read %s", fn);
                        jo_t j = jo_object_alloc ();
                        jo_string (j, "read", fn);
                        revk_info ("SD", &j);
                        response = 200; // Treat as received
                        free (i->data);
                        i->data = buf;
                        i->size = s.st_size;
                        check_file (i);
                     }
                     buf = NULL;
                  }
               }
               fclose (f);
            } else
               ESP_LOGE (TAG, "Read fail %s", fn);
         }
         free (fn);
      }
   }
   free (buf);
   free (url);
   return i;
}

// Image plot

typedef struct plot_s
{
   gfx_pos_t ox,
     oy;
} plot_t;

static void *
my_alloc (void *opaque, uInt items, uInt size)
{
   return mallocspi (items * size);
}

static void
my_free (void *opaque, void *address)
{
   free (address);
}

static const char *
pixel (void *opaque, uint32_t x, uint32_t y, uint16_t r, uint16_t g, uint16_t b, uint16_t a)
{
   plot_t *p = opaque;
   if (a & 0x8000)
      gfx_pixel (p->ox + x, p->oy + y, (g & 0x8000) ? 255 : 0);
   return NULL;
}

void
plot (file_t * i, gfx_pos_t ox, gfx_pos_t oy)
{
   plot_t settings = { ox, oy };
   lwpng_decode_t *p = lwpng_decode (&settings, NULL, &pixel, &my_alloc, &my_free, NULL);
   lwpng_data (p, i->size, i->data);
   const char *e = lwpng_decoded (&p);
   if (e)
      ESP_LOGE (TAG, "PNG fail %s", e);
}

void
image_load (const char *name, file_t * i, char c)
{                               // Load image and set LEDs (image can be prefixed with colour, else default is used)
   int n = 0;
   if (name)
   {
      if (*name == '*')
         name++;                // Skip, refresh actually done in calling side
      const char *colours = name;
      while (*colours && isalpha ((int) (unsigned char) *colours))
         colours++;
      if (*colours == ':')
      {                         // Colours
         colours = name;
         while (isalpha ((int) (unsigned char) *colours) && n < sizeof (led_colour))
            led_colour[n++] = *colours++;
      } else
         led_colour[n++] = c;   // Single from arg
   } else
      led_colour[n++] = c;      // Single from arg
   while (n < sizeof (led_colour))
      led_colour[n++] = 0;
   if (i && i->data)
   {
      gfx_colour (imageplot == REVK_SETTINGS_IMAGEPLOT_NORMAL || imageplot == REVK_SETTINGS_IMAGEPLOT_MASK ? 'K' : 'W');
      gfx_background (imageplot == REVK_SETTINGS_IMAGEPLOT_NORMAL || imageplot == REVK_SETTINGS_IMAGEPLOT_MASKINVERT ? 'W' : 'K');
      plot (i, 0, 0);
      gfx_colour ('K');
      gfx_background ('W');
   }
}

file_t *
getimage (const char *name)
{
   name = skipcolour (name);
   if (!name || !*name)
      return NULL;
   char *url = NULL;
   asprintf (&url, "%s/%s.png", imageurl, name);
   file_t *i = download (url);
   free (url);
   return i;
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

void
epd_lock (void)
{
   xSemaphoreTake (epd_mutex, portMAX_DELAY);
   gfx_lock ();
}

void
epd_unlock (void)
{
   gfx_unlock ();
   xSemaphoreGive (epd_mutex);
}

#ifdef	CONFIG_LWPNG_ENCODE
static esp_err_t
web_frame (httpd_req_t * req)
{
   epd_lock ();
   uint8_t *png = NULL;
   size_t len = 0;
   uint32_t w = gfx_raw_w ();
   uint32_t h = gfx_raw_h ();
   uint8_t *b = gfx_raw_b ();
   ESP_LOGD (TAG, "Encode W=%lu H=%lu", w, h);
   lwpng_encode_t *p = lwpng_encode_1bit (w, h, &my_alloc, &my_free, NULL);
   if (b)
      while (h--)
      {
         lwpng_encode_scanline (p, b);
         b += (w + 7) / 8;
      }
   const char *e = lwpng_encoded (&p, &len, &png);
   ESP_LOGD (TAG, "Encoded %u bytes %s", len, e ? : "");
   if (e)
   {
      revk_web_head (req, *hostname ? hostname : appname);
      revk_web_send (req, e);
      revk_web_foot (req, 0, 1, NULL);
   } else
   {
      httpd_resp_set_type (req, "image/png");
      httpd_resp_send (req, (char *) png, len);
   }
   free (png);
   epd_unlock ();
   return ESP_OK;
}
#endif

static esp_err_t
web_root (httpd_req_t * req)
{
   if (revk_link_down ())
      return revk_web_settings (req);   // Direct to web set up
   web_head (req, *hostname ? hostname : appname);
   revk_web_send (req, "<p><a href=/push>Ding!</a></p>");
   if (card)
      revk_web_send (req, "<p>SD card mounted</p>");
#ifdef	CONFIG_LWPNG_ENCODE
   revk_web_send (req, "<p>");
   int32_t w = gfx_width ();
   int32_t h = gfx_height ();
#define DIV	2
   if (gfxflip & 4)
      revk_web_send (req, "<div style='display:inline-block;width:%dpx;height:%dpx;margin:5px;border:10px solid %s;border-%s:20px solid %s;'><img width=%d height=%d src='frame.png' style='transform:scale(%d,%d)rotate(90deg)translate(%dpx,%dpx);'></div>",  //
                     w / DIV, h / DIV,  //
                     gfxinvert ? "black" : "white",     //
                     gfxflip & 4 ? gfxflip & 2 ? "left" : "right" : gfxflip & 2 ? "top" : "bottom",     //
                     gfxinvert ? "black" : "white",     //
                     gfx_raw_w () / DIV, gfx_raw_h () / DIV,    //
                     gfxflip & 2 ? 1 : -1, gfxflip & 1 ? -1 : 1,        //
                     (h - w) / 2 / DIV * (gfxflip & 1 ? -1 : 1), (h - w) / 2 / DIV * (gfxflip & 2 ? 1 : -1));
   else
      revk_web_send (req, "<img src='frame.png' style='transform:scale(%d,%d);'>", gfxflip & 1 ? -1 : 1, gfxflip & 2 ? -1 : 1);
#undef	DIV
#endif
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
                        "<figure style='display:inline-block;background:white;border:10px solid white;border-left:20px solid white;margin:5px;%s'><img width=240 height=400 src='%s/%s.png'><figcaption style='margin:3px;padding:3px;background:#%06lX%s'>%s%s</figcaption></figure>",
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
   if (client || !prefix || target || strcmp (prefix, topiccommand) || !suffix)
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
nfc_task (void *arg)
{
   esp_err_t err = 0;
   if (!nfcrx.set && !nfctx.set)
   {
      vTaskDelete (NULL);
      return;
   }
   if (nfcrx.set)
   {                            // NFC function
      ESP_LOGE (TAG, "No NFC code yet");
      vTaskDelete (NULL);
      return;
   }
   // Monitor tx for updates for LEDs
   uart_config_t uart_config = {
      .baud_rate = 115200,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
   };
   if (!err)
      err = uart_param_config (NFCUART, &uart_config);
   if (!err)
      err = gpio_reset_pin (nfctx.num);
   if (!err)
      err = uart_set_pin (NFCUART, -1, nfctx.num, -1, -1);
   if (!err && !uart_is_driver_installed (NFCUART))
   {
      ESP_LOGE (TAG, "Installing UART driver %d", NFCUART);
      err = uart_driver_install (NFCUART, NFCBUF, 0, 0, NULL, 0);
   }
   if (err)
   {
      ESP_LOGE (TAG, "UART fail %s", esp_err_to_name (err));
      vTaskDelete (NULL);
      return;
   }
   uint8_t buf[NFCBUF];
   while (1)
   {
      int l = uart_read_bytes (NFCUART, buf, NFCBUF, 5 / portTICK_PERIOD_MS ? : 1);
      if (l <= 0)
         continue;
      uint8_t *p = buf,
         *e = buf + l;
      while (p + 2 < e && (*p || p[1] != 0xFF))
         p++;
      if (*p || p[1] != 0xFF)
         continue;
      p += 2;
      if (*p < 2 || *p != 0x100 - p[1] || *p - 2 > (e - p))
         continue;
      e = p + (*p) + 2;         // Ignoring checksum for now
      p += 2;
      if (*p != 0xD4)
         continue;
      p++;
      if (*p == 0x0E && e == p + 3)
      {
         uint8_t l = (p[1] & 0x3F) | ((p[2] & 6) << 6);
         static uint8_t last1 = 0,
            last2 = 0,
            last3 = 0,
            solid = 0,
            blink = 0;
         uint8_t c = ((last1 ^ l) | (last1 ^ last2) | (last2 ^ last3));
         last1 = last2;
         last2 = last3;
         nfcled = last3 = l;
         l &= ~c;
         if (blink != c || solid != l)
         {                      // Change, update actual LEDs.

            blink = c;
            solid = l;
            nfcledoverride = 255;
            //ESP_LOGE (TAG, "LED solid=%02X blink=%02X", solid, blink);
         }
      }
      //ESP_LOG_BUFFER_HEX_LEVEL (TAG, p, (int) (e - p), ESP_LOG_ERROR);
   }
}

void
push_task (void *arg)
{
   if (!btn1.set || revk_gpio_input (btn1))
   {
      ESP_LOGE (TAG, "No btn1");
      if (btn1.set)
      {
         jo_t j = jo_object_alloc ();
         jo_string (j, "error", "Btn init failed");
         jo_int (j, "gpio", btn1.num);
         revk_error ("btn1", &j);
      }
      vTaskDelete (NULL);
      return;
   }
   while (1)
   {
      uint8_t l = revk_gpio_get (btn1);
      if (l && !b.btn)
      {
         ESP_LOGE (TAG, "Pushed btn1");
         revk_info ("btn1", NULL);
         pushed = uptime () + holdtime;
      }
      b.btn = l;
      usleep (10000);
   }
}

void
led_task (void *arg)
{
   uint8_t or = 0,
      og = 0,
      ob = 0,
      n = 0;
   while (1)
   {
      if (nfcledoverride)
      {
         uint8_t s = 1;
         for (int i = 0; i < leds; i++)
         {
            uint8_t led = nfcled;
            char c = 'K';
            if (led)
            {
               while (s && !(s & led))
                  s <<= 1;
               if (!s)
               {
                  s = 1;
                  while (s && !(s & led))
                     s <<= 1;
               }
               if (s == 2)
                  c = 'G';
               else if (s == 4)
                  c = 'Y';
               else if (s == 8)
                  c = 'R';
               if (!(s <<= 1))
                  s = 1;
            }
            revk_led (strip, i, 255, revk_rgb (c));
         }
         led_strip_refresh (strip);
         usleep (10000);
         if (--nfcledoverride)
            continue;
         // Done
         for (int i = 0; i < leds; i++)
            revk_led (strip, i, 255, 0);
         or = og = ob = 0;
         led_strip_refresh (strip);
      }
      char c = led_colour[n];
      if (!c)
         c = led_colour[n = 0];
      if (++n >= sizeof (led_colour))
         n = 0;
      uint32_t rgb = revk_rgb (c);
      uint8_t r = (rgb >> 16),
         g = (rgb >> 8),
         b = rgb;
      if (r == or && g == og && b == ob && !led_colour[1])
      {                         // No change and not flashing
         usleep (10000);
         continue;
      }
      // Fade
      int t = 0;
      if (led_colour[1])
         t = 0xFF;              // Not fade as multi colour flashing
      for (; t <= 0xFF; t += 0xF)
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
         usleep (led_colour[1] ? 100000 : 50000);
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
   epd_mutex = xSemaphoreCreateMutex ();
   xSemaphoreGive (epd_mutex);

   revk_gpio_output (relay, 0);

   revk_task ("push", push_task, NULL, 4);
   revk_task ("nfc", nfc_task, NULL, 4);

   setactive (imagewait);

   if (leds)
   {
      led_strip_config_t strip_config = {
         .strip_gpio_num = (rgb.num),
         .max_leds = leds,
         .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
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
   config.max_uri_handlers = 6 + revk_num_web_handlers ();
   if (!httpd_start (&webserver, &config))
   {
      register_get_uri ("/", web_root);
      register_get_uri ("/apple-touch-icon.png", web_icon);
      register_get_uri ("/push", web_push);
      register_get_uri ("/message", web_message);
      register_get_uri ("/active", web_active);
#ifdef	CONFIG_LWPNG_ENCODE
      if (gfx_bpp () == 1)
         register_get_uri ("/frame.png", web_frame);
#endif
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
   if (sdcmd.set)
   {
      revk_gpio_input (sdcd);
      sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT ();
      slot.clk = sdclk.num;
      slot.cmd = sdcmd.num;
      slot.d0 = sddat0.num;
      slot.d1 = sddat1.set ? sddat1.num : -1;
      slot.d2 = sddat2.set ? sddat2.num : -1;
      slot.d3 = sddat3.set ? sddat3.num : -1;
      //slot.cd = sdcd.set ? sdcd.num : -1; // We do CD, and not sure how we would tell it polarity
      slot.width = (sddat2.set && sddat3.set ? 4 : sddat1.set ? 2 : 1);
      if (slot.width == 1)
         slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP; // Old boards?
      sdmmc_host_t host = SDMMC_HOST_DEFAULT ();
      host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
      host.slot = SDMMC_HOST_SLOT_1;
      esp_vfs_fat_sdmmc_mount_config_t mount_config = {
         .format_if_mount_failed = 1,
         .max_files = 2,
         .allocation_unit_size = 16 * 1024,
         .disk_status_check_enable = 1,
      };
      if (esp_vfs_fat_sdmmc_mount (sd_mount, &host, &slot, &mount_config, &card))
      {
         jo_t j = jo_object_alloc ();
         ESP_LOGE (TAG, "SD Mount failed");
         jo_string (j, "error", "Failed to mount");
         revk_error ("SD", &j);
         card = NULL;
      } else
         ESP_LOGE (TAG, "SD Mounted");
   }

   epd_lock ();
   gfx_clear (255);             // Black
   epd_unlock ();

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
                        if (n == 1 || ip[i].addr[0] != 0x000080FE)      // Yeh FE80 backwards
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
            epd_lock ();
            gfx_clear (0);
            gfx_message ((char *) overridemsg);
            *overridemsg = 0;
            addqr ();
            epd_unlock ();
         }
      }
      if (*overridename)
      {                         // Special override
         ESP_LOGE (TAG, "Override: %s", overridename);
         char *t = strdup (overridename);
         *overridename = 0;
         file_t *i = getimage (t);
         if (i)
         {
            if (override < up)
               override = up + holdtime;
            last = 0;
            for (int n = 0; n < 3; n++)
            {
               if (!n && *t == '*')
                  gfx_refresh ();
               epd_lock ();
               gfx_clear (0);
               image_load (t, i, 'B');
               addqr ();
               epd_unlock ();
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
         if (last)
         {                      // Show, and reinforce image
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
            epd_lock ();
            gfx_clear (0);
            if (!active)
               gfx_message ("/ / / / / / /PLEASE/WAIT");
            else
               image_load (activename, active, 'B');
            if (last && *activename == '*')
               gfx_refresh ();
            addqr ();
            epd_unlock ();
            if (last)
               revk_gpio_set (relay, 0);
            last = 0;
         }
      } else if (last != now / UPDATERATE)
      {                         // Show idle
         if (!idle)
            idle = getimage (basename);
         epd_lock ();
         gfx_clear (0);
         if (!last || (refresh && lastrefresh != now / refresh) || (gfxnight && t.tm_hour >= 2 && t.tm_hour < 4))
         {
            lastrefresh = now / refresh;
            gfx_refresh ();
         }
         last = now / UPDATERATE;
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
         epd_unlock ();
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
