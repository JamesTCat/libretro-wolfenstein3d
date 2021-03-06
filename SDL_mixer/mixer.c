/*
  SDL_mixer:  An audio mixer library based on the SDL library
  Copyright (C) 1997-2013 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* $Id$ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL_mixer.h"
#include "../surface.h"

/* Magic numbers for various audio file formats */
#define RIFF        0x46464952      /* "RIFF" */
#define WAVE        0x45564157      /* "WAVE" */

static int audio_opened = 0;
static SDL_AudioSpec mixer;

typedef struct _Mix_effectinfo
{
    Mix_EffectFunc_t callback;
    Mix_EffectDone_t done_callback;
    void *udata;
    struct _Mix_effectinfo *next;
} effect_info;

static struct _Mix_Channel
{
    Mix_Chunk *chunk;
    int playing;
    int paused;
    uint8_t *samples;
    int volume;
    int looping;
    int tag;
    uint32_t expire;
    uint32_t start_time;
    Mix_Fading fading;
    int fade_volume;
    int fade_volume_reset;
    uint32_t fade_length;
    uint32_t ticks_fade;
    effect_info *effects;
} *mix_channel = NULL;

static effect_info *posteffects = NULL;

static int num_channels;
static int reserved_channels = 0;

/* rcg07062001 callback to alert when channels are done playing. */
static void (*channel_done_callback)(int channel) = NULL;

/* Music function declarations */
extern int open_music(SDL_AudioSpec *mixer);
extern void close_music(void);

/* Support for user defined music functions, plus the default one */
extern int volatile music_active;
extern void music_mixer(void *udata, uint8_t *stream, int len);
static void (*mix_music)(void *udata, uint8_t *stream, int len) = music_mixer;
static void *music_data = NULL;

/* rcg06042009 report available decoders at runtime. */
static const char **chunk_decoders = NULL;
static int num_decoders = 0;

int Mix_GetNumChunkDecoders(void)
{
   return(num_decoders);
}

const char *Mix_GetChunkDecoder(int index)
{
   if ((index < 0) || (index >= num_decoders))
      return NULL;
   return(chunk_decoders[index]);
}

static void add_chunk_decoder(const char *decoder)
{
   void *ptr = realloc((void *)chunk_decoders, (num_decoders + 1) * sizeof (const char *));
   if (!ptr)
      return;  /* oh well, go on without it. */
   chunk_decoders = (const char **) ptr;
   chunk_decoders[num_decoders++] = decoder;
}

static int initialized = 0;

int Mix_Init(int flags)
{
   return 0;
}

void Mix_Quit(void)
{
   initialized = 0;
}

static void _Mix_channel_done_playing(int channel)
{
   if (channel_done_callback)
      channel_done_callback(channel);
}

static void *Mix_DoEffects(int chan, void *snd, int len)
{
   int posteffect = (chan == MIX_CHANNEL_POST);
   effect_info *e = ((posteffect) ? posteffects : mix_channel[chan].effects);
   void *buf = snd;

   /* are there any registered effects? */
   if (e != NULL)
   {
      /* if this is the postmix, we can just overwrite the original. */
      if (!posteffect)
      {
         buf = malloc(len);
         if (buf == NULL)
            return(snd);
         memcpy(buf, snd, len);
      }

      for (; e != NULL; e = e->next)
      {
         if (e->callback != NULL)
            e->callback(chan, buf, len, e->udata);
      }
   }

   /* be sure to free() the return value if != snd ... */
   return(buf);
}


/* Mixing function */
static void mix_channels(void *udata, uint8_t *stream, int len)
{
   uint8_t *mix_input;
   int i, mixable, volume = SDL_MIX_MAXVOLUME;
   uint32_t sdl_ticks;

   /* Need to initialize the stream in SDL 1.3+ */
   memset(stream, mixer.silence, len);

   /* Mix the music (must be done before the channels are added) */
   if ( music_active || (mix_music != music_mixer) )
      mix_music(music_data, stream, len);

   /* Mix any playing channels... */
   sdl_ticks = LR_GetTicks();
   for ( i=0; i<num_channels; ++i )
   {
      if( ! mix_channel[i].paused )
      {
         if ( mix_channel[i].expire > 0 && mix_channel[i].expire < sdl_ticks )
         {
            /* Expiration delay for that channel is reached */
            mix_channel[i].playing = 0;
            mix_channel[i].looping = 0;
            mix_channel[i].fading = MIX_NO_FADING;
            mix_channel[i].expire = 0;
            _Mix_channel_done_playing(i);
         }
         else if ( mix_channel[i].fading != MIX_NO_FADING )
         {
            uint32_t ticks = sdl_ticks - mix_channel[i].ticks_fade;
            if( ticks > mix_channel[i].fade_length )
            {
               Mix_Volume(i, mix_channel[i].fade_volume_reset); /* Restore the volume */
               if( mix_channel[i].fading == MIX_FADING_OUT )
               {
                  mix_channel[i].playing = 0;
                  mix_channel[i].looping = 0;
                  mix_channel[i].expire = 0;
                  _Mix_channel_done_playing(i);
               }
               mix_channel[i].fading = MIX_NO_FADING;
            }
            else
            {
               if( mix_channel[i].fading == MIX_FADING_OUT )
               {
                  Mix_Volume(i, (mix_channel[i].fade_volume * (mix_channel[i].fade_length-ticks))
                        / mix_channel[i].fade_length );
               }
               else
                  Mix_Volume(i, (mix_channel[i].fade_volume * ticks) / mix_channel[i].fade_length );
            }
         }

         if ( mix_channel[i].playing > 0 )
         {
            int index = 0;
            int remaining = len;
            while (mix_channel[i].playing > 0 && index < len)
            {
               remaining = len - index;
               volume = (mix_channel[i].volume*mix_channel[i].chunk->volume) / MIX_MAX_VOLUME;
               mixable = mix_channel[i].playing;
               if ( mixable > remaining )
                  mixable = remaining;

               mix_input = Mix_DoEffects(i, mix_channel[i].samples, mixable);
               SDL_MixAudio(stream+index,mix_input,mixable,volume);
               if (mix_input != mix_channel[i].samples)
                  free(mix_input);

               mix_channel[i].samples += mixable;
               mix_channel[i].playing -= mixable;
               index += mixable;

               /* rcg06072001 Alert app if channel is done playing. */
               if (!mix_channel[i].playing && !mix_channel[i].looping)
                  _Mix_channel_done_playing(i);
            }

            /* If looping the sample and we are at its end, make sure
               we will still return a full buffer */
            while ( mix_channel[i].looping && index < len )
            {
               int alen = mix_channel[i].chunk->alen;
               remaining = len - index;
               if (remaining > alen)
                  remaining = alen;

               mix_input = Mix_DoEffects(i, mix_channel[i].chunk->abuf, remaining);
               SDL_MixAudio(stream+index, mix_input, remaining, volume);
               if (mix_input != mix_channel[i].chunk->abuf)
                  free(mix_input);

               if (mix_channel[i].looping > 0)
                  --mix_channel[i].looping;
               mix_channel[i].samples = mix_channel[i].chunk->abuf + remaining;
               mix_channel[i].playing = mix_channel[i].chunk->alen - remaining;
               index += remaining;
            }
            if ( ! mix_channel[i].playing && mix_channel[i].looping )
            {
               if (mix_channel[i].looping > 0)
                  --mix_channel[i].looping;
               mix_channel[i].samples = mix_channel[i].chunk->abuf;
               mix_channel[i].playing = mix_channel[i].chunk->alen;
            }
         }
      }
   }

   /* rcg06122001 run posteffects... */
   Mix_DoEffects(MIX_CHANNEL_POST, stream, len);
}

/* Open the mixer with a certain desired audio format */
int Mix_OpenAudio(int frequency, uint16_t format, int nchannels, int chunksize)
{
   int i;
   SDL_AudioSpec desired;

   /* If the mixer is already opened, increment open count */
   if ( audio_opened )
   {
      if ( format == mixer.format && nchannels == mixer.channels )
      {
         ++audio_opened;
         return(0);
      }
      while ( audio_opened )
         Mix_CloseAudio();
   }

   /* Set the desired format and frequency */
   desired.freq = frequency;
   desired.format = format;
   desired.channels = nchannels;
   desired.samples = chunksize;
   desired.callback = mix_channels;
   desired.userdata = NULL;

   /* Accept nearly any audio format */
   if ( SDL_OpenAudio(&desired, &mixer) < 0 )
      return(-1);

   /* Initialize the music players */
   if ( open_music(&mixer) < 0 )
   {
      SDL_CloseAudio();
      return(-1);
   }

   num_channels = MIX_CHANNELS;
   mix_channel = (struct _Mix_Channel *) malloc(num_channels * sizeof(struct _Mix_Channel));

   /* Clear out the audio channels */
   for ( i=0; i<num_channels; ++i )
   {
      mix_channel[i].chunk = NULL;
      mix_channel[i].playing = 0;
      mix_channel[i].looping = 0;
      mix_channel[i].volume = SDL_MIX_MAXVOLUME;
      mix_channel[i].fade_volume = SDL_MIX_MAXVOLUME;
      mix_channel[i].fade_volume_reset = SDL_MIX_MAXVOLUME;
      mix_channel[i].fading = MIX_NO_FADING;
      mix_channel[i].tag = -1;
      mix_channel[i].expire = 0;
      mix_channel[i].effects = NULL;
      mix_channel[i].paused = 0;
   }
   Mix_VolumeMusic(SDL_MIX_MAXVOLUME);

   /* This list is (currently) decided at build time. */
   add_chunk_decoder("WAVE");

   audio_opened = 1;
   SDL_PauseAudio(0);
   return(0);
}

/* Dynamically change the number of channels managed by the mixer.
   If decreasing the number of channels, the upper channels are
   stopped.
   */
int Mix_AllocateChannels(int numchans)
{
   if (numchans<0 || numchans==num_channels)
      return(num_channels);

   if ( numchans < num_channels )
   {
      /* Stop the affected channels */
      int i;
      for(i=numchans; i < num_channels; i++)
      {
         Mix_HaltChannel(i);
      }
   }
   mix_channel = (struct _Mix_Channel *) realloc(mix_channel, numchans * sizeof(struct _Mix_Channel));
   if ( numchans > num_channels )
   {
      /* Initialize the new channels */
      int i;
      for(i=num_channels; i < numchans; i++)
      {
         mix_channel[i].chunk = NULL;
         mix_channel[i].playing = 0;
         mix_channel[i].looping = 0;
         mix_channel[i].volume = SDL_MIX_MAXVOLUME;
         mix_channel[i].fade_volume = SDL_MIX_MAXVOLUME;
         mix_channel[i].fade_volume_reset = SDL_MIX_MAXVOLUME;
         mix_channel[i].fading = MIX_NO_FADING;
         mix_channel[i].tag = -1;
         mix_channel[i].expire = 0;
         mix_channel[i].effects = NULL;
         mix_channel[i].paused = 0;
      }
   }
   num_channels = numchans;
   return(num_channels);
}

/* Return the actual mixer parameters */
int Mix_QuerySpec(int *frequency, uint16_t *format, int *channels)
{
   if ( audio_opened )
   {
      if ( frequency )
         *frequency = mixer.freq;
      if ( format )
         *format = mixer.format;
      if ( channels )
         *channels = mixer.channels;
   }
   return(audio_opened);
}


/*
 * !!! FIXME: Ideally, we want a Mix_LoadSample_RW(), which will handle the
 *             generic setup, then call the correct file format loader.
 */

/* Load a wave file */
Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc)
{
   uint32_t magic;
   Mix_Chunk *chunk;
   SDL_AudioSpec wavespec, *loaded;
   SDL_AudioCVT wavecvt;
   int samplesize;

   /* rcg06012001 Make sure src is valid */
   if ( ! src )
      return(NULL);

   /* Make sure audio has been opened */
   if ( ! audio_opened )
   {
      if ( freesrc )
         SDL_RWclose(src);
      return(NULL);
   }

   /* Allocate the chunk memory */
   chunk = (Mix_Chunk *)malloc(sizeof(Mix_Chunk));
   if ( chunk == NULL )
   {
      if ( freesrc )
         SDL_RWclose(src);
      return(NULL);
   }

   /* Find out what kind of audio file this is */
   magic = SDL_ReadLE32(src);
   /* Seek backwards for compatibility with older loaders */
   SDL_RWseek(src, -(int)sizeof(uint32_t), RW_SEEK_CUR);

   switch (magic)
   {
      case WAVE:
      case RIFF:
         loaded = SDL_LoadWAV_RW(src, freesrc, &wavespec,
               (uint8_t **)&chunk->abuf, &chunk->alen);
         break;
      default:
         if ( freesrc )
            SDL_RWclose(src);
         loaded = NULL;
         break;
   }

   if ( !loaded )
   {
      /* The individual loaders have closed src if needed */
      free(chunk);
      return(NULL);
   }

   /* Build the audio converter and create conversion buffers */
   if ( wavespec.format != mixer.format ||
         wavespec.channels != mixer.channels ||
         wavespec.freq != mixer.freq )
   {
      if ( SDL_BuildAudioCVT(&wavecvt,
               wavespec.format, wavespec.channels, wavespec.freq,
               mixer.format, mixer.channels, mixer.freq) < 0 )
      {
         free(chunk->abuf);
         free(chunk);
         return(NULL);
      }

      samplesize = ((wavespec.format & 0xFF)/8)*wavespec.channels;
      wavecvt.len = chunk->alen & ~(samplesize-1);
      wavecvt.buf = (uint8_t *)calloc(1, wavecvt.len*wavecvt.len_mult);

      if ( wavecvt.buf == NULL )
      {
         free(chunk->abuf);
         free(chunk);
         return(NULL);
      }
      memcpy(wavecvt.buf, chunk->abuf, chunk->alen);
      free(chunk->abuf);

      /* Run the audio converter */
      if ( SDL_ConvertAudio(&wavecvt) < 0 )
      {
         free(wavecvt.buf);
         free(chunk);
         return(NULL);
      }

      chunk->abuf = wavecvt.buf;
      chunk->alen = wavecvt.len_cvt;
   }

   chunk->allocated = 1;
   chunk->volume = MIX_MAX_VOLUME;

   return(chunk);
}

/* Load a wave file of the mixer format from a memory buffer */
Mix_Chunk *Mix_QuickLoad_WAV(uint8_t *mem)
{
   Mix_Chunk *chunk;
   uint8_t magic[4];

   /* Make sure audio has been opened */
   if ( ! audio_opened )
      return(NULL);

   /* Allocate the chunk memory */
   chunk = (Mix_Chunk *)calloc(1,sizeof(Mix_Chunk));
   if ( chunk == NULL )
      return(NULL);

   /* Essentially just skip to the audio data (no error checking - fast) */
   chunk->allocated = 0;
   mem += 12; /* WAV header */
   do {
      memcpy(magic, mem, 4);
      mem += 4;
      chunk->alen = ((mem[3]<<24)|(mem[2]<<16)|(mem[1]<<8)|(mem[0]));
      mem += 4;
      chunk->abuf = mem;
      mem += chunk->alen;
   } while ( memcmp(magic, "data", 4) != 0 );
   chunk->volume = MIX_MAX_VOLUME;

   return(chunk);
}

/* Free an audio chunk previously loaded */
void Mix_FreeChunk(Mix_Chunk *chunk)
{
   int i;

   /* Caution -- if the chunk is playing, the mixer will crash */
   if ( chunk )
   {
      /* Guarantee that this chunk isn't playing */
      if ( mix_channel )
      {
         for ( i=0; i<num_channels; ++i )
         {
            if ( chunk == mix_channel[i].chunk )
            {
               mix_channel[i].playing = 0;
               mix_channel[i].looping = 0;
            }
         }
      }
      /* Actually free the chunk */
      if ( chunk->allocated )
         free(chunk->abuf);
      free(chunk);
   }
}

/* Add your own music player or mixer function.
   If 'mix_func' is NULL, the default music player is re-enabled.
   */
void Mix_HookMusic(void (*mix_func)(void *udata, uint8_t *stream, int len),
      void *arg)
{
   if ( mix_func != NULL )
   {
      music_data = arg;
      mix_music = mix_func;
   }
   else
   {
      music_data = NULL;
      mix_music = music_mixer;
   }
}

void *Mix_GetMusicHookData(void)
{
   return(music_data);
}

void Mix_ChannelFinished(void (*channel_finished)(int channel))
{
   channel_done_callback = channel_finished;
}

/* Reserve the first channels (0 -> n-1) for the application, i.e. don't allocate
   them dynamically to the next sample if requested with a -1 value below.
   Returns the number of reserved channels.
   */
int Mix_ReserveChannels(int num)
{
   if (num > num_channels)
      num = num_channels;
   reserved_channels = num;
   return num;
}

static int checkchunkintegral(Mix_Chunk *chunk)
{
   int frame_width = 1;

   if ((mixer.format & 0xFF) == 16)
      frame_width = 2;
   frame_width *= mixer.channels;
   while (chunk->alen % frame_width)
      chunk->alen--;
   return chunk->alen;
}

/* Play an audio chunk on a specific channel.
   If the specified channel is -1, play on the first free channel.
   'ticks' is the number of milliseconds at most to play the sample, or -1
   if there is no limit.
   Returns which channel was used to play the sound.
   */
int Mix_PlayChannelTimed(int which, Mix_Chunk *chunk, int loops, int ticks)
{
   int i;

   /* Don't play null pointers :-) */
   if ( chunk == NULL )
      return(-1);
   if ( !checkchunkintegral(chunk))
      return(-1);

   /* Lock the mixer while modifying the playing channels */
   /* If which is -1, play on the first free channel */
   if ( which == -1 )
   {
      for ( i=reserved_channels; i<num_channels; ++i )
      {
         if ( mix_channel[i].playing <= 0 )
            break;
      }
      if ( i == num_channels )
         which = -1;
      else
         which = i;
   }

   /* Queue up the audio data for this channel */
   if ( which >= 0 && which < num_channels )
   {
      uint32_t sdl_ticks = LR_GetTicks();
      if (Mix_Playing(which))
         _Mix_channel_done_playing(which);
      mix_channel[which].samples = chunk->abuf;
      mix_channel[which].playing = chunk->alen;
      mix_channel[which].looping = loops;
      mix_channel[which].chunk = chunk;
      mix_channel[which].paused = 0;
      mix_channel[which].fading = MIX_NO_FADING;
      mix_channel[which].start_time = sdl_ticks;
      mix_channel[which].expire = (ticks > 0) ? (sdl_ticks + ticks) : 0;
   }

   /* Return the channel on which the sound is being played */
   return(which);
}

/* Change the expiration delay for a channel */
int Mix_ExpireChannel(int which, int ticks)
{
   int status = 0;

   if ( which == -1 )
   {
      int i;
      for ( i=0; i < num_channels; ++ i )
         status += Mix_ExpireChannel(i, ticks);
   }
   else if ( which < num_channels )
   {
      mix_channel[which].expire = (ticks>0) ? (LR_GetTicks() + ticks) : 0;
      ++ status;
   }
   return(status);
}

/* Set volume of a particular channel */
int Mix_Volume(int which, int volume)
{
   int i;
   int prev_volume = 0;

   if ( which == -1 )
   {
      for ( i=0; i<num_channels; ++i )
         prev_volume += Mix_Volume(i, volume);
      prev_volume /= num_channels;
   }
   else if ( which < num_channels )
   {
      prev_volume = mix_channel[which].volume;
      if ( volume >= 0 )
      {
         if ( volume > SDL_MIX_MAXVOLUME )
            volume = SDL_MIX_MAXVOLUME;
         mix_channel[which].volume = volume;
      }
   }
   return(prev_volume);
}

/* Halt playing of a particular channel */
int Mix_HaltChannel(int which)
{
   int i;

   if ( which == -1 )
   {
      for ( i=0; i<num_channels; ++i )
         Mix_HaltChannel(i);
   }
   else if ( which < num_channels )
   {
      if (mix_channel[which].playing)
      {
         _Mix_channel_done_playing(which);
         mix_channel[which].playing = 0;
         mix_channel[which].looping = 0;
      }
      mix_channel[which].expire = 0;
      if(mix_channel[which].fading != MIX_NO_FADING) /* Restore volume */
         mix_channel[which].volume = mix_channel[which].fade_volume_reset;
      mix_channel[which].fading = MIX_NO_FADING;
   }
   return(0);
}

Mix_Fading Mix_FadingChannel(int which)
{
   if ( which < 0 || which >= num_channels )
      return MIX_NO_FADING;
   return mix_channel[which].fading;
}

/* Check the status of a specific channel.
   If the specified mix_channel is -1, check all mix channels.
   */
int Mix_Playing(int which)
{
   int status;

   status = 0;
   if ( which == -1 )
   {
      int i;

      for ( i=0; i<num_channels; ++i )
      {
         if ((mix_channel[i].playing > 0) ||
               mix_channel[i].looping)
            ++status;
      }
   }
   else if ( which < num_channels )
   {
      if ( (mix_channel[which].playing > 0) ||
            mix_channel[which].looping )
         ++status;
   }
   return(status);
}

/* rcg06072001 Get the chunk associated with a channel. */
Mix_Chunk *Mix_GetChunk(int channel)
{
   Mix_Chunk *retval = NULL;

   if ((channel >= 0) && (channel < num_channels))
      retval = mix_channel[channel].chunk;

   return(retval);
}

/* Close the mixer, halting all playing audio */
void Mix_CloseAudio(void)
{
   int i;

   if ( audio_opened )
   {
      if ( audio_opened == 1 )
      {
         close_music();
         Mix_HaltChannel(-1);
         SDL_CloseAudio();
         free(mix_channel);
         mix_channel = NULL;

         /* rcg06042009 report available decoders at runtime. */
         free((void *)chunk_decoders);
         chunk_decoders = NULL;
         num_decoders = 0;
      }
      --audio_opened;
   }
}

/* Pause a particular channel (or all) */
void Mix_Pause(int which)
{
   uint32_t sdl_ticks = LR_GetTicks();
   if ( which == -1 )
   {
      int i;

      for ( i=0; i<num_channels; ++i )
      {
         if ( mix_channel[i].playing > 0 )
            mix_channel[i].paused = sdl_ticks;
      }
   }
   else if (which < num_channels)
   {
      if ( mix_channel[which].playing > 0 )
         mix_channel[which].paused = sdl_ticks;
   }
}

/* Resume a paused channel */
void Mix_Resume(int which)
{
   uint32_t sdl_ticks = LR_GetTicks();

   if (which == -1)
   {
      int i;

      for ( i=0; i<num_channels; ++i )
      {
         if ( mix_channel[i].playing > 0 )
         {
            if(mix_channel[i].expire > 0)
               mix_channel[i].expire += sdl_ticks - mix_channel[i].paused;
            mix_channel[i].paused = 0;
         }
      }
   }
   else if ( which < num_channels )
   {
      if ( mix_channel[which].playing > 0 )
      {
         if(mix_channel[which].expire > 0)
            mix_channel[which].expire += sdl_ticks - mix_channel[which].paused;
         mix_channel[which].paused = 0;
      }
   }
}

int Mix_Paused(int which)
{
   if (which < 0)
   {
      int status = 0;
      int i;
      for( i=0; i < num_channels; ++i )
      {
         if (mix_channel[i].paused)
            ++ status;
      }
      return(status);
   }
   else if (which < num_channels)
      return(mix_channel[which].paused != 0);
   return(0);
}

/* Change the group of a channel */
int Mix_GroupChannel(int which, int tag)
{
   if ( which < 0 || which > num_channels )
      return(0);

   mix_channel[which].tag = tag;
   return(1);
}

/* Assign several consecutive channels to a group */
int Mix_GroupChannels(int from, int to, int tag)
{
   int status = 0;
   for( ; from <= to; ++ from )
      status += Mix_GroupChannel(from, tag);
   return(status);
}

/* Finds the first available channel in a group of channels */
int Mix_GroupAvailable(int tag)
{
   int i;
   for( i=0; i < num_channels; i ++ )
   {
      if ( ((tag == -1) || (tag == mix_channel[i].tag)) &&
            (mix_channel[i].playing <= 0) )
         return i;
   }
   return(-1);
}

/* Finds the "oldest" sample playing in a group of channels */
int Mix_GroupOldest(int tag)
{
   int i;
   int         chan = -1;
   uint32_t mintime = LR_GetTicks();

   for( i=0; i < num_channels; i ++ )
   {
      if ( (mix_channel[i].tag==tag || tag==-1) && mix_channel[i].playing > 0
            && mix_channel[i].start_time <= mintime )
      {
         mintime = mix_channel[i].start_time;
         chan = i;
      }
   }
   return(chan);
}

/* end of mixer.c ... */

