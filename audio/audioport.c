#include "stream.h"
#include <cell/audio.h>
#include <cell/sysmodule.h>
#include <sys/synchronization.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <altivec.h>

#include "buffer.h"
#include "resampler.h"

static void init_audioport(void)
{
   static int init_count = 0;
   if (init_count == 0)
   {
      cellSysmoduleLoadModule(CELL_SYSMODULE_AUDIO);
      cellAudioInit();
      init_count++;
   }
}

typedef struct audioport
{
   volatile uint64_t quit_thread;
   uint32_t audio_port;

   uint32_t channels;

   sys_lwmutex_t lock;
   sys_lwmutex_t cond_lock;
   sys_lwcond_t cond;
   pthread_t thread;

   resampler_t *re;
   float *re_buffer;
   int16_t *re_pull_buffer;

   fifo_buffer_t *buffer;

   cell_audio_sample_cb_t sample_cb;
   void *userdata;

   uint32_t is_paused;
} audioport_t;

static void audio_convert_s16_to_float_C(float *out, const int16_t *in, size_t samples)
{
   for (size_t i = 0; i < samples; i++)
      out[i] = (float)in[i] / 0x8000;
}

static void audio_convert_s16_to_float_altivec(float *out, const int16_t *in, size_t samples)
{
   // Unaligned loads/store is a bit expensive, so we optimize for the good path (very likely).
   if (((uintptr_t)out & 15) + ((uintptr_t)in & 15) == 0)
   {
      size_t i;
      for (i = 0; i + 8 <= samples; i += 8, in += 8, out += 8)
      {
         vector signed short input = vec_ld(0, in);
         vector signed int hi = vec_unpackh(input);
         vector signed int lo = vec_unpackl(input);
         vector float out_hi = vec_ctf(hi, 15);
         vector float out_lo = vec_ctf(lo, 15);

         vec_st(out_hi, 0, out);
         vec_st(out_lo, 16, out);
      }

      audio_convert_s16_to_float_C(out, in, samples - i);
   }
   else
      audio_convert_s16_to_float_C(out, in, samples);
}

static uint32_t resampler_cb(void *userdata, float **data)
{
   audioport_t *port = userdata;
   uint32_t has_read = 0;

   if (port->sample_cb)
      has_read = port->sample_cb(port->re_pull_buffer, CELL_AUDIO_BLOCK_SAMPLES * port->channels * sizeof(int16_t), port->userdata);
   else
   {
      has_read = CELL_AUDIO_BLOCK_SAMPLES * port->channels;
      sys_lwmutex_lock(&port->lock, SYS_NO_TIMEOUT);
      uint32_t avail = fifo_read_avail(port->buffer);
      if (avail < CELL_AUDIO_BLOCK_SAMPLES * port->channels * sizeof(int16_t))
         has_read = avail / sizeof(int16_t);

      fifo_read(port->buffer, port->re_pull_buffer, has_read * sizeof(int16_t));
      sys_lwmutex_unlock(&port->lock);
      sys_lwcond_signal(&port->cond);
   }

   if (has_read < CELL_AUDIO_BLOCK_SAMPLES * port->channels * 2)
      memset(port->re_pull_buffer + has_read, 0, (CELL_AUDIO_BLOCK_SAMPLES * port->channels - has_read) * sizeof(int16_t));

   audio_convert_s16_to_float_altivec(port->re_buffer, port->re_pull_buffer, CELL_AUDIO_BLOCK_SAMPLES * port->channels);

   *data = port->re_buffer;
   return CELL_AUDIO_BLOCK_SAMPLES;
}

static void resampler_event_loop(audioport_t *port, sys_event_queue_t id)
{
   sys_event_t event;
   port->re_buffer = memalign(128, CELL_AUDIO_BLOCK_SAMPLES * port->channels * sizeof(float));
   port->re_pull_buffer = memalign(128, CELL_AUDIO_BLOCK_SAMPLES * port->channels * sizeof(int16_t));

   float *res_buffer = memalign(128, CELL_AUDIO_BLOCK_SAMPLES * port->channels * sizeof(float));

   while (!port->quit_thread)
   {
      resampler_cb_read(port->re, CELL_AUDIO_BLOCK_SAMPLES, res_buffer);
      sys_event_queue_receive(id, &event, SYS_NO_TIMEOUT);
      cellAudioAddData(port->audio_port, res_buffer, CELL_AUDIO_BLOCK_SAMPLES, 1.0);
   }
   free(res_buffer);
   free(port->re_buffer);
   free(port->re_pull_buffer);
   port->re_buffer = NULL;
   port->re_pull_buffer = NULL;
}

static void pull_event_loop(audioport_t *port, sys_event_queue_t id)
{
   sys_event_t event;

   int16_t *in_buf = memalign(128, CELL_AUDIO_BLOCK_SAMPLES * port->channels * sizeof(int16_t));
   float *conv_buf = memalign(128, CELL_AUDIO_BLOCK_SAMPLES * port->channels * sizeof(float));
   while (!port->quit_thread)
   {
      uint32_t has_read = 0;
      if (port->sample_cb)
         has_read = port->sample_cb(in_buf, CELL_AUDIO_BLOCK_SAMPLES * port->channels, port->userdata);
      else
      {
         has_read = CELL_AUDIO_BLOCK_SAMPLES * port->channels;
         sys_lwmutex_lock(&port->lock, SYS_NO_TIMEOUT);
         uint32_t avail = fifo_read_avail(port->buffer);
         if (avail < CELL_AUDIO_BLOCK_SAMPLES * port->channels * sizeof(int16_t))
            has_read = avail / sizeof(int16_t);

         fifo_read(port->buffer, in_buf, has_read * sizeof(int16_t));
         sys_lwmutex_unlock(&port->lock);
      }

      if (has_read < CELL_AUDIO_BLOCK_SAMPLES * port->channels)
         memset(in_buf + has_read, 0, (CELL_AUDIO_BLOCK_SAMPLES * port->channels - has_read) * sizeof(int16_t));

      resampler_int16_t_to_float(conv_buf, in_buf, CELL_AUDIO_BLOCK_SAMPLES * port->channels);
      sys_event_queue_receive(id, &event, SYS_NO_TIMEOUT);
      cellAudioAddData(port->audio_port, conv_buf, CELL_AUDIO_BLOCK_SAMPLES, 1.0);

      pthread_cond_signal(&port->cond);
   }
   free(conv_buf);
}

static void* event_loop(void *data)
{
   audioport_t *port = data;

   sys_event_queue_t id;
   sys_ipc_key_t key;

   cellAudioCreateNotifyEventQueue(&id, &key);
   cellAudioSetNotifyEventQueue(key);

   if (port->re)
      resampler_event_loop(port, id);
   else
      pull_event_loop(port, id);

   cellAudioRemoveNotifyEventQueue(key);
   pthread_exit(NULL);
   return NULL;
}

static cell_audio_handle_t audioport_init(const struct cell_audio_params *params)
{
   init_audioport();

   audioport_t *handle = calloc(1, sizeof(*handle));

   CellAudioPortParam port_params = {
      .nChannel = params->channels,
      .nBlock = 8,
      .attr = 0
   };

   handle->channels = params->channels;

   handle->sample_cb = params->sample_cb;
   handle->userdata = params->userdata;
   handle->buffer = fifo_new(params->buffer_size ? params->buffer_size : 4096);

   if (params->samplerate != 48000)
   {
      handle->re = resampler_new(resampler_cb, 48000.0 / params->samplerate, params->channels, handle);
   }

   sys_lwmutex_attribute_t attr;
   sys_lwmutex_attribute_t attr2;
   sys_lwmutex_attribute_t cond_attr;

   sys_lwmutex_attribute_initialize(attr);
   sys_lwmutex_create(&handle->lock, &attr);

   sys_lwmutex_attribute_initialize(attr2);
   sys_lwmutex_create(&handle->cond_lock, &attr2);

   sys_lwcond_attribute_initialize(cond_attr);
   sys_lwcond_create(&handle->cond, &handle->cond_lock, &cond_attr);

   cellAudioPortOpen(&port_params, &handle->audio_port);
   cellAudioPortStart(handle->audio_port);

   pthread_create(&handle->thread, NULL, event_loop, handle);
   return handle;
}

static void audioport_pause(cell_audio_handle_t handle)
{
   audioport_t *port = handle;
   port->is_paused = 1;
   cellAudioPortStop(port->audio_port);
}

static int32_t audioport_unpause(cell_audio_handle_t handle)
{
   audioport_t *port = handle;
   port->is_paused = 0;
   cellAudioPortStart(port->audio_port);
   return 0;
}

static uint32_t audioport_is_paused(cell_audio_handle_t handle)
{
   audioport_t *port = handle;
   return port->is_paused;
}

static void audioport_free(cell_audio_handle_t handle)
{
   audioport_t *port = handle;

   port->quit_thread = 1;
   pthread_join(port->thread, NULL);

   sys_lwmutex_destroy(&port->lock);
   sys_lwmutex_destroy(&port->cond_lock);
   sys_lwcond_destroy(&port->cond);

   if (port->re)
      resampler_free(port->re);
   if (port->buffer)
      fifo_free(port->buffer);

   cellAudioPortStop(port->audio_port);
   cellAudioPortClose(port->audio_port);

   free(port);
}

static uint32_t audioport_write_avail(cell_audio_handle_t handle)
{
   audioport_t *port = handle;

   sys_lwmutex_lock(&port->lock, SYS_NO_TIMEOUT);
   uint32_t ret = fifo_write_avail(port->buffer);
   sys_lwmutex_unlock(&port->lock);
   return ret / sizeof(int16_t);
}

static int32_t audioport_write(cell_audio_handle_t handle, const int16_t *data, uint32_t samples)
{
   int32_t ret = samples;
   uint32_t bytes = samples * sizeof(int16_t);

   audioport_t *port = handle;
   while (bytes > 0)
   {
      sys_lwmutex_lock(&port->lock, SYS_NO_TIMEOUT);
      uint32_t avail = fifo_write_avail(port->buffer);
      sys_lwmutex_unlock(&port->lock);

      uint32_t to_write = avail < bytes ? avail : bytes;
      if (to_write > 0)
      {
	 sys_lwmutex_lock(&port->lock, SYS_NO_TIMEOUT);
         fifo_write(port->buffer, data, to_write);
	 sys_lwmutex_unlock(&port->lock);
         bytes -= to_write;
         data += to_write >> 1;
      }
      else
         sys_lwcond_wait(&port->cond, 0);
   }

   return ret;
}

static uint32_t audioport_alive(cell_audio_handle_t handle)
{
   (void)handle;
   return 1;
}

const cell_audio_driver_t cell_audio_audioport = {
   .init = audioport_init,
   .write = audioport_write,
   .write_avail = audioport_write_avail,
   .pause = audioport_pause,
   .unpause = audioport_unpause,
   .is_paused = audioport_is_paused,
   .alive = audioport_alive,
   .free = audioport_free
};
