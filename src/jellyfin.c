#define _GNU_SOURCE
#include "jellyfin.h"
#include "config.h"
#include "http.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CLIENT_NAME "VitaFin"
#define CLIENT_VER  "1.0.0"
#define ITEM_FIELDS "Overview,PrimaryImageAspectRatio,ChildCount,ProductionYear,ParentId"

static void auth_line(char *buf, int len, const char *token) {
  snprintf(buf, len,
           "Authorization: MediaBrowser Client=\"" CLIENT_NAME "\", Device=\"PS Vita\", DeviceId=\"%s\", Version=\"" CLIENT_VER "\"%s%s%s",
           g_cfg.device_id, token && token[0] ? ", Token=\"" : "", token && token[0] ? token : "", token && token[0] ? "\"" : "");
}

void jf_apply_config(void) {
  char line[512];
  auth_line(line, sizeof(line), g_cfg.token);
  http_set_auth_header(line);
}

static void url_of(char *buf, int len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void url_of(char *buf, int len, const char *fmt, ...) {
  int n = snprintf(buf, len, "%s", g_cfg.server);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, len - n, fmt, ap);
  va_end(ap);
}

static void normalize_server(char *s, size_t cap) {
  /* trim whitespace and trailing slashes, add http:// if no scheme */
  while (*s == ' ') memmove(s, s + 1, strlen(s));
  size_t n = strlen(s);
  while (n && (s[n - 1] == '/' || s[n - 1] == ' ')) s[--n] = 0;
  if (n && !strstr(s, "://") && n + 7 < cap) {
    memmove(s + 7, s, n + 1);
    memcpy(s, "http://", 7);
  }
}

static jval *get_json(const char *url, int *status) {
  http_resp r;
  int rc = http_request("GET", url, NULL, &r);
  if (status) *status = (int)r.status;
  jval *v = (rc == 0 && r.status >= 200 && r.status < 300) ? json_parse(r.data) : NULL;
  http_resp_free(&r);
  return v;
}

int jf_public_info(const char *server, char *name, int name_len, char *err, int err_len) {
  char srv[256], url[512];
  snprintf(srv, sizeof(srv), "%s", server);
  normalize_server(srv, sizeof(srv));
  snprintf(url, sizeof(url), "%s/System/Info/Public", srv);
  http_resp r;
  if (http_request("GET", url, NULL, &r) != 0) {
    snprintf(err, err_len, "Could not connect to the server. (%s)", r.error);
    http_resp_free(&r);
    return -1;
  }
  jval *v = json_parse(r.data);
  http_resp_free(&r);
  if (!v || !json_get(v, "Id")) {
    snprintf(err, err_len, "This server does not appear to be a Jellyfin server.");
    json_free(v);
    return -1;
  }
  snprintf(name, name_len, "%s", json_str(v, "ServerName", "Jellyfin"));
  json_free(v);
  return 0;
}

int jf_login(const char *server, const char *user, const char *pass, char *err, int err_len) {
  char srv[256], url[512], line[512];
  snprintf(srv, sizeof(srv), "%s", server);
  normalize_server(srv, sizeof(srv));

  char name[128];
  if (jf_public_info(srv, name, sizeof(name), err, err_len) < 0) return -1;

  auth_line(line, sizeof(line), NULL);
  http_set_auth_header(line);

  char *eu = json_escape(user), *ep = json_escape(pass);
  size_t blen = strlen(eu) + strlen(ep) + 32;
  char *body = malloc(blen);
  snprintf(body, blen, "{\"Username\":\"%s\",\"Pw\":\"%s\"}", eu, ep);
  free(eu); free(ep);

  snprintf(url, sizeof(url), "%s/Users/AuthenticateByName", srv);
  http_resp r;
  int rc = http_request("POST", url, body, &r);
  free(body);
  if (rc != 0) {
    snprintf(err, err_len, "Could not connect to the server. (%s)", r.error);
    http_resp_free(&r);
    return -1;
  }
  if (r.status == 401 || r.status == 403) {
    snprintf(err, err_len, "The user name or password is incorrect.");
    http_resp_free(&r);
    return -1;
  }
  jval *v = json_parse(r.data);
  http_resp_free(&r);
  const char *tok = json_str(v, "AccessToken", NULL);
  jval *u = json_get(v, "User");
  const char *uid = json_str(u, "Id", NULL);
  if (!tok || !uid) {
    snprintf(err, err_len, "Sign-in failed (HTTP %ld).", r.status);
    json_free(v);
    return -1;
  }
  snprintf(g_cfg.server, sizeof(g_cfg.server), "%s", srv);
  snprintf(g_cfg.username, sizeof(g_cfg.username), "%s", user);
  snprintf(g_cfg.token, sizeof(g_cfg.token), "%s", tok);
  snprintf(g_cfg.user_id, sizeof(g_cfg.user_id), "%s", uid);
  snprintf(g_cfg.server_name, sizeof(g_cfg.server_name), "%s", name);
  json_free(v);
  config_save();
  jf_apply_config();
  return 0;
}

void jf_logout(void) {
  char url[512];
  url_of(url, sizeof(url), "/Sessions/Logout");
  http_resp r;
  http_request("POST", url, "", &r);
  http_resp_free(&r);
  g_cfg.token[0] = 0;
  g_cfg.user_id[0] = 0;
  config_save();
  jf_apply_config();
}

/* ---------------- item parsing ---------------- */

static void cp(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

static void parse_item(const jval *o, jf_item *it) {
  memset(it, 0, sizeof(*it));
  it->default_sub = -1;
  it->default_audio = -1;
  cp(it->id, sizeof(it->id), json_str(o, "Id", ""));
  cp(it->name, sizeof(it->name), json_str(o, "Name", ""));
  cp(it->type, sizeof(it->type), json_str(o, "Type", ""));
  cp(it->collection_type, sizeof(it->collection_type), json_str(o, "CollectionType", ""));
  cp(it->series_name, sizeof(it->series_name), json_str(o, "SeriesName", ""));
  cp(it->series_id, sizeof(it->series_id), json_str(o, "SeriesId", ""));
  cp(it->season_id, sizeof(it->season_id), json_str(o, "SeasonId", ""));
  cp(it->album_artist, sizeof(it->album_artist), json_str(o, "AlbumArtist", ""));
  cp(it->official_rating, sizeof(it->official_rating), json_str(o, "OfficialRating", ""));
  const char *ov = json_str(o, "Overview", NULL);
  if (ov) it->overview = strdup(ov);
  it->year = (int)json_num(o, "ProductionYear", 0);
  it->index_number = (int)json_num(o, "IndexNumber", -1);
  it->parent_index_number = (int)json_num(o, "ParentIndexNumber", -1);
  it->is_folder = json_bool(o, "IsFolder", 0);
  it->child_count = (int)json_num(o, "ChildCount", 0);
  it->runtime_ticks = (int64_t)json_num(o, "RunTimeTicks", 0);
  it->primary_aspect = (float)json_num(o, "PrimaryImageAspectRatio", 0);
  it->community_rating = (float)json_num(o, "CommunityRating", 0);

  jval *ud = json_get(o, "UserData");
  it->played = json_bool(ud, "Played", 0);
  it->position_ticks = (int64_t)json_num(ud, "PlaybackPositionTicks", 0);
  it->unplayed_count = (int)json_num(ud, "UnplayedItemCount", 0);

  jval *tags = json_get(o, "ImageTags");
  cp(it->primary_tag, sizeof(it->primary_tag), json_str(tags, "Primary", ""));
  cp(it->thumb_tag, sizeof(it->thumb_tag), json_str(tags, "Thumb", ""));
  jval *bd = json_at(json_get(o, "BackdropImageTags"), 0);
  if (bd && bd->type == J_STR) cp(it->backdrop_tag, sizeof(it->backdrop_tag), bd->str);
  cp(it->series_primary_tag, sizeof(it->series_primary_tag), json_str(o, "SeriesPrimaryImageTag", ""));
  cp(it->parent_backdrop_id, sizeof(it->parent_backdrop_id), json_str(o, "ParentBackdropItemId", ""));
  jval *pbd = json_at(json_get(o, "ParentBackdropImageTags"), 0);
  if (pbd && pbd->type == J_STR) cp(it->parent_backdrop_tag, sizeof(it->parent_backdrop_tag), pbd->str);
  cp(it->parent_thumb_id, sizeof(it->parent_thumb_id), json_str(o, "ParentThumbItemId", ""));
  cp(it->parent_thumb_tag, sizeof(it->parent_thumb_tag), json_str(o, "ParentThumbImageTag", ""));

  jval *ms = json_at(json_get(o, "MediaSources"), 0);
  if (ms) {
    cp(it->media_source_id, sizeof(it->media_source_id), json_str(ms, "Id", ""));
    cp(it->container, sizeof(it->container), json_str(ms, "Container", ""));
    it->bitrate = (int64_t)json_num(ms, "Bitrate", 0);
    jval *streams = json_get(ms, "MediaStreams");
    int def_audio = (int)json_num(ms, "DefaultAudioStreamIndex", -1);
    it->default_audio = def_audio;
    it->default_sub = (int)json_num(ms, "DefaultSubtitleStreamIndex", -1);
    for (jval *s = streams ? streams->child : NULL; s; s = s->next) {
      const char *t = json_str(s, "Type", "");
      if (!strcmp(t, "Video") && !it->vcodec[0]) {
        cp(it->vcodec, sizeof(it->vcodec), json_str(s, "Codec", ""));
        cp(it->vprofile, sizeof(it->vprofile), json_str(s, "Profile", ""));
        it->width = (int)json_num(s, "Width", 0);
        it->height = (int)json_num(s, "Height", 0);
      } else if (!strcmp(t, "Audio")) {
        int idx = (int)json_num(s, "Index", -2);
        if (!it->acodec[0] || idx == def_audio) cp(it->acodec, sizeof(it->acodec), json_str(s, "Codec", ""));
        if (it->nauds < JF_MAX_AUDIO) {
          jf_audio *au = &it->auds[it->nauds++];
          au->index = idx;
          au->channels = (int)json_num(s, "Channels", 0);
          au->is_default = json_bool(s, "IsDefault", 0);
          cp(au->lang, sizeof(au->lang), json_str(s, "Language", ""));
          cp(au->codec, sizeof(au->codec), json_str(s, "Codec", ""));
          const char *dt = json_str(s, "DisplayTitle", NULL);
          if (!dt) dt = json_str(s, "Title", NULL);
          if (!dt) dt = au->lang[0] ? au->lang : "Audio";
          cp(au->title, sizeof(au->title), dt);
        }
      } else if (!strcmp(t, "Subtitle") && it->nsubs < JF_MAX_SUBS) {
        jf_sub *sb = &it->subs[it->nsubs++];
        sb->index = (int)json_num(s, "Index", -1);
        sb->is_text = json_bool(s, "IsTextSubtitleStream", 0);
        sb->is_default = json_bool(s, "IsDefault", 0);
        sb->is_forced = json_bool(s, "IsForced", 0);
        sb->is_external = json_bool(s, "IsExternal", 0);
        cp(sb->lang, sizeof(sb->lang), json_str(s, "Language", ""));
        const char *dt = json_str(s, "DisplayTitle", NULL);
        if (!dt) dt = json_str(s, "Title", NULL);
        if (!dt) dt = sb->lang[0] ? sb->lang : "Unknown";
        cp(sb->title, sizeof(sb->title), dt);
      }
    }
  }
}

static int parse_items(jval *v, jf_list *out) {
  memset(out, 0, sizeof(*out));
  if (!v) return -1;
  jval *arr = v->type == J_ARR ? v : json_get(v, "Items");
  out->total = (int)json_num(v, "TotalRecordCount", arr ? arr->count : 0);
  if (!arr || arr->type != J_ARR) { json_free(v); return 0; }
  out->items = calloc(arr->count ? arr->count : 1, sizeof(jf_item));
  for (jval *c = arr->child; c; c = c->next) parse_item(c, &out->items[out->count++]);
  if (v->type == J_ARR) out->total = out->count;
  json_free(v);
  return 0;
}

static int fetch_list(const char *url, jf_list *out) {
  int status = 0;
  jval *v = get_json(url, &status);
  if (!v) { memset(out, 0, sizeof(*out)); return status == 401 ? -401 : -1; }
  return parse_items(v, out);
}

int jf_get_views(jf_list *out) {
  char url[512];
  url_of(url, sizeof(url), "/Users/%s/Views", g_cfg.user_id);
  return fetch_list(url, out);
}

int jf_get_resume(jf_list *out, int limit) {
  char url[640];
  url_of(url, sizeof(url), "/Users/%s/Items/Resume?Limit=%d&MediaTypes=Video&Fields=" ITEM_FIELDS "&EnableImageTypes=Primary,Thumb,Backdrop",
         g_cfg.user_id, limit);
  return fetch_list(url, out);
}

int jf_get_nextup(jf_list *out, int limit) {
  char url[640];
  url_of(url, sizeof(url), "/Shows/NextUp?UserId=%s&Limit=%d&Fields=" ITEM_FIELDS, g_cfg.user_id, limit);
  return fetch_list(url, out);
}

int jf_get_latest(const char *parent_id, jf_list *out, int limit) {
  char url[640];
  url_of(url, sizeof(url), "/Users/%s/Items/Latest?ParentId=%s&Limit=%d&Fields=" ITEM_FIELDS, g_cfg.user_id, parent_id, limit);
  return fetch_list(url, out);
}

int jf_get_children(const jf_item *parent, const char *sort, int descending, int start, int limit, jf_list *out) {
  char url[1024], filt[160] = "";
  const char *ct = parent->collection_type;
  /* Show a flat poster grid for movie / show libraries, like a native app would. */
  if (!strcmp(ct, "movies")) snprintf(filt, sizeof(filt), "&Recursive=true&IncludeItemTypes=Movie");
  else if (!strcmp(ct, "tvshows")) snprintf(filt, sizeof(filt), "&Recursive=true&IncludeItemTypes=Series");
  else if (!strcmp(ct, "music")) snprintf(filt, sizeof(filt), "&Recursive=true&IncludeItemTypes=MusicAlbum");
  else if (!strcmp(ct, "boxsets")) snprintf(filt, sizeof(filt), "&IncludeItemTypes=BoxSet");
  else if (!strcmp(parent->type, "MusicAlbum")) snprintf(filt, sizeof(filt), "&SortBy=ParentIndexNumber,IndexNumber");
  const char *order = descending ? "Descending" : "Ascending";
  if (!strcmp(parent->type, "MusicAlbum")) {
    url_of(url, sizeof(url), "/Users/%s/Items?ParentId=%s&SortBy=ParentIndexNumber,IndexNumber,SortName&StartIndex=%d&Limit=%d&Fields=" ITEM_FIELDS,
           g_cfg.user_id, parent->id, start, limit);
  } else {
    url_of(url, sizeof(url), "/Users/%s/Items?ParentId=%s&SortBy=%s&SortOrder=%s&StartIndex=%d&Limit=%d%s&Fields=" ITEM_FIELDS
           "&EnableImageTypes=Primary,Thumb,Backdrop&EnableTotalRecordCount=true",
           g_cfg.user_id, parent->id, sort, order, start, limit, filt);
  }
  return fetch_list(url, out);
}

int jf_get_seasons(const char *series_id, jf_list *out) {
  char url[640];
  url_of(url, sizeof(url), "/Shows/%s/Seasons?UserId=%s&Fields=" ITEM_FIELDS, series_id, g_cfg.user_id);
  return fetch_list(url, out);
}

int jf_get_episodes(const char *series_id, const char *season_id, jf_list *out) {
  char url[640];
  url_of(url, sizeof(url), "/Shows/%s/Episodes?UserId=%s&SeasonId=%s&Fields=" ITEM_FIELDS, series_id, g_cfg.user_id, season_id);
  return fetch_list(url, out);
}

int jf_get_item(const char *id, jf_item *out) {
  char url[512];
  url_of(url, sizeof(url), "/Users/%s/Items/%s", g_cfg.user_id, id);
  jval *v = get_json(url, NULL);
  if (!v) return -1;
  parse_item(v, out);
  json_free(v);
  return 0;
}

void jf_item_clear(jf_item *it) {
  free(it->overview);
  it->overview = NULL;
}

void jf_item_copy(jf_item *dst, const jf_item *src) {
  *dst = *src;
  dst->overview = src->overview ? strdup(src->overview) : NULL;
}

void jf_list_free(jf_list *l) {
  for (int i = 0; i < l->count; i++) jf_item_clear(&l->items[i]);
  free(l->items);
  memset(l, 0, sizeof(*l));
}

int jf_list_append(jf_list *dst, jf_list *src) {
  jf_item *n = realloc(dst->items, (dst->count + src->count + 1) * sizeof(jf_item));
  if (!n) return -1;
  dst->items = n;
  memcpy(dst->items + dst->count, src->items, src->count * sizeof(jf_item));
  dst->count += src->count;
  if (src->total) dst->total = src->total;
  free(src->items);
  memset(src, 0, sizeof(*src));
  return 0;
}

/* ---------------- images ---------------- */

void jf_image_url(char *buf, int len, const char *item_id, const char *type, const char *tag, int max_w, int max_h) {
  url_of(buf, len, "/Items/%s/Images/%s?fillWidth=%d&fillHeight=%d&quality=85&format=Jpg%s%s",
         item_id, type, max_w, max_h, tag && tag[0] ? "&tag=" : "", tag ? tag : "");
}

int jf_tile_image(const jf_item *it, int landscape, char *buf, int len, int w, int h) {
  if (landscape) {
    if (!strcmp(it->type, "Episode") && it->primary_tag[0]) { jf_image_url(buf, len, it->id, "Primary", it->primary_tag, w, h); return 1; }
    if (it->thumb_tag[0]) { jf_image_url(buf, len, it->id, "Thumb", it->thumb_tag, w, h); return 1; }
    if (it->parent_thumb_tag[0]) { jf_image_url(buf, len, it->parent_thumb_id, "Thumb", it->parent_thumb_tag, w, h); return 1; }
    if (it->backdrop_tag[0]) { jf_image_url(buf, len, it->id, "Backdrop", it->backdrop_tag, w, h); return 1; }
    if (it->parent_backdrop_tag[0]) { jf_image_url(buf, len, it->parent_backdrop_id, "Backdrop", it->parent_backdrop_tag, w, h); return 1; }
  }
  if (it->primary_tag[0]) { jf_image_url(buf, len, it->id, "Primary", it->primary_tag, w, h); return 1; }
  if (it->series_primary_tag[0] && it->series_id[0]) { jf_image_url(buf, len, it->series_id, "Primary", it->series_primary_tag, w, h); return 1; }
  return 0;
}

/* ---------------- playback ---------------- */

int jf_is_playable(const jf_item *it) {
  return !strcmp(it->type, "Movie") || !strcmp(it->type, "Episode") || !strcmp(it->type, "Video") ||
         !strcmp(it->type, "MusicVideo") || !strcmp(it->type, "Audio") || !strcmp(it->type, "Trailer");
}

/* Everything is streamed as MPEG-TS (H.264 + AAC) for the hardware decoders.
 * Jellyfin copies the H.264 stream untouched when it already fits these limits
 * and only re-encodes when it has to. Music comes as raw ADTS AAC. */
int jf_stream_url(const jf_item *it, int64_t start_ticks, char *buf, int len, const char *ps, int audio, int burn_sub) {
  if (!strcmp(it->type, "Audio")) {
    url_of(buf, len, "/Audio/%s/stream.aac?static=false&MediaSourceId=%s&AudioCodec=aac&Container=aac&TranscodingContainer=aac"
           "&AudioBitrate=192000&MaxAudioChannels=2&AudioChannels=2&StartTimeTicks=%lld&DeviceId=%s&PlaySessionId=%s&api_key=%s",
           it->id, it->media_source_id, (long long)start_ticks, g_cfg.device_id, ps, g_cfg.token);
    return 0;
  }
  url_of(buf, len,
         "/Videos/%s/stream.ts?static=false&MediaSourceId=%s&Container=ts&VideoCodec=h264&AudioCodec=aac"
         "&Profile=high&Level=41&MaxWidth=1280&MaxHeight=720&MaxVideoBitDepth=8&VideoBitrate=%d"
         "&AudioBitrate=160000&MaxAudioChannels=2&AudioChannels=2&RequireAvc=true"
         "&AllowVideoStreamCopy=true&AllowAudioStreamCopy=true&BreakOnNonKeyFrames=false"
         "&StartTimeTicks=%lld&DeviceId=%s&PlaySessionId=%s&api_key=%s",
         it->id, it->media_source_id, g_cfg.max_bitrate * 1000, (long long)start_ticks, g_cfg.device_id, ps, g_cfg.token);
  if (audio >= 0) {
    size_t n = strlen(buf);
    snprintf(buf + n, len - n, "&AudioStreamIndex=%d", audio);
  }
  if (burn_sub >= 0) {
    /* picture subtitles (PGS / VobSub) are rendered into the video by the server */
    size_t n = strlen(buf);
    snprintf(buf + n, len - n, "&SubtitleStreamIndex=%d&SubtitleMethod=Encode", burn_sub);
  }
  return 0;
}

void jf_subtitle_url(const jf_item *it, int index, char *buf, int len) {
  url_of(buf, len, "/Videos/%s/%s/Subtitles/%d/0/Stream.srt?api_key=%s", it->id, it->media_source_id, index, g_cfg.token);
}

void jf_report(const char *what, const jf_item *it, int64_t pos, int paused, const char *ps, int transcode) {
  char url[512], body[512];
  if (!strcmp(what, "start")) url_of(url, sizeof(url), "/Sessions/Playing");
  else if (!strcmp(what, "progress")) url_of(url, sizeof(url), "/Sessions/Playing/Progress");
  else url_of(url, sizeof(url), "/Sessions/Playing/Stopped");
  snprintf(body, sizeof(body),
           "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PositionTicks\":%lld,\"IsPaused\":%s,"
           "\"PlaySessionId\":\"%s\",\"PlayMethod\":\"%s\",\"CanSeek\":true}",
           it->id, it->media_source_id, (long long)pos, paused ? "true" : "false", ps,
           transcode ? "Transcode" : "DirectPlay");
  http_resp r;
  http_request("POST", url, body, &r);
  http_resp_free(&r);
}

void jf_kill_encoding(const char *ps) {
  char url[512];
  url_of(url, sizeof(url), "/Videos/ActiveEncodings?deviceId=%s&playSessionId=%s", g_cfg.device_id, ps);
  http_resp r;
  http_request("DELETE", url, NULL, &r);
  http_resp_free(&r);
}

int jf_mark_played(const char *id, int played) {
  char url[512];
  url_of(url, sizeof(url), "/Users/%s/PlayedItems/%s", g_cfg.user_id, id);
  http_resp r;
  int rc = http_request(played ? "POST" : "DELETE", url, played ? "" : NULL, &r);
  http_resp_free(&r);
  return rc;
}
