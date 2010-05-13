/*
 * Copyright (C) 2010 Julien BLACHE <jb@jblache.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#if defined(__linux__)
# include <sys/timerfd.h>
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
# include <sys/time.h>
# include <sys/event.h>
#endif

#if defined(HAVE_SYS_EVENTFD_H) && defined(HAVE_EVENTFD)
# define USE_EVENTFD
# include <sys/eventfd.h>
#endif

#include <avahi-common/malloc.h>

#include <event.h>

#include <gcrypt.h>

#include "db.h"
#include "daap_query.h"
#include "logger.h"
#include "mdns_avahi.h"
#include "conffile.h"
#include "misc.h"
#include "rng.h"
#include "transcode.h"
#include "player.h"
#include "raop.h"
#include "laudio.h"


#ifndef MIN
# define MIN(a, b) ((a < b) ? a : b)
#endif

#define VAR_PLAYER_VOLUME "player:volume"

enum player_sync_source
  {
    PLAYER_SYNC_CLOCK,
    PLAYER_SYNC_LAUDIO,
  };

typedef int (*cmd_func)(void *arg);

struct player_command
{
  cmd_func func;
  cmd_func func_bh;
  void *arg;
  int ret;

  int raop_pending;
};

struct player_source
{
  uint32_t id;

  uint64_t stream_start;
  uint64_t output_start;
  uint64_t end;

  struct transcode_ctx *ctx;

  struct player_source *pl_next;
  struct player_source *pl_prev;

  struct player_source *shuffle_next;
  struct player_source *shuffle_prev;

  struct player_source *play_next;
};


struct event_base *evbase_player;

#ifdef USE_EVENTFD
static int exit_efd;
static int cmd_efd;
#else
static int exit_pipe[2];
static int cmd_pipe[2];
#endif
static int player_exit;
static struct event exitev;
static struct event cmdev;
static pthread_t tid_player;

/* Player status */
static enum play_status player_state;
static enum repeat_mode repeat;
static char shuffle;

/* Status updates (for DACP) */
static int update_fd;

/* Playback timer */
static int pb_timer_fd;
static struct event pb_timer_ev;
#if defined(__linux__)
static struct timespec pb_timer_last;
#endif

/* Sync source */
static enum player_sync_source pb_sync_source;

/* Sync values */
static struct timespec pb_pos_stamp;
static uint64_t pb_pos;

/* Stream position (packets) */
static uint64_t last_rtptime;

/* AirTunes devices */
static struct raop_device *dev_list;
static pthread_mutex_t dev_lck = PTHREAD_MUTEX_INITIALIZER;

/* Device status */
static enum laudio_state laudio_status;
static int laudio_selected;
static int raop_sessions;

/* Commands */
static struct player_command cmd;
static pthread_mutex_t cmd_lck = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cmd_cond = PTHREAD_COND_INITIALIZER;

/* Last commanded volume */
static int volume;

/* Shuffle RNG state */
struct rng_ctx shuffle_rng;

/* Audio source */
static struct player_source *source_head;
static struct player_source *shuffle_head;
static struct player_source *cur_playing;
static struct player_source *cur_streaming;
static struct evbuffer *audio_buf;


static void
status_update(enum play_status status)
{
#ifndef USE_EVENTFD
  int dummy = 42;
#endif
  int ret;

  player_state = status;

  if (update_fd < 0)
    return;

#ifdef USE_EVENTFD
  ret = eventfd_write(update_fd, 1);
  if (ret < 0)
#else
  ret = write(update_fd, &dummy, sizeof(dummy));
  if (ret != sizeof(dummy))
#endif
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not send status update: %s\n", strerror(errno));
    }
}


static int
player_get_current_pos_clock(uint64_t *pos, struct timespec *ts, int commit)
{
  uint64_t delta;
  int ret;

  ret = clock_gettime(CLOCK_MONOTONIC, ts);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Couldn't get clock: %s\n", strerror(errno));

      return -1;
    }

  delta = (ts->tv_sec - pb_pos_stamp.tv_sec) * 1000000 + (ts->tv_nsec - pb_pos_stamp.tv_nsec) / 1000;

#ifdef DEBUG_SYNC
  DPRINTF(E_DBG, L_PLAYER, "Delta is %" PRIu64 " usec\n", delta);
#endif

  delta = (delta * 44100) / 1000000;

#ifdef DEBUG_SYNC
  DPRINTF(E_DBG, L_PLAYER, "Delta is %" PRIu64 " samples\n", delta);
#endif

  *pos = pb_pos + delta;

  if (commit)
    {
      pb_pos = *pos;

      pb_pos_stamp.tv_sec = ts->tv_sec;
      pb_pos_stamp.tv_nsec = ts->tv_nsec;

      DPRINTF(E_DBG, L_PLAYER, "Pos: %" PRIu64 " (clock)\n", *pos);
    }

  return 0;
}

static int
player_get_current_pos_laudio(uint64_t *pos, struct timespec *ts, int commit)
{
  int ret;

  *pos = laudio_get_pos();

  ret = clock_gettime(CLOCK_MONOTONIC, ts);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Couldn't get clock: %s\n", strerror(errno));

      return -1;
    }

  if (commit)
    {
      pb_pos = *pos;

      pb_pos_stamp.tv_sec = ts->tv_sec;
      pb_pos_stamp.tv_nsec = ts->tv_nsec;

      DPRINTF(E_DBG, L_PLAYER, "Pos: %" PRIu64 " (laudio)\n", *pos);
    }

  return 0;
}

int
player_get_current_pos(uint64_t *pos, struct timespec *ts, int commit)
{
  switch (pb_sync_source)
    {
      case PLAYER_SYNC_CLOCK:
	return player_get_current_pos_clock(pos, ts, commit);

      case PLAYER_SYNC_LAUDIO:
	return player_get_current_pos_laudio(pos, ts, commit);
    }

  return -1;
}

/* Forward */
static int
playback_stop(void *arg);

static void
player_laudio_status_cb(enum laudio_state status)
{
  struct timespec ts;
  uint64_t pos;

  switch (status)
    {
      /* Switch sync to clock sync */
      case LAUDIO_STOPPING:
	DPRINTF(E_DBG, L_PLAYER, "Local audio stopping\n");

	laudio_status = status;

	/* Synchronize pb_pos and pb_pos_stamp before laudio stops entirely */
	player_get_current_pos_laudio(&pos, &ts, 1);

	pb_sync_source = PLAYER_SYNC_CLOCK;
	break;

      /* Switch sync to laudio sync */
      case LAUDIO_RUNNING:
	DPRINTF(E_DBG, L_PLAYER, "Local audio running\n");

	laudio_status = status;

	pb_sync_source = PLAYER_SYNC_LAUDIO;
	break;

      case LAUDIO_FAILED:
	DPRINTF(E_DBG, L_PLAYER, "Local audio failed\n");

	pb_sync_source = PLAYER_SYNC_CLOCK;

	laudio_close();

	if (raop_sessions == 0)
	  playback_stop(NULL);

	laudio_selected = 0;
	break;

      default:
      	laudio_status = status;
	break;
    }
}


/* Audio sources */
/* Thread: httpd (DACP) */
struct player_source *
player_queue_make(const char *query, const char *sort)
{
  struct query_params qp;
  struct db_media_file_info dbmfi;
  struct player_source *q_head;
  struct player_source *q_tail;
  struct player_source *ps;
  uint32_t id;
  int ret;

  memset(&qp, 0, sizeof(struct query_params));

  qp.type = Q_ITEMS;
  qp.offset = 0;
  qp.limit = 0;
  qp.idx_type = I_NONE;
  qp.sort = S_NONE;

  qp.filter = daap_query_parse_sql(query);
  if (!qp.filter)
    {
      DPRINTF(E_LOG, L_PLAYER, "Improper DAAP query!\n");

      return NULL;
    }

  if (sort)
    {
      if (strcmp(sort, "name") == 0)
	qp.sort = S_NAME;
      else if (strcmp(sort, "album") == 0)
	qp.sort = S_ALBUM;
    }

  ret = db_query_start(&qp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not start query\n");

      if (qp.filter)
	free(qp.filter);

      return NULL;
    }

  DPRINTF(E_DBG, L_PLAYER, "Player queue query returned %d items\n", qp.results);

  q_head = NULL;
  q_tail = NULL;
  while (((ret = db_query_fetch_file(&qp, &dbmfi)) == 0) && (dbmfi.id))
    {
      ret = safe_atou32(dbmfi.id, &id);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Invalid song id in query result!\n");

	  continue;
	}

      ps = (struct player_source *)malloc(sizeof(struct player_source));
      if (!ps)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Out of memory for struct player_source\n");

	  ret = -1;
	  break;
	}

      memset(ps, 0, sizeof(struct player_source));

      ps->id = id;

      if (!q_head)
	q_head = ps;

      if (q_tail)
	{
	  q_tail->pl_next = ps;
	  ps->pl_prev = q_tail;

	  q_tail->shuffle_next = ps;
	  ps->shuffle_prev = q_tail;
	}

      q_tail = ps;

      DPRINTF(E_DBG, L_PLAYER, "Added song id %d (%s)\n", id, dbmfi.title);
    }

  if (qp.filter)
    free(qp.filter);

  db_query_end(&qp);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Error fetching results\n");

      return NULL;
    }

  if (!q_head)
    return NULL;

  q_head->pl_prev = q_tail;
  q_tail->pl_next = q_head;

  return q_head;
}

static void
source_free(struct player_source *ps)
{
  if (ps->ctx)
    transcode_cleanup(ps->ctx);

  free(ps);
}

static void
source_stop(struct player_source *ps)
{
  struct player_source *tmp;

  while (ps)
    {
      if (ps->ctx)
	{
	  transcode_cleanup(ps->ctx);
	  ps->ctx = NULL;
	}

      tmp = ps;
      ps = ps->play_next;

      tmp->play_next = NULL;
    }
}

static struct player_source *
source_shuffle(struct player_source *head)
{
  struct player_source *ps;
  struct player_source **ps_array;
  int nitems;
  int i;

  if (!head)
    return NULL;

  ps = head;
  nitems = 0;
  do
    {
      nitems++;
      ps = ps->pl_next;
    }
  while (ps != head);

  ps_array = (struct player_source **)malloc(nitems * sizeof(struct player_source *));
  if (!ps_array)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not allocate memory for shuffle array\n");
      return NULL;
    }

  ps = head;
  i = 0;
  do
    {
      ps_array[i] = ps;

      ps = ps->pl_next;
      i++;
    }
  while (ps != head);

  shuffle_ptr(&shuffle_rng, (void **)ps_array, nitems);

  for (i = 0; i < nitems; i++)
    {
      ps = ps_array[i];

      if (i > 0)
	ps->shuffle_prev = ps_array[i - 1];

      if (i < (nitems - 1))
	ps->shuffle_next = ps_array[i + 1];
    }

  ps_array[0]->shuffle_prev = ps_array[nitems - 1];
  ps_array[nitems - 1]->shuffle_next = ps_array[0];

  ps = ps_array[0];

  free(ps_array);

  return ps;
}

static void
source_reshuffle(void)
{
  struct player_source *ps;

  ps = source_shuffle(source_head);
  if (!ps)
    return;

  if (cur_streaming)
    shuffle_head = cur_streaming;
  else
    shuffle_head = ps;
}

/* Helper */
static int
source_open(struct player_source *ps)
{
  struct media_file_info *mfi;

  ps->stream_start = 0;
  ps->output_start = 0;
  ps->end = 0;
  ps->play_next = NULL;

  mfi = db_file_fetch_byid(ps->id);
  if (!mfi)
    {
      DPRINTF(E_LOG, L_PLAYER, "Couldn't fetch file id %d\n", ps->id);

      return -1;
    }

  if (mfi->disabled)
    {
      DPRINTF(E_DBG, L_PLAYER, "File id %d is disabled, skipping\n", ps->id);

      free_mfi(mfi, 0);
      return -1;
    }

  DPRINTF(E_DBG, L_PLAYER, "Opening %s\n", mfi->path);

  ps->ctx = transcode_setup(mfi, NULL, 0);

  free_mfi(mfi, 0);

  if (!ps->ctx)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not open file id %d\n", ps->id);

      return -1;
    }

  return 0;
}

static int
source_next(int force)
{
  struct player_source *ps;
  struct player_source *head;
  struct player_source *limit;
  enum repeat_mode r_mode;
  int ret;

  head = (shuffle) ? shuffle_head : source_head;
  limit = head;
  r_mode = repeat;

  /* Force repeat mode at user request */
  if (force && (r_mode == REPEAT_SONG))
    r_mode = REPEAT_ALL;

  /* Playlist has only one file, treat REPEAT_ALL as REPEAT_SONG */
  if ((r_mode == REPEAT_ALL) && (source_head == source_head->pl_next))
    r_mode = REPEAT_SONG;
  /* Playlist has only one file, not a user action, treat as REPEAT_ALL
   * and source_check() will stop playback
   */
  else if (!force && (r_mode == REPEAT_OFF) && (source_head == source_head->pl_next))
    r_mode = REPEAT_SONG;

  if (!cur_streaming)
    ps = head;
  else
    ps = (shuffle) ? cur_streaming->shuffle_next : cur_streaming->pl_next;

  switch (r_mode)
    {
      case REPEAT_SONG:
	if (cur_streaming->ctx)
	  ret = transcode_seek(cur_streaming->ctx, 0);
	else
	  ret = source_open(cur_streaming);

	if (ret < 0)
	  {
	    DPRINTF(E_LOG, L_PLAYER, "Failed to restart song for song repeat\n");

	    return -1;
	  }

	return 0;

      case REPEAT_ALL:
	if (!shuffle)
	  {
	    limit = ps;
	    break;
	  }

	/* Reshuffle before repeating playlist */
	if (cur_streaming && (ps == shuffle_head))
	  {
	    source_reshuffle();
	    ps = shuffle_head;
	  }

	limit = shuffle_head;

	break;

      case REPEAT_OFF:
	limit = head;

	if (force && (ps == limit))
	  {
	    DPRINTF(E_DBG, L_PLAYER, "End of playlist reached and repeat is OFF\n");

	    playback_stop(NULL);
	    return 0;
	  }
	break;
    }

  do
    {
      ret = source_open(ps);
      if (ret < 0)
	{
	  if (shuffle)
	    ps = ps->shuffle_next;
	  else
	    ps = ps->pl_next;

	  continue;
	}

      break;
    }
  while (ps != limit);

  /* Couldn't open any of the files in our queue */
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_PLAYER, "Could not open any file in the queue (next)\n");

      return -1;
    }

  if (!force && cur_streaming)
    cur_streaming->play_next = ps;

  cur_streaming = ps;

  return 0;
}

static int
source_prev(void)
{
  struct player_source *ps;
  struct player_source *head;
  struct player_source *limit;
  int ret;

  if (!cur_streaming)
    return -1;

  head = (shuffle) ? shuffle_head : source_head;
  ps = (shuffle) ? cur_streaming->shuffle_prev : cur_streaming->pl_prev;
  limit = ps;

  if ((repeat == REPEAT_OFF) && (cur_streaming == head))
    {
      DPRINTF(E_DBG, L_PLAYER, "Start of playlist reached and repeat is OFF\n");

      playback_stop(NULL);
      return 0;
    }

  /* We are not reshuffling on prev calls in the shuffle case - should we? */

  do
    {
      ret = source_open(ps);
      if (ret < 0)
	{
	  if (shuffle)
	    ps = ps->shuffle_prev;
	  else
	    ps = ps->pl_prev;

	  continue;
	}

      break;
    }
  while (ps != limit);

  /* Couldn't open any of the files in our queue */
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_PLAYER, "Could not open any file in the queue (prev)\n");

      return -1;
    }

  cur_streaming = ps;

  return 0;
}

static int
source_position(struct player_source *ps)
{
  struct player_source *p;
  int ret;

  ret = 0;
  for (p = source_head; p != ps; p = p->pl_next)
    ret++;

  return ret;
}

static uint64_t
source_check(void)
{
  struct timespec ts;
  struct player_source *ps;
  struct player_source *head;
  uint64_t pos;
  enum repeat_mode r_mode;
  int i;
  int ret;

  if (!cur_streaming)
    return 0;

  ret = player_get_current_pos(&pos, &ts, 0);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Couldn't get current playback position\n");

      return 0;
    }

  if (!cur_playing)
    {
      if (pos >= cur_streaming->output_start)
	{
	  cur_playing = cur_streaming;
	  status_update(PLAY_PLAYING);
	}

      return pos;
    }

  if ((cur_playing->end == 0) || (pos < cur_playing->end))
    return pos;

  r_mode = repeat;
  /* Playlist has only one file, treat REPEAT_ALL as REPEAT_SONG */
  if ((r_mode == REPEAT_ALL) && (source_head == source_head->pl_next))
    r_mode = REPEAT_SONG;

  if (r_mode == REPEAT_SONG)
    {
      ps = cur_playing;

      /* Check that we haven't gone to the next file already
       * (repeat song toggled in the last 2 seconds of a song)
       */
      if (cur_playing->play_next)
	{
	  cur_playing = cur_playing->play_next;

	  if (ps->ctx)
	    {
	      transcode_cleanup(ps->ctx);
	      ps->ctx = NULL;
	      ps->play_next = NULL;
	    }
        }

      cur_playing->stream_start = ps->end + 1;
      cur_playing->output_start = cur_playing->stream_start;

      /* Do not use cur_playing to reset the end position, it may have changed */
      ps->end = 0;

      status_update(PLAY_PLAYING);

      return pos;
    }

  head = (shuffle) ? shuffle_head : source_head;

  i = 0;
  while (cur_playing && (cur_playing->end != 0) && (pos > cur_playing->end))
    {
      i++;

      /* Stop playback if:
       * - at end of playlist (NULL)
       * - repeat OFF and at end of playlist (wraparound)
       */
      if (!cur_playing->play_next
	  || ((r_mode == REPEAT_OFF) && (cur_playing->play_next == head)))
	{
	  playback_stop(NULL);

	  return pos;
        }

      ps = cur_playing;
      cur_playing = cur_playing->play_next;

      cur_playing->stream_start = ps->end + 1;
      cur_playing->output_start = cur_playing->stream_start;

      if (ps->ctx)
	{
	  transcode_cleanup(ps->ctx);
	  ps->ctx = NULL;
	  ps->play_next = NULL;
	}
    }

  if (i > 0)
    {
      DPRINTF(E_DBG, L_PLAYER, "Playback switched to next song\n");

      status_update(PLAY_PLAYING);
    }

  return pos;
}

static void
source_read(uint8_t *buf, int len, uint64_t rtptime)
{
  int new;
  int ret;
  int nbytes;

  if (!cur_streaming)
    return;

  nbytes = 0;
  new = 0;
  while (nbytes < len)
    {
      if (new)
	{
	  DPRINTF(E_DBG, L_PLAYER, "New file\n");

	  new = 0;

	  ret = source_next(0);
	  if (ret < 0)
	    return;
	}

      if (EVBUFFER_LENGTH(audio_buf) == 0)
	{
	  ret = transcode(cur_streaming->ctx, audio_buf, len - nbytes);
	  if (ret <= 0)
	    {
	      /* EOF or error */
	      cur_streaming->end = rtptime + BTOS(nbytes) - 1;

	      new = 1;
	      continue;
	    }
	}

      nbytes += evbuffer_remove(audio_buf, buf + nbytes, len - nbytes);
    }
}


static void
playback_write(void)
{
  uint8_t rawbuf[AIRTUNES_V2_PACKET_SAMPLES * 2 * 2];

  source_check();
  /* Make sure playback is still running after source_check() */
  if (player_state == PLAY_STOPPED)
    return;

  last_rtptime += AIRTUNES_V2_PACKET_SAMPLES;

  memset(rawbuf, 0, sizeof(rawbuf));

  source_read(rawbuf, sizeof(rawbuf), last_rtptime);

  if (laudio_status & LAUDIO_F_STARTED)
    laudio_write(rawbuf, last_rtptime);

  if (raop_sessions > 0)
    raop_v2_write(rawbuf, last_rtptime);
}

#if defined(__linux__)
static void
player_playback_cb(int fd, short what, void *arg)
{
  struct itimerspec next;
  uint64_t ticks;
  int ret;

  /* Acknowledge timer */
  read(fd, &ticks, sizeof(ticks));

  playback_write();

  /* Make sure playback is still running */
  if (player_state == PLAY_STOPPED)
    return;

  pb_timer_last.tv_nsec += AIRTUNES_V2_STREAM_PERIOD;
  if (pb_timer_last.tv_nsec >= 1000000000)
    {
      pb_timer_last.tv_sec++;
      pb_timer_last.tv_nsec -= 1000000000;
    }

  next.it_interval.tv_sec = 0;
  next.it_interval.tv_nsec = 0;
  next.it_value.tv_sec = pb_timer_last.tv_sec;
  next.it_value.tv_nsec = pb_timer_last.tv_nsec;

  ret = timerfd_settime(pb_timer_fd, TFD_TIMER_ABSTIME, &next, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not set playback timer: %s\n", strerror(errno));

      playback_stop(NULL);
      return;
    }

  ret = event_add(&pb_timer_ev, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not re-add playback timer event\n");

      playback_stop(NULL);
      return;
    }
}
#endif /* __linux__ */


#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
static void
player_playback_cb(int fd, short what, void *arg)
{
  struct timespec ts;
  struct kevent kev;
  int ret;

  ts.tv_sec = 0;
  ts.tv_nsec = 0;

  while (kevent(pb_timer_fd, NULL, 0, &kev, 1, &ts) > 0)
    {
      if (kev.filter != EVFILT_TIMER)
        continue;

      playback_write();

      /* Make sure playback is still running */
      if (player_state == PLAY_STOPPED)
	return;
    }

  ret = event_add(&pb_timer_ev, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not re-add playback timer event\n");

      playback_stop(NULL);
      return;
    }
}
#endif /* __FreeBSD__ || __FreeBSD_kernel__ */


static void
device_free(struct raop_device *dev)
{
  free(dev->name);
  free(dev->address);

  free(dev);
}

/* Helpers - call with dev_lck held */
static void
device_remove(struct raop_device *dev)
{
  struct raop_device *rd;
  struct raop_device *prev;

  prev = NULL;
  for (rd = dev_list; rd; rd = rd->next)
    {
      if (rd == dev)
	break;

      prev = rd;
    }

  if (!rd)
    return;

  DPRINTF(E_DBG, L_PLAYER, "Removing AirTunes device %s; stopped advertising\n", dev->name);

  if (!prev)
    dev_list = dev->next;
  else
    prev->next = dev->next;

  device_free(dev);
}

static int
device_check(struct raop_device *dev)
{
  struct raop_device *rd;

  for (rd = dev_list; rd; rd = rd->next)
    {
      if (rd == dev)
	break;
    }

  return (rd) ? 0 : -1;
}

/* RAOP callbacks executed in the player thread */
static void
device_streaming_cb(struct raop_device *dev, struct raop_session *rs, enum raop_session_state status)
{
  int ret;

  pthread_mutex_lock(&dev_lck);

  if (status == RAOP_FAILED)
    {
      raop_sessions--;

      ret = device_check(dev);
      if (ret < 0)
	{
	  pthread_mutex_unlock(&dev_lck);

	  DPRINTF(E_WARN, L_PLAYER, "AirTunes device disappeared during streaming!\n");

	  return;
	}

      DPRINTF(E_LOG, L_PLAYER, "AirTunes device %s FAILED\n", dev->name);

      if (player_state == PLAY_PLAYING)
	dev->selected = 0;

      dev->session = NULL;

      if (!dev->advertised)
	device_remove(dev);
    }
  else if (status == RAOP_STOPPED)
    {
      raop_sessions--;

      ret = device_check(dev);
      if (ret < 0)
	{
	  pthread_mutex_unlock(&dev_lck);

	  DPRINTF(E_WARN, L_PLAYER, "AirTunes device disappeared during streaming!\n");

	  return;
	}

      DPRINTF(E_LOG, L_PLAYER, "AirTunes device %s stopped\n", dev->name);

      dev->session = NULL;

      if (!dev->advertised)
	device_remove(dev);
    }

  pthread_mutex_unlock(&dev_lck);
}

static void
device_command_cb(struct raop_device *dev, struct raop_session *rs, enum raop_session_state status)
{
  cmd.raop_pending--;

  raop_set_status_cb(rs, device_streaming_cb);

  if (status == RAOP_FAILED)
    device_streaming_cb(dev, rs, status);

  if (cmd.raop_pending == 0)
    {
      if (cmd.func_bh)
	cmd.ret = cmd.func_bh(cmd.arg);
      else
	cmd.ret = 0;

      pthread_cond_signal(&cmd_cond);
      pthread_mutex_unlock(&cmd_lck);
    }
}

static void
device_shutdown_cb(struct raop_device *dev, struct raop_session *rs, enum raop_session_state status)
{
  int ret;

  cmd.raop_pending--;

  if (raop_sessions)
    raop_sessions--;

  pthread_mutex_lock(&dev_lck);

  ret = device_check(dev);
  if (ret < 0)
    {
      pthread_mutex_unlock(&dev_lck);

      DPRINTF(E_WARN, L_PLAYER, "AirTunes device disappeared before shutdown completion!\n");

      if (cmd.ret != -2)
	cmd.ret = -1;
      goto out;
    }

  dev->session = NULL;

  if (!dev->advertised)
    device_remove(dev);

  pthread_mutex_unlock(&dev_lck);

 out:
  if (cmd.raop_pending == 0)
    {
      /* cmd.ret already set
       *  - to 0 (or -2 if password issue) in speaker_set()
       *  - to -1 above on error
       */
      pthread_cond_signal(&cmd_cond);
      pthread_mutex_unlock(&cmd_lck);
    }
}

static void
device_lost_cb(struct raop_device *dev, struct raop_session *rs, enum raop_session_state status)
{
  /* We lost that device during startup for some reason, not much we can do here */
  if (status == RAOP_FAILED)
    DPRINTF(E_WARN, L_PLAYER, "Failed to stop lost device\n");
  else
    DPRINTF(E_INFO, L_PLAYER, "Lost device stopped properly\n");
}

static void
device_activate_cb(struct raop_device *dev, struct raop_session *rs, enum raop_session_state status)
{
  struct timespec ts;
  int ret;

  cmd.raop_pending--;

  pthread_mutex_lock(&dev_lck);

  ret = device_check(dev);
  if (ret < 0)
    {
      pthread_mutex_unlock(&dev_lck);

      DPRINTF(E_WARN, L_PLAYER, "AirTunes device disappeared during startup!\n");

      raop_set_status_cb(rs, device_lost_cb);
      raop_device_stop(rs);

      if (cmd.ret != -2)
	cmd.ret = -1;
      goto out;
    }

  if (status == RAOP_PASSWORD)
    {
      status = RAOP_FAILED;
      cmd.ret = -2;
    }

  if (status == RAOP_FAILED)
    {
      dev->selected = 0;

      if (!dev->advertised)
	device_remove(dev);

      pthread_mutex_unlock(&dev_lck);

      if (cmd.ret != -2)
	cmd.ret = -1;
      goto out;
    }

  dev->session = rs;

  pthread_mutex_unlock(&dev_lck);

  raop_sessions++;

  if ((player_state == PLAY_PLAYING) && (raop_sessions == 1))
    {
      ret = clock_gettime(CLOCK_MONOTONIC, &ts);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Could not get current time: %s\n", strerror(errno));

#if defined(__linux__)
	  /* Fallback to nearest timer expiration time */
	  ts.tv_sec = pb_timer_last.tv_sec;
	  ts.tv_nsec = pb_timer_last.tv_nsec;
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
	  if (cmd.ret != -2)
	    cmd.ret = -1;
	  goto out;
#endif
	}

      raop_playback_start(last_rtptime + AIRTUNES_V2_PACKET_SAMPLES, &ts);
    }

  raop_set_status_cb(rs, device_streaming_cb);

 out:
  if (cmd.raop_pending == 0)
    {
      /* cmd.ret already set
       *  - to 0 in speaker_set() (default)
       *  - to -2 above if password issue
       *  - to -1 above on error
       */
      pthread_cond_signal(&cmd_cond);
      pthread_mutex_unlock(&cmd_lck);
    }
}

static void
device_probe_cb(struct raop_device *dev, struct raop_session *rs, enum raop_session_state status)
{
  int ret;

  cmd.raop_pending--;

  pthread_mutex_lock(&dev_lck);

  ret = device_check(dev);
  if (ret < 0)
    {
      pthread_mutex_unlock(&dev_lck);

      DPRINTF(E_WARN, L_PLAYER, "AirTunes device disappeared during probe!\n");

      if (cmd.ret != -2)
	cmd.ret = -1;
      goto out;
    }

  if (status == RAOP_PASSWORD)
    {
      status = RAOP_FAILED;
      cmd.ret = -2;
    }

  if (status == RAOP_FAILED)
    {
      dev->selected = 0;

      if (!dev->advertised)
	device_remove(dev);

      pthread_mutex_unlock(&dev_lck);

      if (cmd.ret != -2)
	cmd.ret = -1;
      goto out;
    }

  pthread_mutex_unlock(&dev_lck);

 out:
  if (cmd.raop_pending == 0)
    {
      /* cmd.ret already set
       *  - to 0 in speaker_set() (default)
       *  - to -2 above if password issue
       *  - to -1 above on error
       */
      pthread_cond_signal(&cmd_cond);
      pthread_mutex_unlock(&cmd_lck);
    }
}

static void
device_restart_cb(struct raop_device *dev, struct raop_session *rs, enum raop_session_state status)
{
  int ret;

  cmd.raop_pending--;

  pthread_mutex_lock(&dev_lck);

  ret = device_check(dev);
  if (ret < 0)
    {
      pthread_mutex_unlock(&dev_lck);

      DPRINTF(E_WARN, L_PLAYER, "AirTunes device disappeared during restart!\n");

      raop_set_status_cb(rs, device_lost_cb);
      raop_device_stop(rs);

      goto out;
    }

  if (status == RAOP_FAILED)
    {
      dev->selected = 0;

      if (!dev->advertised)
	device_remove(dev);

      pthread_mutex_unlock(&dev_lck);

      goto out;
    }

  dev->session = rs;

  pthread_mutex_unlock(&dev_lck);

  raop_sessions++;
  raop_set_status_cb(rs, device_streaming_cb);

 out:
  if (cmd.raop_pending == 0)
    {
      cmd.ret = cmd.func_bh(cmd.arg);

      pthread_cond_signal(&cmd_cond);
      pthread_mutex_unlock(&cmd_lck);
    }
}


/* Actual commands, executed in the player thread */
static int
get_status(void *arg)
{
  struct timespec ts;
  struct player_source *ps;
  struct player_status *status;
  uint64_t pos;
  int ret;

  status = (struct player_status *)arg;

  status->shuffle = shuffle;
  status->repeat = repeat;

  status->volume = volume;

  switch (player_state)
    {
      case PLAY_STOPPED:
	DPRINTF(E_DBG, L_PLAYER, "Player status: stopped\n");

	status->status = PLAY_STOPPED;
	break;

      case PLAY_PAUSED:
	DPRINTF(E_DBG, L_PLAYER, "Player status: paused\n");

	status->status = PLAY_PAUSED;
	status->id = cur_streaming->id;

	pos = last_rtptime + AIRTUNES_V2_PACKET_SAMPLES - cur_streaming->stream_start;
	status->pos_ms = (pos * 1000) / 44100;

	status->pos_pl = source_position(cur_streaming);
	break;

      case PLAY_PLAYING:
	if (!cur_playing)
	  {
	    DPRINTF(E_DBG, L_PLAYER, "Player status: playing (buffering)\n");

	    status->status = PLAY_PAUSED;
	    ps = cur_streaming;

	    /* Avoid a visible 2-second jump backward for the client */
	    pos = ps->output_start - ps->stream_start;
	  }
	else
	  {
	    DPRINTF(E_DBG, L_PLAYER, "Player status: playing\n");

	    status->status = PLAY_PLAYING;
	    ps = cur_playing;

	    ret = player_get_current_pos(&pos, &ts, 0);
	    if (ret < 0)
	      {
		DPRINTF(E_LOG, L_PLAYER, "Could not get current stream position for playstatus\n");

		pos = 0;
	      }

	    if (pos < ps->stream_start)
	      pos = 0;
	    else
	      pos -= ps->stream_start;
	  }

	status->pos_ms = (pos * 1000) / 44100;

	status->id = ps->id;
	status->pos_pl = source_position(ps);
	break;
    }

  return 0;
}

static int
now_playing(void *arg)
{
  uint32_t *id;

  id = (uint32_t *)arg;

  if (cur_playing)
    *id = cur_playing->id;
  else if (cur_streaming)
    *id = cur_streaming->id;
  else
    return -1;

  return 0;
}

static int
playback_stop(void *arg)
{
  if (laudio_status != LAUDIO_CLOSED)
    laudio_close();

  if (raop_sessions > 0)
    raop_playback_stop();

  if (event_initialized(&pb_timer_ev))
    event_del(&pb_timer_ev);

  close(pb_timer_fd);
  pb_timer_fd = -1;

  if (cur_playing)
    source_stop(cur_playing);
  else
    source_stop(cur_streaming);

  cur_playing = NULL;
  cur_streaming = NULL;

  evbuffer_drain(audio_buf, EVBUFFER_LENGTH(audio_buf));

  status_update(PLAY_STOPPED);

  return 0;
}

/* Playback startup bottom half */
static int
playback_start_bh(void *arg)
{
#if defined(__linux__)
  struct itimerspec next;
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
  struct kevent kev;
#endif
  int ret;

  if ((laudio_status == LAUDIO_CLOSED) && (raop_sessions == 0))
    {
      DPRINTF(E_LOG, L_PLAYER, "Cannot start playback: no output started\n");

      goto out_fail;
    }

  /* Start laudio first as it can fail, but can be stopped easily if needed */
  if (laudio_status == LAUDIO_OPEN)
    {
      laudio_set_volume(volume);

      ret = laudio_start(pb_pos, last_rtptime + AIRTUNES_V2_PACKET_SAMPLES);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Local audio failed to start\n");

	  goto out_fail;
	}
    }

  ret = clock_gettime(CLOCK_MONOTONIC, &pb_pos_stamp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Couldn't get current clock: %s\n", strerror(errno));

      goto out_fail;
    }

  memset(&pb_timer_ev, 0, sizeof(struct event));

#if defined(__linux__)
  pb_timer_last.tv_sec = pb_pos_stamp.tv_sec;
  pb_timer_last.tv_nsec = pb_pos_stamp.tv_nsec;

  pb_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  if (pb_timer_fd < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not create playback timer: %s\n", strerror(errno));

      goto out_fail;
    }

  next.it_interval.tv_sec = 0;
  next.it_interval.tv_nsec = 0;
  next.it_value.tv_sec = pb_timer_last.tv_sec;
  next.it_value.tv_nsec = pb_timer_last.tv_nsec;

  ret = timerfd_settime(pb_timer_fd, TFD_TIMER_ABSTIME, &next, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not set playback timer: %s\n", strerror(errno));

      goto out_fail;
    }
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
  pb_timer_fd = kqueue();
  if (pb_timer_fd < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not create kqueue: %s\n", strerror(errno));

      goto out_fail;
    }

  memset(&kev, 0, sizeof(struct kevent));

  EV_SET(&kev, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, AIRTUNES_V2_STREAM_PERIOD, 0);

  ret = kevent(pb_timer_fd, &kev, 1, NULL, 0, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not add kevent timer: %s\n", strerror(errno));

      goto out_fail;
    }
#endif

  event_set(&pb_timer_ev, pb_timer_fd, EV_READ, player_playback_cb, NULL);
  event_base_set(evbase_player, &pb_timer_ev);

  ret = event_add(&pb_timer_ev, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not set up playback timer event\n");

      goto out_fail;
    }

  /* Everything OK, start RAOP */
  if (raop_sessions > 0)
    raop_playback_start(last_rtptime + AIRTUNES_V2_PACKET_SAMPLES, &pb_pos_stamp);

  status_update(PLAY_PLAYING);

  return 0;

 out_fail:
  close(pb_timer_fd);
  pb_timer_fd = -1;
  playback_stop(NULL);

  return -1;
}

static int
playback_start(void *arg)
{
  struct raop_device *rd;
  uint32_t *idx_id;
  int ret;

  if (!source_head)
    {
      DPRINTF(E_LOG, L_PLAYER, "Nothing to play!\n");

      return -1;
    }

  idx_id = (uint32_t *)arg;

  if (player_state == PLAY_PLAYING)
    {
      if (idx_id)
	{
	  if (cur_playing)
	    *idx_id = cur_playing->id;
	  else
	    *idx_id = cur_streaming->id;
	}

      status_update(player_state);

      return 0;
    }

  pb_pos = last_rtptime + AIRTUNES_V2_PACKET_SAMPLES - 88200;

  if (idx_id)
    {
      if (cur_playing)
	source_stop(cur_playing);
      else if (cur_streaming)
	source_stop(cur_streaming);

      cur_playing = NULL;
      cur_streaming = NULL;

      if (shuffle)
	{
	  source_reshuffle();
	  cur_streaming = shuffle_head;
	}
      else
	cur_streaming = source_head;

      if (*idx_id > 0)
	{
	  cur_streaming = source_head;
	  for (; *idx_id > 0; (*idx_id)--)
	    cur_streaming = cur_streaming->pl_next;

	  if (shuffle)
	    shuffle_head = cur_streaming;
	}

      ret = source_open(cur_streaming);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Couldn't jump to queue position %d\n", *idx_id);

	  return -1;
	}

      *idx_id = cur_streaming->id;
      cur_streaming->stream_start = last_rtptime + AIRTUNES_V2_PACKET_SAMPLES;
      cur_streaming->output_start = cur_streaming->stream_start;
    }
  else if (!cur_streaming)
    {
      if (shuffle)
	source_reshuffle();

      ret = source_next(0);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Couldn't find anything to play!\n");

	  return -1;
	}

      cur_streaming->stream_start = last_rtptime + AIRTUNES_V2_PACKET_SAMPLES;
      cur_streaming->output_start = cur_streaming->stream_start;
    }

  /* Start local audio if needed */
  if (laudio_selected && (laudio_status == LAUDIO_CLOSED))
    {
      ret = laudio_open();
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Could not open local audio\n");

	  return -1;
	}
    }

  /* Start RAOP sessions on selected devices if needed */
  cmd.raop_pending = 0;

  pthread_mutex_lock(&dev_lck);

  for (rd = dev_list; rd; rd = rd->next)
    {
      if (rd->selected && !rd->session)
	{
	  ret = raop_device_start(rd, device_restart_cb, last_rtptime + AIRTUNES_V2_PACKET_SAMPLES);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_PLAYER, "Could not start selected AirTunes device %s\n", rd->name);
	      continue;
	    }

	  cmd.raop_pending++;
	}
    }

  pthread_mutex_unlock(&dev_lck);

  if ((laudio_status == LAUDIO_CLOSED) && (cmd.raop_pending == 0) && (raop_sessions == 0))
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not start playback: no output selected or couldn't start any output\n");

      return -1;
    }

  /* We're async if we need to start RAOP devices */
  if (cmd.raop_pending > 0)
    return 1; /* async */

  /* Otherwise, just run the bottom half */
  return playback_start_bh(arg);
}

static int
playback_prev_bh(void *arg)
{
  int ret;

  if (cur_playing)
    source_stop(cur_playing);
  else
    source_stop(cur_streaming);

  ret = source_prev();
  if (ret < 0)
    {
      playback_stop(NULL);

      return -1;
    }

  if (player_state == PLAY_STOPPED)
    return -1;

  cur_streaming->stream_start = last_rtptime + AIRTUNES_V2_PACKET_SAMPLES;
  cur_streaming->output_start = cur_streaming->stream_start;

  cur_playing = NULL;

  /* Silent status change - playback_start() sends the real status update */
  player_state = PLAY_PAUSED;

  return 0;
}

static int
playback_next_bh(void *arg)
{
  int ret;

  if (cur_playing)
    source_stop(cur_playing);
  else
    source_stop(cur_streaming);

  ret = source_next(1);
  if (ret < 0)
    {
      playback_stop(NULL);

      return -1;
    }

  if (player_state == PLAY_STOPPED)
    return -1;

  cur_streaming->stream_start = last_rtptime + AIRTUNES_V2_PACKET_SAMPLES;
  cur_streaming->output_start = cur_streaming->stream_start;

  cur_playing = NULL;

  /* Silent status change - playback_start() sends the real status update */
  player_state = PLAY_PAUSED;

  return 0;
}

static int
playback_seek_bh(void *arg)
{
  struct player_source *ps;
  int ms;
  int ret;

  ms = *(int *)arg;

  if (cur_playing)
    ps = cur_playing;
  else
    ps = cur_streaming;

  ps->end = 0;

  /* Seek to commanded position */
  ret = transcode_seek(ps->ctx, ms);
  if (ret < 0)
    {
      playback_stop(NULL);

      return -1;
    }

  /* Adjust start_pos for the new position */
  ps->stream_start = last_rtptime + AIRTUNES_V2_PACKET_SAMPLES - ((uint64_t)ret * 44100) / 1000;
  ps->output_start = last_rtptime + AIRTUNES_V2_PACKET_SAMPLES;

  cur_streaming = ps;
  cur_playing = NULL;

  /* Silent status change - playback_start() sends the real status update */
  player_state = PLAY_PAUSED;

  return 0;
}

static int
playback_pause_bh(void *arg)
{
  struct player_source *ps;
  uint64_t pos;
  int ms;
  int ret;

  if (cur_playing)
    ps = cur_playing;
  else
    ps = cur_streaming;

  pos = ps->end;
  ps->end = 0;

  /* Seek back to current playback position */
  pos -= ps->stream_start;
  ms = (int)((pos * 1000) / 44100);

  ret = transcode_seek(ps->ctx, ms);
  if (ret < 0)
    {
      playback_stop(NULL);

      return -1;
    }

  /* Adjust start_pos to take into account the pause and seek back */
  ps->stream_start = last_rtptime + AIRTUNES_V2_PACKET_SAMPLES - ((uint64_t)ret * 44100) / 1000;
  ps->output_start = last_rtptime + AIRTUNES_V2_PACKET_SAMPLES;

  cur_streaming = ps;
  cur_playing = NULL;

  status_update(PLAY_PAUSED);

  return 0;
}

static int
playback_pause(void *arg)
{
  struct player_source *ps;
  uint64_t pos;

  pos = source_check();
  if (pos == 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not retrieve current position for pause\n");

      return playback_stop(arg);
    }

  /* Make sure playback is still running after source_check() */
  if (player_state == PLAY_STOPPED)
    return -1;

  if (cur_playing)
    ps = cur_playing;
  else
    ps = cur_streaming;

  /* Store pause position */
  ps->end = pos;

  cmd.raop_pending = raop_flush(device_command_cb, last_rtptime + AIRTUNES_V2_PACKET_SAMPLES);

  if (laudio_status != LAUDIO_CLOSED)
    laudio_stop();

  if (event_initialized(&pb_timer_ev))
    event_del(&pb_timer_ev);

  close(pb_timer_fd);
  pb_timer_fd = -1;

  if (ps->play_next)
    source_stop(ps->play_next);

  cur_playing = NULL;
  cur_streaming = ps;
  cur_streaming->play_next = NULL;

  evbuffer_drain(audio_buf, EVBUFFER_LENGTH(audio_buf));

  /* We're async if we need to flush RAOP devices */
  if (cmd.raop_pending > 0)
    return 1; /* async */

  /* Otherwise, just run the bottom half */
  return cmd.func_bh(arg);
}


static int
speaker_activate(struct raop_device *rd)
{
  struct timespec ts;
  uint64_t pos;
  int ret;

  if (!rd)
    {
      /* Local */
      DPRINTF(E_DBG, L_PLAYER, "Activating local audio\n");

      if (laudio_status == LAUDIO_CLOSED)
	{
	  ret = laudio_open();
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_PLAYER, "Could not open local audio\n");

	      return -1;
	    }
	}

      if (player_state == PLAY_PLAYING)
	{
	  laudio_set_volume(volume);

	  ret = player_get_current_pos(&pos, &ts, 0);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_PLAYER, "Could not get current stream position for local audio start\n");

	      laudio_close();
	      return -1;
	    }

	  ret = laudio_start(pos, last_rtptime + AIRTUNES_V2_PACKET_SAMPLES);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_PLAYER, "Local playback failed to start\n");

	      laudio_close();
	      return -1;
	    }
	}

      return 0;
    }
  else
    {
      /* RAOP */
      if (player_state == PLAY_PLAYING)
	{
	  DPRINTF(E_DBG, L_PLAYER, "Activating RAOP device %s\n", rd->name);

	  ret = raop_device_start(rd, device_activate_cb, last_rtptime + AIRTUNES_V2_PACKET_SAMPLES);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_PLAYER, "Could not start device %s\n", rd->name);

	      return -1;
	    }
	}
      else
	{
	  DPRINTF(E_DBG, L_PLAYER, "Probing RAOP device %s\n", rd->name);

	  ret = raop_device_probe(rd, device_probe_cb);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_PLAYER, "Could not probe device %s\n", rd->name);

	      return -1;
	    }
	}

      cmd.raop_pending++;

      return 0;
    }

  return -1;
}

static int
speaker_deactivate(struct raop_device *rd)
{
  if (!rd)
    {
      /* Local */
      DPRINTF(E_DBG, L_PLAYER, "Deactivating local audio\n");

      if (laudio_status == LAUDIO_CLOSED)
	return 0;

      if (laudio_status & LAUDIO_F_STARTED)
	laudio_stop();

      laudio_close();

      return 0;
    }
  else
    {
      /* RAOP */
      DPRINTF(E_DBG, L_PLAYER, "Deactivating RAOP device %s\n", rd->name);

      raop_set_status_cb(rd->session, device_shutdown_cb);
      raop_device_stop(rd->session);

      cmd.raop_pending++;
      return 0;
    }

  return -1;
}

static int
speaker_set(void *arg)
{
  struct raop_device *rd;
  uint64_t *ids;
  int nspk;
  int i;
  int ret;

  ids = (uint64_t *)arg;

  if (ids)
    nspk = ids[0];
  else
    nspk = 0;

  DPRINTF(E_DBG, L_PLAYER, "Speaker set: %d speakers\n", nspk);

  cmd.raop_pending = 0;
  cmd.ret = 0;

  pthread_mutex_lock(&dev_lck);

  /* RAOP devices */
  for (rd = dev_list; rd; rd = rd->next)
    {
      for (i = 1; i <= nspk; i++)
	{
	  DPRINTF(E_DBG, L_PLAYER, "Set %" PRIu64 " device %" PRIu64 "\n", ids[i], rd->id);

	  if (ids[i] == rd->id)
	    break;
	}

      if (i <= nspk)
	{
	  if (rd->has_password && !rd->password)
	    {
	      DPRINTF(E_INFO, L_PLAYER, "RAOP device %s is password-protected, but we don't have it\n", rd->name);

	      cmd.ret = -2;
	      continue;
	    }

	  DPRINTF(E_DBG, L_PLAYER, "RAOP device %s selected\n", rd->name);
	  rd->selected = 1;

	  if (!rd->session)
	    {
	      ret = speaker_activate(rd);
	      if (ret < 0)
		{
		  DPRINTF(E_LOG, L_PLAYER, "Could not activate RAOP device %s\n", rd->name);

		  rd->selected = 0;

		  if (cmd.ret != -2)
		    cmd.ret = -1;
		}
	    }
	}
      else
	{
	  DPRINTF(E_DBG, L_PLAYER, "RAOP device %s NOT selected\n", rd->name);
	  rd->selected = 0;

	  if (rd->session)
	    {
	      ret = speaker_deactivate(rd);
	      if (ret < 0)
		{
		  DPRINTF(E_LOG, L_PLAYER, "Could not deactivate RAOP device %s\n", rd->name);

		  if (cmd.ret != -2)
		    cmd.ret = -1;
		}
	    }
	}
    }

  pthread_mutex_unlock(&dev_lck);

  /* Local audio */
  for (i = 1; i <= nspk; i++)
    {
      if (ids[i] == 0)
	break;
    }

  if (i <= nspk)
    {
      DPRINTF(E_DBG, L_PLAYER, "Local audio selected\n");
      laudio_selected = 1;

      if (!(laudio_status & LAUDIO_F_STARTED))
	{
	  ret = speaker_activate(NULL);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_PLAYER, "Could not activate local audio output\n");

	      laudio_selected = 0;

	      if (cmd.ret != -2)
		cmd.ret = -1;
	    }
	}
    }
  else
    {
      DPRINTF(E_DBG, L_PLAYER, "Local audio NOT selected\n");
      laudio_selected = 0;

      if (laudio_status != LAUDIO_CLOSED)
	{
	  ret = speaker_deactivate(NULL);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_PLAYER, "Could not deactivate local audio output\n");

	      if (cmd.ret != -2)
		cmd.ret = -1;
	    }
	}
    }

  if (cmd.raop_pending > 0)
    return 1; /* async */

  return cmd.ret;
}

static int
volume_set(void *arg)
{
  int vol;
  int ret;

  vol = *(int *)arg;

  volume = vol;

  cmd.raop_pending = raop_set_volume(volume, device_command_cb);
  laudio_set_volume(volume);

  ret = db_config_save_int(VAR_PLAYER_VOLUME, volume);
  if (ret < 0)
    DPRINTF(E_WARN, L_PLAYER, "Could not save volume setting to DB\n");

  if (cmd.raop_pending > 0)
    return 1; /* async */

  return 0;
}

static int
repeat_set(void *arg)
{
  enum repeat_mode *mode;

  mode = (enum repeat_mode *)arg;

  switch (*mode)
    {
      case REPEAT_OFF:
      case REPEAT_SONG:
      case REPEAT_ALL:
	repeat = *mode;
	break;

      default:
	DPRINTF(E_LOG, L_PLAYER, "Invalid repeat mode: %d\n", *mode);
	return -1;
    }

  return 0;
}

static int
shuffle_set(void *arg)
{
  int *enable;

  enable = (int *)arg;

  switch (*enable)
    {
      case 1:
	if (!shuffle)
	  source_reshuffle();
	/* FALLTHROUGH*/
      case 0:
	shuffle = *enable;
	break;

      default:
	DPRINTF(E_LOG, L_PLAYER, "Invalid shuffle mode: %d\n", *enable);
	return -1;
    }

  return 0;
}

static int
queue_add(void *arg)
{
  struct player_source *ps;
  struct player_source *ps_shuffle;
  struct player_source *source_tail;
  struct player_source *ps_tail;

  ps = (struct player_source *)arg;

  ps_shuffle = source_shuffle(ps);
  if (!ps_shuffle)
    ps_shuffle = ps;

  if (source_head)
    {
      /* Playlist order */
      source_tail = source_head->pl_prev;
      ps_tail = ps->pl_prev;

      source_tail->pl_next = ps;
      ps_tail->pl_next = source_head;

      source_head->pl_prev = ps_tail;
      ps->pl_prev = source_tail;

      /* Shuffle */
      source_tail = shuffle_head->shuffle_prev;
      ps_tail = ps_shuffle->shuffle_prev;

      source_tail->shuffle_next = ps_shuffle;
      ps_tail->shuffle_next = shuffle_head;

      shuffle_head->shuffle_prev = ps_tail;
      ps_shuffle->shuffle_prev = source_tail;
    }
  else
    {
      source_head = ps;
      shuffle_head = ps_shuffle;
    }

  return 0;
}

static int
queue_clear(void *arg)
{
  struct player_source *ps;

  if (!source_head)
    return 0;

  shuffle_head = NULL;
  source_head->pl_prev->pl_next = NULL;

  for (ps = source_head; ps; ps = source_head)
    {
      source_head = ps->pl_next;

      source_free(ps);
    }

  return 0;
}


/* Command helpers */
/* Thread: player */
static void
command_cb(int fd, short what, void *arg)
{
  int ret;

#ifdef USE_EVENTFD
  eventfd_t count;

  ret = eventfd_read(cmd_efd, &count);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not read event counter: %s\n", strerror(errno));

      goto readd;
    }
#else
  int dummy;

  read(cmd_pipe[0], &dummy, sizeof(dummy));
#endif

  pthread_mutex_lock(&cmd_lck);

  ret = cmd.func(cmd.arg);

  if (ret <= 0)
    {
      cmd.ret = ret;

      pthread_cond_signal(&cmd_cond);
      pthread_mutex_unlock(&cmd_lck);
    }

  /* ret > 0 means command is asynchronous and will unlock and signal
   * by itself upon completion
   */

#ifdef USE_EVENTFD
 readd:
#endif
  event_add(&cmdev, NULL);
}


/* Thread: httpd (DACP) */
/* Must be called with cmd_lck held */
static int
sync_command(void)
{
#ifndef USE_EVENTFD
  int dummy = 42;
#endif
  int ret;

  if (!cmd.func)
    {
      DPRINTF(E_LOG, L_PLAYER, "BUG: cmd.func is NULL!\n");

      return -1;
    }

#ifdef USE_EVENTFD
  ret = eventfd_write(cmd_efd, 1);
  if (ret < 0)
#else
  ret = write(cmd_pipe[1], &dummy, sizeof(dummy));
  if (ret != sizeof(dummy))
#endif
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not send command event: %s\n", strerror(errno));

      return -1;
    }

  pthread_cond_wait(&cmd_cond, &cmd_lck);

  ret = cmd.ret;
  cmd.func = NULL;
  cmd.func_bh = NULL;
  cmd.arg = NULL;

  return ret;
}


/* Player API executed in the httpd (DACP) thread */
int
player_get_status(struct player_status *status)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = get_status;
  cmd.func_bh = NULL;
  cmd.arg = status;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

int
player_now_playing(uint32_t *id)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = now_playing;
  cmd.func_bh = NULL;
  cmd.arg = id;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

int
player_playback_start(uint32_t *idx_id)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = playback_start;
  cmd.func_bh = playback_start_bh;
  cmd.arg = idx_id;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

int
player_playback_stop(void)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = playback_stop;
  cmd.arg = NULL;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

int
player_playback_pause(void)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = playback_pause;
  cmd.func_bh = playback_pause_bh;
  cmd.arg = NULL;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

int
player_playback_seek(int ms)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = playback_pause;
  cmd.func_bh = playback_seek_bh;
  cmd.arg = &ms;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

int
player_playback_next(void)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = playback_pause;
  cmd.func_bh = playback_next_bh;
  cmd.arg = NULL;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

int
player_playback_prev(void)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = playback_pause;
  cmd.func_bh = playback_prev_bh;
  cmd.arg = NULL;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

void
player_speaker_enumerate(spk_enum_cb cb, void *arg)
{
  struct raop_device *rd;
  char *laudio_name;

  laudio_name = cfg_getstr(cfg_getsec(cfg, "audio"), "nickname");

  pthread_mutex_lock(&dev_lck);

  /* Auto-select local audio if there are no AirTunes devices */
  if (!dev_list)
    laudio_selected = 1;

  cb(0, laudio_name, laudio_selected, 0, arg);

  for (rd = dev_list; rd; rd = rd->next)
    {
      if (rd->advertised)
	cb(rd->id, rd->name, rd->selected, rd->has_password, arg);
    }

  pthread_mutex_unlock(&dev_lck);
}

int
player_speaker_set(uint64_t *ids)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = speaker_set;
  cmd.func_bh = NULL;
  cmd.arg = ids;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

int
player_volume_set(int vol)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = volume_set;
  cmd.func_bh = NULL;
  cmd.arg = &vol;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

int
player_repeat_set(enum repeat_mode mode)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = repeat_set;
  cmd.func_bh = NULL;
  cmd.arg = &mode;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

int
player_shuffle_set(int enable)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = shuffle_set;
  cmd.func_bh = NULL;
  cmd.arg = &enable;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

int
player_queue_add(struct player_source *ps)
{
  int ret;

  pthread_mutex_lock(&cmd_lck);

  cmd.func = queue_add;
  cmd.func_bh = NULL;
  cmd.arg = ps;

  ret = sync_command();

  pthread_mutex_unlock(&cmd_lck);

  return ret;
}

void
player_queue_clear(void)
{
  pthread_mutex_lock(&cmd_lck);

  cmd.func = queue_clear;
  cmd.func_bh = NULL;
  cmd.arg = NULL;

  sync_command();

  pthread_mutex_unlock(&cmd_lck);
}


/* Thread: main (mdns) */
static void
raop_device_cb(const char *name, const char *type, const char *domain, const char *hostname, int family, const char *address, int port, AvahiStringList *txt)
{
  AvahiStringList *p;
  struct raop_device *rd;
  struct raop_device *prev;
  cfg_t *apex;
  char *at_name;
  char *key;
  char *val;
  uint64_t id;
  size_t valsz;
  int has_password;
  int ret;

  if (family != AF_INET)
    return;

  ret = safe_hextou64(name, &id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not extract AirTunes device ID (%s)\n", name);

      return;
    }

  at_name = strchr(name, '@');
  if (!at_name)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not extract AirTunes device name (%s)\n", name);

      return;
    }
  at_name++;

  DPRINTF(E_DBG, L_PLAYER, "Found AirTunes device %" PRIx64 "/%s (%d)\n", id, at_name, port);

  if (port < 0)
    {
      /* Device stopped advertising */
      pthread_mutex_lock(&dev_lck);

      prev = NULL;
      for (rd = dev_list; rd; rd = rd->next)
	{
	  if (rd->id == id)
	    break;

	  prev = rd;
	}

      if (!rd)
	{
	  pthread_mutex_unlock(&dev_lck);

	  DPRINTF(E_WARN, L_PLAYER, "AirTunes device %s stopped advertising, but not in our list\n", name);

	  return;
	}

      rd->advertised = 0;

      if (!rd->session)
	{
	  if (!prev)
	    dev_list = rd->next;
	  else
	    prev->next = rd->next;

	  device_free(rd);

	  DPRINTF(E_DBG, L_PLAYER, "Removed AirTunes device %s; stopped advertising\n", name);
	}

      pthread_mutex_unlock(&dev_lck);
    }
  else
    {
      p = avahi_string_list_find(txt, "pw");
      if (!p)
	{
	  DPRINTF(E_LOG, L_PLAYER, "AirTunes %s: no pw field in TXT record!\n", name);

	  return;
	}

      avahi_string_list_get_pair(p, &key, &val, &valsz);
      avahi_free(key);
      if (!val)
        {
          DPRINTF(E_LOG, L_PLAYER, "AirTunes %s: pw has no value\n", name);

          return;
        }

      has_password = (strcmp(val, "false") != 0);

      avahi_free(val);
      val = NULL;

      if (has_password)
	{
	  DPRINTF(E_LOG, L_PLAYER, "AirTunes device %s is password-protected\n", name);

	  apex = cfg_gettsec(cfg, "apex", at_name);
	  if (apex)
	    val = cfg_getstr(apex, "password");

	  if (!val)
	    DPRINTF(E_LOG, L_PLAYER, "No password given in config for AirTunes device %s\n", name);
	}

      pthread_mutex_lock(&dev_lck);

      for (rd = dev_list; rd; rd = rd->next)
	{
	  if (rd->id == id)
	    break;
	}

      if (rd)
	{
	  DPRINTF(E_DBG, L_PLAYER, "Updating AirTunes device %s already in list, updating\n", name);

	  free(rd->name);
	  free(rd->address);
	}
      else
	{
	  DPRINTF(E_DBG, L_PLAYER, "Adding AirTunes device %s (password: %s)\n", name, (val) ? "yes" : "no");

	  rd = (struct raop_device *)malloc(sizeof(struct raop_device));
	  if (!rd)
	    {
	      pthread_mutex_unlock(&dev_lck);

	      DPRINTF(E_LOG, L_PLAYER, "Out of memory for new AirTunes device\n");

	      return;
	    }

	  memset(rd, 0, sizeof(struct raop_device));

	  rd->id = id;

	  rd->next = dev_list;
	  dev_list = rd;
	}

      rd->advertised = 1;
      rd->port = port;

      rd->name = strdup(at_name);
      rd->address = strdup(address);

      rd->has_password = has_password;
      rd->password = val;

      pthread_mutex_unlock(&dev_lck);
    }
}

/* Thread: player */
static void *
player(void *arg)
{
  int ret;

  ret = db_perthread_init();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Error: DB init failed\n");

      pthread_exit(NULL);
    }

  event_base_dispatch(evbase_player);

  if (!player_exit)
    DPRINTF(E_LOG, L_PLAYER, "Player event loop terminated ahead of time!\n");

  db_perthread_deinit();

  pthread_exit(NULL);
}

/* Thread: player */
static void
exit_cb(int fd, short what, void *arg)
{
  event_base_loopbreak(evbase_player);

  player_exit = 1;
}

/* Thread: main at DACP init/deinit */
void
player_set_updatefd(int fd)
{
  update_fd = fd;
}

/* Thread: main */
int
player_init(void)
{
  uint32_t rnd;
  int ret;

  player_exit = 0;

  dev_list = NULL;

  laudio_status = LAUDIO_CLOSED;
  raop_sessions = 0;

  cmd.func = NULL;

  pb_timer_fd = -1;

  source_head = NULL;
  shuffle_head = NULL;
  cur_playing = NULL;
  cur_streaming = NULL;

  player_state = PLAY_STOPPED;
  repeat = REPEAT_OFF;
  shuffle = 0;

  update_fd = -1;

  /* Random RTP time start */
  gcry_randomize(&rnd, sizeof(rnd), GCRY_STRONG_RANDOM);
  last_rtptime = ((uint64_t)1 << 32) | rnd;

  rng_init(&shuffle_rng);

  ret = db_config_fetch_int(VAR_PLAYER_VOLUME, &volume);
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_PLAYER, "Could not fetch last volume setting from DB\n");

      volume = 75;
    }

  audio_buf = evbuffer_new();
  if (!audio_buf)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not allocate evbuffer for audio buffer\n");

      return -1;
    }


#ifdef USE_EVENTFD
  exit_efd = eventfd(0, EFD_CLOEXEC);
  if (exit_efd < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not create eventfd: %s\n", strerror(errno));

      goto exit_fail;
    }
#else
# if defined(__linux__)
  ret = pipe2(exit_pipe, O_CLOEXEC);
# else
  ret = pipe(exit_pipe);
# endif
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not create pipe: %s\n", strerror(errno));

      goto exit_fail;
    }
#endif /* USE_EVENTFD */

#ifdef USE_EVENTFD
  cmd_efd = eventfd(0, EFD_CLOEXEC);
  if (cmd_efd < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not create command eventfd: %s\n", strerror(errno));

      goto cmd_fail;
    }
#else
# if defined(__linux__)
  ret = pipe2(cmd_pipe, O_CLOEXEC);
# else
  ret = pipe(cmd_pipe);
# endif
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not create command pipe: %s\n", strerror(errno));

      goto cmd_fail;
    }
#endif /* USE_EVENTFD */

  evbase_player = event_base_new();
  if (!evbase_player)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not create an event base\n");

      goto evbase_fail;
    }

#ifdef USE_EVENTFD
  event_set(&exitev, exit_efd, EV_READ, exit_cb, NULL);
#else
  event_set(&exitev, exit_pipe[0], EV_READ, exit_cb, NULL);
#endif
  event_base_set(evbase_player, &exitev);
  event_add(&exitev, NULL);

#ifdef USE_EVENTFD
  event_set(&cmdev, cmd_efd, EV_READ, command_cb, NULL);
#else
  event_set(&cmdev, cmd_pipe[0], EV_READ, command_cb, NULL);
#endif
  event_base_set(evbase_player, &cmdev);
  event_add(&cmdev, NULL);

  ret = laudio_init(player_laudio_status_cb);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Local audio init failed\n");

      goto laudio_fail;
    }

  ret = raop_init();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "RAOP init failed\n");

      goto raop_fail;
    }

  raop_set_volume(volume, NULL);

  ret = mdns_browse("_raop._tcp", raop_device_cb);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_PLAYER, "Could not browser for AirTunes devices\n");

      goto mdns_browse_fail;
    }

  ret = pthread_create(&tid_player, NULL, player, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not spawn player thread: %s\n", strerror(errno));

      goto thread_fail;
    }

  return 0;

 thread_fail:
 mdns_browse_fail:
  raop_deinit();
 raop_fail:
  laudio_deinit();
 laudio_fail:
  event_base_free(evbase_player);
 evbase_fail:
#ifdef USE_EVENTFD
  close(cmd_efd);
#else
  close(cmd_pipe[0]);
  close(cmd_pipe[1]);
#endif
 cmd_fail:
#ifdef USE_EVENTFD
  close(exit_efd);
#else
  close(exit_pipe[0]);
  close(exit_pipe[1]);
#endif
 exit_fail:
  evbuffer_free(audio_buf);

  return -1;
}

/* Thread: main */
void
player_deinit(void)
{
  int ret;

#ifdef USE_EVENTFD
  ret = eventfd_write(exit_efd, 1);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not send exit event: %s\n", strerror(errno));

      return;
    }
#else
  int dummy = 42;

  ret = write(exit_pipe[1], &dummy, sizeof(dummy));
  if (ret != sizeof(dummy))
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not write to exit fd: %s\n", strerror(errno));

      return;
    }
#endif

  ret = pthread_join(tid_player, NULL);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not join HTTPd thread: %s\n", strerror(errno));

      return;
    }

  if (source_head)
    queue_clear(NULL);

  evbuffer_free(audio_buf);

  laudio_deinit();
  raop_deinit();

#ifdef USE_EVENTFD
  close(exit_efd);
  close(cmd_efd);
  cmd_efd = -1;
#else
  close(exit_pipe[0]);
  close(exit_pipe[1]);
  close(cmd_pipe[0]);
  close(cmd_pipe[1]);
  cmd_pipe[0] = -1;
  cmd_pipe[1] = -1;
#endif
  event_base_free(evbase_player);
}