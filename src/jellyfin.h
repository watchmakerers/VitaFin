#ifndef JELLYFIN_H
#define JELLYFIN_H

#include <stdint.h>

#define TICKS_PER_SEC 10000000LL

#define JF_MAX_SUBS 24

typedef struct {
  int index;          /* stream index on the server */
  int is_text;        /* text subs can be fetched as SRT; image subs must be burned in */
  int is_default, is_forced, is_external;
  char lang[8];
  char title[64];     /* e.g. "English - SUBRIP - Default" */
} jf_sub;

#define JF_MAX_AUDIO 16

typedef struct {
  int index;          /* stream index on the server */
  int channels;
  int is_default;
  char lang[8];
  char codec[16];
  char title[64];     /* e.g. "English - Dolby Digital - 5.1 - Default" */
} jf_audio;

typedef struct {
  char id[40];
  char name[192];
  char type[24];            /* Movie, Series, Season, Episode, Folder, CollectionFolder, MusicAlbum, Audio... */
  char collection_type[24]; /* movies, tvshows, music, homevideos... (views only) */
  char series_name[128];
  char series_id[40];
  char season_id[40];
  char album_artist[128];
  char *overview;
  int year;
  int index_number, parent_index_number;
  int is_folder;
  int child_count, unplayed_count;
  int played;
  int64_t runtime_ticks, position_ticks;
  char primary_tag[40];
  char thumb_tag[40];
  char backdrop_tag[40];
  char series_primary_tag[40];
  char parent_backdrop_id[40];
  char parent_backdrop_tag[40];
  char parent_thumb_id[40];
  char parent_thumb_tag[40];
  float primary_aspect;     /* width/height of primary image, 0 if unknown */
  char official_rating[16];
  float community_rating;
  /* media info (filled by jf_get_item) */
  char media_source_id[40];
  char container[32];
  char vcodec[16], acodec[16], vprofile[24];
  int width, height;
  int64_t bitrate;
  jf_sub subs[JF_MAX_SUBS];
  int nsubs;
  int default_sub;    /* server-suggested stream index, -1 = none */
  jf_audio auds[JF_MAX_AUDIO];
  int nauds;
  int default_audio;  /* server-suggested (follows the user's language preference), -1 = unknown */
} jf_item;

typedef struct {
  jf_item *items;
  int count;
  int total;
} jf_list;

void jf_apply_config(void);   /* (re)build auth header from g_cfg */
int jf_public_info(const char *server, char *name, int name_len, char *err, int err_len);
int jf_login(const char *server, const char *user, const char *pass, char *err, int err_len);
void jf_logout(void);

int jf_get_views(jf_list *out);
int jf_get_resume(jf_list *out, int limit);
int jf_get_nextup(jf_list *out, int limit);
int jf_get_latest(const char *parent_id, jf_list *out, int limit);
/* sort: "SortName", "DateCreated", "PremiereDate", "CommunityRating" */
int jf_get_children(const jf_item *parent, const char *sort, int descending, int start, int limit, jf_list *out);
int jf_get_seasons(const char *series_id, jf_list *out);
int jf_get_episodes(const char *series_id, const char *season_id, jf_list *out);
int jf_get_item(const char *id, jf_item *out);

void jf_list_free(jf_list *l);
int jf_list_append(jf_list *dst, jf_list *src); /* moves src items */
void jf_item_copy(jf_item *dst, const jf_item *src);
void jf_item_clear(jf_item *it);

/* Image helpers. type: "Primary", "Thumb", "Backdrop". */
void jf_image_url(char *buf, int len, const char *item_id, const char *type, const char *tag, int max_w, int max_h);
/* Picks the best image for a tile of the given shape; returns 0 if none. */
int jf_tile_image(const jf_item *it, int landscape, char *buf, int len, int w, int h);

/* Playback stream URL. audio = audio stream index (-1 = server default);
 * burn_sub = stream index of an image subtitle to burn in, or -1. */
int jf_stream_url(const jf_item *it, int64_t start_ticks, char *buf, int len, const char *play_session, int audio, int burn_sub);
/* URL of a text subtitle track converted to SRT (full timeline). */
void jf_subtitle_url(const jf_item *it, int stream_index, char *buf, int len);
int jf_is_playable(const jf_item *it);
void jf_report(const char *what, const jf_item *it, int64_t pos_ticks, int paused, const char *play_session, int transcode);
int jf_mark_played(const char *id, int played);
/* Stop the server-side transcode for a play session. */
void jf_kill_encoding(const char *play_session);

#endif
