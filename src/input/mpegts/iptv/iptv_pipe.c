/*
 *  IPTV - pipe handler
 *
 *  Copyright (C) 2014 Jaroslav Kysela
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvheadend.h"
#include "iptv_private.h"
#include "spawn.h"
#include "tvhpoll.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <assert.h>

/*
 * Spawn task and create pipes
 */
static int
iptv_pipe_start
  ( iptv_input_t *mi, iptv_mux_t *im, const char *raw, const url_t *url )
{
  char **argv = NULL, **envp = NULL;
  const char *replace[] = { "${service_name}", im->mm_iptv_svcname ?: "", NULL };
  int rd;
  pid_t pid;

  if (strncmp(raw, "pipe://", 7))
    goto err;

  if (spawn_parse_args(&argv, 64, raw + 7, replace))
    goto err;

  if (spawn_parse_args(&envp, 64, im->mm_iptv_env ?: "", NULL))
    goto err;

  if (spawn_and_give_stdout(argv[0], argv, envp, &rd, &pid, 1)) {
    tvherror(LS_IPTV, "Unable to start pipe '%s' (wrong executable?)", raw);
    goto err;
  }

  tvhtrace(LS_IPTV, "iptv pipe: spawned \"%s\" rd %d pid %d", argv[0], rd, pid);

  spawn_free_args(argv);
  spawn_free_args(envp);

  fcntl(rd, F_SETFD, fcntl(rd, F_GETFD) | FD_CLOEXEC);
  fcntl(rd, F_SETFL, fcntl(rd, F_GETFL) | O_NONBLOCK);

  im->mm_iptv_fd = rd;
  im->im_data = (void *)(intptr_t)pid;

  im->mm_iptv_respawn_last = mclk();

  if (url)
    iptv_input_mux_started(mi, im, 1);
  return 0;

err:
  if (argv)
    spawn_free_args(argv);
  if (envp)
    spawn_free_args(envp);
  return -1;
}

static void
iptv_pipe_stop
  ( iptv_input_t *mi, iptv_mux_t *im )
{
  pid_t pid = (intptr_t)im->im_data;
  spawn_kill(pid, tvh_kill_to_sig(im->mm_iptv_kill), im->mm_iptv_kill_timeout);
  iptv_input_close_fds(mi, im);
}

static ssize_t
iptv_pipe_read ( iptv_input_t *mi, iptv_mux_t *im )
{
  int r, rd = im->mm_iptv_fd;
  ssize_t res = 0;
  char buf[64*1024];
  pid_t pid;

  while (rd > 0 && res < sizeof(buf)) {
    r = read(rd, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EAGAIN)
        break;
      if (ERRNO_AGAIN(errno))
        continue;
    }
    if (r <= 0) {
      iptv_input_close_fds(mi, im);
      pid = (intptr_t)im->im_data;
      im->im_data = NULL;
      spawn_kill(pid, tvh_kill_to_sig(im->mm_iptv_kill), im->mm_iptv_kill_timeout);
      if (mclk() < im->mm_iptv_respawn_last + sec2mono(2)) {
        tvherror(LS_IPTV, "stdin pipe %d unexpectedly closed: %s",
                 rd, r < 0 ? strerror(errno) : "No data");
      } else {
        /* avoid deadlock here */
        tvh_mutex_unlock(&iptv_lock);
        tvh_mutex_lock(&global_lock);
        tvh_mutex_lock(&iptv_lock);
        if (im->mm_active) {
          if (iptv_pipe_start(mi, im, im->mm_iptv_url_raw, NULL)) {
            tvherror(LS_IPTV, "unable to respawn %s", im->mm_iptv_url_raw);
          } else {
            iptv_input_fd_started(mi, im);
            im->mm_iptv_respawn_last = mclk();
          }
        }
        tvh_mutex_unlock(&iptv_lock);
        tvh_mutex_unlock(&global_lock);
        tvh_mutex_lock(&iptv_lock);
      }
      break;
    }
    sbuf_append(&im->mm_iptv_buffer, buf, r);
    res += r;
  }

  return res;
}

/*
 * Initialise pipe handler
 */

void
iptv_pipe_init ( void )
{
  static iptv_handler_t ih[] = {
    {
      .scheme = "pipe",
      .buffer_limit = 5000,
      .start  = iptv_pipe_start,
      .stop   = iptv_pipe_stop,
      .read   = iptv_pipe_read,
      .pause  = iptv_input_pause_handler
    },
  };
  iptv_handler_register(ih, ARRAY_SIZE(ih));
}
