/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RIPLIX_SSH_KEYS_H
#define RIPLIX_SSH_KEYS_H
#include <wolfssl/wolfcrypt/types.h>
#define SSH_PATH 512
#define SSH_KEY_BYTES 4096
#define SSH_BLOB_BYTES 1024
#define SSH_PUBLIC_LINE 2560
#define SSH_ECDSA 1
#define SSH_RSA 2
#define SSH_ED25519 3
struct ssh_key {
    byte der[SSH_KEY_BYTES],blob[SSH_BLOB_BYTES];
    unsigned der_length,blob_length,bits,type;
    char algorithm[40];
};
void ssh_wipe(void *,unsigned);
int ssh_secret(const char *,char *,unsigned);
int ssh_directory(char *,unsigned);
int ssh_path(char *,unsigned,const char *);
byte *ssh_read_file(const char *,unsigned *,int);
int ssh_write_file(const char *,const byte *,unsigned,unsigned,int);
int ssh_key_generate(struct ssh_key *,unsigned,unsigned);
int ssh_key_load(struct ssh_key *,const char *,const char *);
int ssh_key_save(struct ssh_key *,const char *,const char *,int);
int ssh_public_line(const byte *,unsigned,const char *,char *,unsigned);
int ssh_fingerprint(const byte *,unsigned,char *,unsigned);
word32 ssh_be32(const byte *);
void ssh_put32(byte *,word32);
#endif
