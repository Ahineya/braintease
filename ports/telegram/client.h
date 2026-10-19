/* SPDX-License-Identifier: MIT */
#ifndef FUSE_TELEGRAM_CLIENT_H
#define FUSE_TELEGRAM_CLIENT_H
#include "core.h"
#define TG_CONNECTING 1
#define TG_QR 2
#define TG_PASSWORD 3
#define TG_READY 4
#define TG_SENDING 5
#define TG_FAILED 6
typedef struct TgClient TgClient;
/* Config is a private regular file: api_id=decimal, api_hash=32 hex chars.
 * Session path is explicit and never a source file. Invalid existing files
 * fail closed. No account operations occur in the constructor. */
TgClient *tg_client_new(const char *,const char *);
void tg_client_free(TgClient *);
void tg_client_step(TgClient *);
unsigned tg_client_state(const TgClient *);
unsigned long tg_client_revision(const TgClient *);
const char *tg_client_status(const TgClient *);
const char *tg_client_qr(const TgClient *);
const char *tg_client_hint(const TgClient *);
unsigned tg_client_password_length(const TgClient *);
int tg_client_password_key(TgClient *,int);
int tg_client_send(TgClient *,const TgByte *,unsigned);
int tg_client_restart(TgClient *);
int tg_client_browse(TgClient *);
void tg_client_filter(TgClient *,const char *);
unsigned tg_client_chat_count(TgClient *);
unsigned tg_client_chat_key(TgClient *,unsigned);
const char *tg_client_chat_title(TgClient *,unsigned);
const char *tg_client_chat_preview(TgClient *,unsigned);
unsigned tg_client_chat_flags(TgClient *,unsigned);
unsigned tg_client_chat_unread(TgClient *,unsigned);
int tg_client_open(TgClient *,unsigned);
const char *tg_client_active_title(TgClient *);
unsigned tg_client_active_key(TgClient *);
int tg_client_can_send(TgClient *);
int tg_client_send_active(TgClient *,const TgByte *,unsigned);
int tg_client_history_more(TgClient *);
void tg_client_latest(TgClient *);
unsigned tg_client_history_lines(TgClient *,unsigned);
const char *tg_client_history_line(TgClient *,unsigned,unsigned);
/* Internal engine, shared by handshake and account state machine. */
#endif
