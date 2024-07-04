#include "revk.h"
#include <driver/rmt_tx.h>
#include <driver/rmt_rx.h>
#include "cn_wired.h"
#include "cn_wired_driver.h"

static const char TAG[] = "Faikin";

#define	CN_WIRED_SYNC	2600    // uS
#define	CN_WIRED_START	(cnmark900?900:1000)    // uS
#define	CN_WIRED_SPACE	300     // uS
#define	CN_WIRED_0	400     // uS
#define	CN_WIRED_1	1000    // uS
#define	CN_WIRED_IDLE	16000   // uS
#define	CN_WIRED_TERM	2000    // uS
#define	CN_WIRED_MARGIN	200     // uS
rmt_channel_handle_t rmt_tx = NULL,
   rmt_rx = NULL;
rmt_encoder_handle_t rmt_encoder = NULL;
rmt_symbol_word_t rmt_rx_raw[70];       // Needs to allow for 66, extra is to spot longer messages
volatile size_t rmt_rx_len = 0; // Rx is ready
const rmt_receive_config_t rmt_rx_config = {
   .signal_range_min_ns = 1000, // shortest - to eliminate glitches
   .signal_range_max_ns = 10000000,     // longest - needs to be over the 2600uS sync pulse...
};

const rmt_transmit_config_t rmt_tx_config = {
   .flags.eot_level = 1,
};

bool
rmt_rx_callback (rmt_channel_handle_t channel, const rmt_rx_done_event_data_t * edata, void *user_data)
{
   if (edata->num_symbols < 64)
   {                            // Silly... restart rx
      rmt_rx_len = 0;
      rmt_receive (rmt_rx, rmt_rx_raw, sizeof (rmt_rx_raw), &rmt_rx_config);
      return pdFALSE;
   }
   // Got something
   rmt_rx_len = edata->num_symbols;
   return pdFALSE;
}

esp_err_t
cn_wired_driver_install (gpio_num_t rx_num, gpio_num_t tx_num, int rx_invert, int tx_invert)
{
   esp_err_t err = ESP_OK;

   if (rmt_rx || rmt_tx)
   {
      ESP_LOGE (TAG, "cn_wired driver already initialized");
      return ESP_FAIL;
   }

   if (!rmt_encoder)
   {
      rmt_copy_encoder_config_t encoder_config = {
      };
      err = REVK_ERR_CHECK (rmt_new_copy_encoder (&encoder_config, &rmt_encoder));
   }
   if (!err)
   {                         // Create rmt_tx
      rmt_tx_channel_config_t tx_chan_config = {
         .clk_src = RMT_CLK_SRC_DEFAULT,     // select source clock
         .gpio_num = tx_num, // GPIO number
         .mem_block_symbols = 72,    // symbols
         .resolution_hz = 1 * 1000 * 1000,   // 1 MHz tick resolution, i.e., 1 tick = 1 µs
         .trans_queue_depth = 1,     // set the number of transactions that can pend in the background
         .flags.invert_out = tx_invert,
#ifdef  CONFIG_IDF_TARGET_ESP32S3
         .flags.with_dma = true,
#endif
      };
      err = REVK_ERR_CHECK (rmt_new_tx_channel (&tx_chan_config, &rmt_tx));
      if (rmt_tx && !err)
         err = REVK_ERR_CHECK (rmt_enable (rmt_tx));
   }
   if (!err)
   {                         // Create rmt_rx
      rmt_rx_channel_config_t rx_chan_config = {
         .clk_src = RMT_CLK_SRC_DEFAULT,     // select source clock
         .resolution_hz = 1 * 1000 * 1000,   // 1MHz tick resolution, i.e. 1 tick = 1us
         .mem_block_symbols = 72,    // 
         .gpio_num = rx_num, // GPIO number
         .flags.invert_in = rx_invert,
#ifdef  CONFIG_IDF_TARGET_ESP32S3
         .flags.with_dma = true,
#endif
      };
      err = REVK_ERR_CHECK (rmt_new_rx_channel (&rx_chan_config, &rmt_rx));
      if (rmt_rx)
      {
         rmt_rx_event_callbacks_t cbs = {
            .on_recv_done = rmt_rx_callback,
         };
         err = REVK_ERR_CHECK (rmt_rx_register_event_callbacks (rmt_rx, &cbs, NULL));
         if (!err)
            err = REVK_ERR_CHECK (rmt_enable (rmt_rx));
      }
   }
   rmt_rx_len = 0;
   if (!err)
      err = REVK_ERR_CHECK (rmt_receive (rmt_rx, rmt_rx_raw, sizeof (rmt_rx_raw), &rmt_rx_config));
   return err;
}

void
cn_wired_driver_delete (void)
{
   if (rmt_tx)
   {
      REVK_ERR_CHECK (rmt_disable (rmt_tx));
      REVK_ERR_CHECK (rmt_del_channel (rmt_tx));
      rmt_tx = NULL;
   }
   if (rmt_rx)
   {
      REVK_ERR_CHECK (rmt_disable (rmt_rx));
      REVK_ERR_CHECK (rmt_del_channel (rmt_rx));
      rmt_rx = NULL;
   }
}

esp_err_t
cn_wired_read_bytes (uint8_t *rx, int wait)
{
   esp_err_t err = ESP_OK;
   uint32_t sum0 = 0,
      sum1 = 0,
      sums = 0,
      cnt0 = 0,
      cnt1 = 0,
      cnts = 0,
      sync = 0,
      start = 0;
   const char *e = NULL;
   int p = 0,
      dur = 0;

   if (!rmt_tx || !rmt_encoder || !rmt_rx)
   {
      // Not ready?
      return ESP_ERR_INVALID_STATE;
   }

   // Wait rx
   while (!rmt_rx_len && --wait)
      usleep (1000);
   if (!rmt_rx_len)
   {
      return ESP_ERR_TIMEOUT;
   }

   // Process receive
   // Sanity checking
   if (!e && rmt_rx_len != CN_WIRED_LEN * 8 + 2)
      e = "Wrong length";
   if (!e && rmt_rx_raw[p].level0)
      e = "Bad start polarity";
   if (!e && ((dur = rmt_rx_raw[p].duration0) < CN_WIRED_SYNC - CN_WIRED_MARGIN || dur > CN_WIRED_SYNC + CN_WIRED_MARGIN))
      e = "Bad start duration";
   sync = rmt_rx_raw[p].duration0;
   if (!e && ((dur = rmt_rx_raw[p].duration1) < CN_WIRED_START - CN_WIRED_MARGIN || dur > CN_WIRED_START + CN_WIRED_MARGIN))
      e = "Bad start bit";
   start = rmt_rx_raw[p].duration1;
   p++;
   for (int i = 0; !e && i < CN_WIRED_LEN; i++)
   {
      rx[i] = 0;
      for (uint8_t b = 0x01; !e && b; b <<= 1)
      {
         if (!e
               && ((dur = rmt_rx_raw[p].duration0) < CN_WIRED_SPACE - CN_WIRED_MARGIN || dur > CN_WIRED_SPACE + CN_WIRED_MARGIN))
            e = "Bad space duration";
         sums += rmt_rx_raw[p].duration0;
         cnts++;
         if (!e && (dur = rmt_rx_raw[p].duration1) > CN_WIRED_1 - CN_WIRED_MARGIN && dur < CN_WIRED_1 + CN_WIRED_MARGIN)
         {
            rx[i] |= b;
            sum1 += rmt_rx_raw[p].duration1;
            cnt1++;
         } else if (!e && ((dur = rmt_rx_raw[p].duration1) < CN_WIRED_0 - CN_WIRED_MARGIN || dur > CN_WIRED_1 + CN_WIRED_MARGIN))
            e = "Bad bit duration";
         else
         {
            sum0 += rmt_rx_raw[p].duration1;
            cnt0++;
         }
         p++;
      }
   }
   if (e)
   {
      jo_t j = jo_comms_alloc ();
      jo_string (j, "error", e);
      //jo_int (j, "ts", esp_timer_get_time ());
      if (dur)
         jo_int (j, "duration", dur);
      if (p > 1)
         jo_base16 (j, "data", rx, (p - 1) / 8);
      jo_int (j, "sync", sync);
      jo_int (j, "start", start);
      jo_int (j, "rmt_rx_len", rmt_rx_len);
      if (cnts)
         jo_int (j, "space", sums / cnts);
      if (cnt0)
         jo_int (j, "0", sum0 / cnt0);
      if (cnt1)
         jo_int (j, "1", sum1 / cnt1);
      revk_error ("comms", &j);
      err = ESP_ERR_INVALID_RESPONSE;
   } else if (dump)
   {
      jo_t j = jo_comms_alloc ();
      //jo_int (j, "ts", esp_timer_get_time ());
      jo_base16 (j, "dump", rx, CN_WIRED_LEN);
      jo_int (j, "sync", sync);
      jo_int (j, "start", start);
      if (cnts)
         jo_int (j, "space", sums / cnts);
      if (cnt0)
         jo_int (j, "0", sum0 / cnt0);
      if (cnt1)
         jo_int (j, "1", sum1 / cnt1);
      revk_info ("rx", &j);
   }

   // Next Rx
   rmt_rx_len = 0;
   REVK_ERR_CHECK (rmt_receive (rmt_rx, rmt_rx_raw, sizeof (rmt_rx_raw), &rmt_rx_config));

   return err;
}

esp_err_t
cn_wired_write_bytes (const uint8_t *buf)
{
   esp_err_t err;
   // Encode manually, yes, silly, but bytes encoder has no easy way to add the start bits.
   rmt_symbol_word_t seq[3 + CN_WIRED_LEN * 8 + 1];
   int p = 0;
   seq[p].duration0 = CN_WIRED_SYNC - 1000;  // 2500us low - do in two parts? so we start with high for data
   seq[p].level0 = 0;
   seq[p].duration1 = 1000;
   seq[p++].level1 = 0;
   void add (int d)
   {
      seq[p].duration0 = d;
      seq[p].level0 = 1;
      seq[p].duration1 = CN_WIRED_SPACE;
      seq[p++].level1 = 0;
   }
   add (CN_WIRED_START);
   for (int i = 0; i < CN_WIRED_LEN; i++)
      for (uint8_t b = 0x01; b; b <<= 1)
         add ((buf[i] & b) ? CN_WIRED_1 : CN_WIRED_0);
   seq[p].duration0 = CN_WIRED_IDLE;
   seq[p].level0 = 1;
   seq[p].duration1 = CN_WIRED_TERM;
   seq[p++].level1 = 0;

   err = REVK_ERR_CHECK (rmt_transmit (rmt_tx, rmt_encoder, seq, p * sizeof (rmt_symbol_word_t), &rmt_tx_config));
   if (!err)
      err = REVK_ERR_CHECK (rmt_tx_wait_all_done (rmt_tx, 1000));
   return err;
}
