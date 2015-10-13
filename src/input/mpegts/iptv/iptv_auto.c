/*
 *  IPTV - automatic network based on playlists
 *
 *  Copyright (C) 2015 Jaroslav Kysela
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
#include "http.h"
#include "iptv_private.h"

#include <fcntl.h>
#include <sys/stat.h>

/*
 *
 */
static void
iptv_auto_network_process_m3u_item(iptv_network_t *in,
                                   const char *url, const char *name,
                                   int *total, int *count)
{
  htsmsg_t *conf;
  mpegts_mux_t *mm;
  iptv_mux_t *im;
  int change;

  if (url == NULL ||
      (strncmp(url, "file://", 7) &&
       strncmp(url, "http://", 7) &&
       strncmp(url, "https://", 8)))
    return;

  LIST_FOREACH(mm, &in->mn_muxes, mm_network_link) {
    im = (iptv_mux_t *)mm;
    if (strcmp(im->mm_iptv_url ?: "", url) == 0) {
      im->im_delete_flag = 0;
      if (strcmp(im->mm_iptv_svcname ?: "", name ?: "")) {
        free(im->mm_iptv_svcname);
        im->mm_iptv_svcname = name ? strdup(name) : NULL;
        change = 1;
      }
      if (change)
        idnode_notify_changed(&im->mm_id);
      (*total)++;
      return;
    }
  }


  conf = htsmsg_create_map();
  htsmsg_add_str(conf, "iptv_url", url);
  if (name)
    htsmsg_add_str(conf, "iptv_sname", name);
  im = iptv_mux_create0(in, NULL, conf);
  htsmsg_destroy(conf);

  if (im) {
    im->mm_config_save((mpegts_mux_t *)im);
    (*total)++;
    (*count)++;
  }
}

/*
 *
 */
static int
iptv_auto_network_process_m3u(iptv_network_t *in, char *data)
{
  char *url, *name = NULL;
  int total = 0, count = 0;

  while (*data && *data != '\n') data++;
  if (*data) data++;
  while (*data) {
    if (strncmp(data, "#EXTINF:", 8) == 0) {
      name = NULL;
      data += 8;
      while (*data && *data != ',') data++;
      if (*data == ',') {
        data++;
        while (*data && *data <= ' ') data++;
        if (*data)
          name = data;
      }
      while (*data && *data != '\n') data++;
      if (*data) { *data = '\0'; data++; }
      continue;
    }
    while (*data && *data <= ' ') data++;
    url = data;
    while (*data && *data != '\n') data++;
    if (*data) { *data = '\0'; data++; }
    if (*url)
      iptv_auto_network_process_m3u_item(in, url, name, &total, &count);
  }

  if (total == 0)
    return -1;
  tvhinfo("iptv", "m3u parse: %d new mux(es) in network '%s' (total %d)",
          count, in->mn_network_name, total);
  return 0;
}

/*
 *
 */
static int
iptv_auto_network_process(iptv_network_t *in, char *data, size_t len)
{
  mpegts_mux_t *mm;
  int r = -1, count;

  /* note that we know that data are terminated with '\0' */

  if (data == NULL || len == 0)
    return -1;

  LIST_FOREACH(mm, &in->mn_muxes, mm_network_link)
    ((iptv_mux_t *)mm)->im_delete_flag = 1;

  while (*data && *data <= ' ') data++;

  if (!strncmp(data, "#EXTM3U", 7))
    r = iptv_auto_network_process_m3u(in, data);

  if (r == 0) {
    count = 0;
    LIST_FOREACH(mm, &in->mn_muxes, mm_network_link)
      if (((iptv_mux_t *)mm)->im_delete_flag) {
        mm->mm_delete(mm, 1);
        count++;
      }
    tvhinfo("iptv", "removed %d mux(es) from network '%s'", count, in->mn_network_name);
  } else {
    LIST_FOREACH(mm, &in->mn_muxes, mm_network_link)
      ((iptv_mux_t *)mm)->im_delete_flag = 0;
    tvherror("iptv", "unknown playlist format for network '%s'", in->mn_network_name);
  }

  return -1;
}

/*
 *
 */
static int
iptv_auto_network_file(iptv_network_t *in, const char *filename)
{
  int fd;
  struct stat st;
  char *data;
  size_t r;
  off_t off;

  fd = tvh_open(filename, O_RDONLY, 0);
  if (fd < 0) {
    tvherror("iptv", "unable to open file '%s' (network '%s'): %s",
             filename, in->mn_network_name, strerror(errno));
    return -1;
  }
  if (fstat(fd, &st) || st.st_size == 0) {
    tvherror("iptv", "unable to stat file '%s' (network '%s'): %s",
             filename, in->mn_network_name, strerror(errno));
    close(fd);
    return -1;
  }
  data = malloc(st.st_size+1);
  off = 0;
  do {
    r = read(fd, data + off, st.st_size - off);
    if (r < 0) {
      if (ERRNO_AGAIN(errno))
        continue;
      break;
    }
    off += r;
  } while (off != st.st_size);
  close(fd);

  if (off == st.st_size) {
    data[off] = '\0';
    return iptv_auto_network_process(in, data, off);
  }
  return -1;
}

/*
 *
 */
static void
iptv_auto_network_fetch_done(void *aux)
{
  http_client_t *hc = aux;
  iptv_network_t *in = hc->hc_aux;
  if (in->in_http_client) {
    in->in_http_client = NULL;
    http_client_close((http_client_t *)aux);
  }
}

/*
 *
 */
static int
iptv_auto_network_fetch_complete(http_client_t *hc)
{
  iptv_network_t *in = hc->hc_aux;

  switch (hc->hc_code) {
  case HTTP_STATUS_MOVED:
  case HTTP_STATUS_FOUND:
  case HTTP_STATUS_SEE_OTHER:
  case HTTP_STATUS_NOT_MODIFIED:
    return 0;
  }

  pthread_mutex_lock(&global_lock);

  if (hc->hc_code == HTTP_STATUS_OK && hc->hc_result == 0 && hc->hc_data_size > 0)
    iptv_auto_network_process(in, hc->hc_data, hc->hc_data_size);
  else
    tvherror("iptv", "unable to fetch data from url for network '%s' [%d-%d/%zd]",
             in->mn_network_name, hc->hc_code, hc->hc_result, hc->hc_data_size);

  /* note: http_client_close must be called outside http_client callbacks */
  gtimer_arm(&in->in_fetch_timer, iptv_auto_network_fetch_done, hc, 0);

  pthread_mutex_unlock(&global_lock);

  return 0;
}

/*
 *
 */
static void
iptv_auto_network_fetch(void *aux)
{
  iptv_network_t *in = aux;
  http_client_t *hc;
  url_t u;

  if (strncmp(in->in_url, "file://", 7) == 0) {
    iptv_auto_network_file(in, in->in_url + 7);
    goto arm;
  }

  gtimer_disarm(&in->in_auto_timer);
  if (in->in_http_client) {
    http_client_close(in->in_http_client);
    in->in_http_client = NULL;
  }

  memset(&u, 0, sizeof(u));
  if (urlparse(in->in_url, &u) < 0) {
    tvherror("iptv", "wrong url for network '%s'", in->mn_network_name);
    goto arm;
  }
  hc = http_client_connect(in, HTTP_VERSION_1_1, u.scheme, u.host, u.port, NULL);
  if (hc == NULL) {
    tvherror("iptv", "unable to open http client for network '%s'", in->mn_network_name);
    goto arm;
  }
  hc->hc_handle_location = 1;
  hc->hc_data_limit = 1024*1024;
  hc->hc_data_complete = iptv_auto_network_fetch_complete;
  http_client_register(hc);
  http_client_ssl_peer_verify(hc, in->in_ssl_peer_verify);
  if (http_client_simple(hc, &u) < 0) {
    http_client_close(hc);
    tvherror("iptv", "unable to send http command for network '%s'", in->mn_network_name);
    goto arm;
  }

  in->in_http_client = hc;

arm:
  gtimer_arm(&in->in_auto_timer, iptv_auto_network_fetch, in,
             MAX(1, in->in_refetch_period) * 60);
}

/*
 *
 */
void
iptv_auto_network_init( iptv_network_t *in )
{
  gtimer_arm(&in->in_auto_timer, iptv_auto_network_fetch, in, 0);
}

/*
 *
 */
void
iptv_auto_network_done( iptv_network_t *in )
{
  gtimer_disarm(&in->in_auto_timer);
  gtimer_disarm(&in->in_fetch_timer);
  if (in->in_http_client) {
    http_client_close(in->in_http_client);
    in->in_http_client = NULL;
  }
}
