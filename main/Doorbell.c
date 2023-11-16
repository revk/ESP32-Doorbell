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
#include "images.h"

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
        u8(gfxflip,6)    \
	io(bellpush,10)	\
	u8(holdtime,30)	\

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

const char *
gfx_qr (const char *value)
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
   gfx_lock ();
   gfx_clear (0);
   int s = (w > h ? h : w) / width;
   ESP_LOGE (TAG, "QR %d/%d %d", w, h, s);
   int ox = (w - width * s) / 2;
   int oy = (h - width * s) / 2;
   for (int y = 0; y < width; y++)
      for (int x = 0; x < width; x++)
         if (qr[width * y + x] & QR_TAG_BLACK)
            for (int dy = 0; dy < s; dy++)
               for (int dx = 0; dx < s; dx++)
                  gfx_pixel (ox + x * s + dx, oy + y * s + dy, 0xFF);
   gfx_pos (1, 1, GFX_T | GFX_L);
   gfx_text (1, "%s", value);
   gfx_unlock ();
   free (qr);
#endif
   return NULL;
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
   if (!strcmp (suffix, "qr"))
   {
      // TODO set that this is an override
      return gfx_qr (value) ? : "";
   }
   if (!strcmp (suffix, "message"))
   {
      // TODO set that this is an override
      gfx_message (value);
      return "";
   }
   // TODO setting the status page number
   // TODO cancel, i.e. door open
   return NULL;
}

// --------------------------------------------------------------------------------
// Web
#ifdef	CONFIG_REVK_APCONFIG
#error 	Clash with CONFIG_REVK_APCONFIG set
#endif

uint32_t pushed = 0;
uint32_t override = 0;

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

   if (gfxmosi || gfxdc || gfxsck)
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
   uint8_t last = 0;
   while (1)
   {
      usleep (100000);
      time_t now = time (0);
      struct tm t;
      localtime_r (&now, &t);
      uint32_t up = uptime ();
      if (override + holdtime < up)
         override = 0;
      if (override)
         continue;
      if (pushed + holdtime < up)
         pushed = 0;            // Time out
      if (pushed)
      {                         // Bell was pushed
         if (last)
         {                      // Show status as was showing idle
            last = 0;
            // Send MQTT TODO
            // Show status page
            gfx_message ("[6]//PLEASE LEAVE/PARCELS/BEHIND/THE GATE//--->");
         }
      } else if (last != t.tm_mday)
      {                         // Show idle
         gfx_lock ();
         gfx_clear (0);
         gfx_pos (0, 0, 0);
         gfx_icon2 (480, 800, image_Idle);
	 gfx_pos(0,gfx_height()-1,GFX_B|GFX_L);
	 gfx_text(6,"%04d-%02d-%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday);
         gfx_unlock ();
         last = t.tm_mday;
      }
   }
}
