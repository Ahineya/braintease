# Telegram guest prototype

The client supports QR login, two-step verification, saved sessions, a
searchable chat list with existing pinned chats first, paginated message
history, incoming text, and sending to the selected private chat/group/channel.
Archived chats are included after the main list. Search matches loaded titles
and usernames without ASCII case sensitivity. `*` marks pinned chats and `A`
marks archived chats; unread counts follow server updates.

The composer accepts up to 255 ASCII characters. Non-ASCII message/name glyphs
use ASCII fallbacks in the current terminal; media is represented by captions
or placeholders. Password input accepts UTF-8 and remains inside the C engine;
RFL sees only its masked length. Secret chats and media downloads are excluded.

The C implementation owns MTProto 2, TCP transport, TL parsing, RSA_PAD/DH,
SRP, login tokens and account state. `rfl/lib/telegram.rfl` supplies the thin
binding, and `rfl/apps/telegram/main.rfl` supplies the themed curses UI.
Everything runs inside Fuse-X. The host crypto device performs only stateless
cryptographic operations; no TDLib process, host session or host Telegram API
helper is involved. `live.py` only manages a private disk and terminal.

## Build and launch

Build a current native runtime and Fuse-X kernel with crypto operations 1–12,
then build the application:

```sh
make -C fuse-x telegram
```

Both standard launchers build and install `/bin/telegram`. The Ripple application
can also be built with `make -C ports/fuzix BUILD="$PWD/build/fuzix" telegram`.
Credentials are never installed by the standard image builder.
Create a private local configuration file (mode 0600):

```ini
api_id=YOUR_APPLICATION_ID
api_hash=YOUR_APPLICATION_HASH
```

The guest defaults are `$HOME/.telegram.conf` and `$HOME/.telegram.session`.
For root these are `/root/.telegram.conf` and `/root/.telegram.session`.
Normal image upgrades move the old `/etc/telegram.conf` and `/telegram.session`
without changing the saved login. Explicit command-line paths still work.
Alternatively run `telegram CONFIG_PATH SESSION_PATH`. An existing session
must be a private regular file. Invalid or corrupt records fail closed.

For a separate live test with a persistent private disk:

```sh
python3 ports/telegram/live.py --config /absolute/path/client.conf --background
python3 ports/telegram/live.py --attach
```

Use a terminal at least 80×30. Scan the rotating QR with Telegram's
Settings → Devices → Link Desktop Device scanner, approve, then enter the
2FA password in the attached terminal. Use arrows and Enter to open a chat,
`/` to search, Tab to switch between the list and composer, and Enter to send
from the composer. PgUp/PgDn scroll history; PgUp at the oldest loaded row
fetches another page. F5 reloads the latest history. F10 always exits; Esc
leaves search/composer first, then exits from the chat list. Run `--stop` to shut down the private guest cleanly.
`--status` and `--snapshot` inspect its terminal without attaching.

For a continuously refreshing, read-only browser preview, run
`python3 ports/telegram/preview.py` and open its printed local URL. It listens
only on loopback, uses an unguessable URL path and stops after 30 minutes.
It cannot accept passwords or send messages. Prefer this or the attached
terminal when scanning: static screenshots can expire in about 30 seconds.

If terminal attachment is unavailable, `--password-file /absolute/path`
forwards a private UTF-8 file to the guest **only while its 2FA prompt is
visible**. The file must be mode 0600 and contain one line of at most 512
UTF-8 octets. Do not put passwords or app credentials in command arguments.

The live launcher stores its disk, terminal capture, screenshots and any
recovery backups under ignored `build/telegram-private/` (directory 0700,
files 0600). A saved session contains an authorization key and must remain
private. Its SHA-256 checksum detects corruption; the disk is not encrypted.
After an interrupted launch, the helper validates/repairs the stopped root
filesystem, keeping a private recovery backup. Never open this disk in a
second guest at the same time.

To reuse an approved private login in a normal image, shut down both guests and
copy the application, configuration and session together:

```sh
python3 ports/telegram/install.py --source-image build/telegram-private/guest.img --target native --disk fuse-x/build/ROOTFS.IMG
python3 ports/telegram/install.py --source-image build/telegram-private/guest.img --target ripple --disk build/fuzix/fuzix.img
```

The installer locks both images, validates the session and matching app ID,
keeps private backups and writes mode 0600 login files. A different existing
login is preserved with an error. Normal launcher updates preserve these files;
`--fresh` creates a new image and requires importing the login again. Run one
copy of this shared authorization at a time.

## Protocol scope

Production bootstrap DC addresses and RSA anchors are pinned to official
Telegram sources. The accepted DH group is Telegram's known 2048-bit safe
prime, with generator and public-value checks. New unknown keys/groups fail
closed. Layer 230 schema references and upstream licenses are retained in
`upstream/`; `generate.py` regenerates `keys.c` and `schema.h`, and
`generate_tl.py` regenerates the bounded TL walker tables. Optional media and
metadata are traversed by the same schema, without handwritten byte guesses.

Packets and decompressed data are bounded to 28 KiB; containers, recursion,
replays and retries have explicit limits. The engine checks message MACs,
session IDs, time windows, padding, nonces and handshake proofs. It supports
QR DC migration, salted/time-corrected retries and session restoration.
Production builds have no configurable test endpoint or fixture keys.
Incoming packet salts need not equal the current outgoing salt; the MAC,
session, timestamp and replay checks authenticate those packets. Authenticated
salt corrections can refer to acknowledgements as well as the current RPC.

The chat model retains up to 2,048 peers and a moving window of 192 messages
for the open chat, fetching dialogs/history eight at a time. Dialog pages are paced to one per
second; server FLOOD_WAIT responses pause retries for the requested interval.
Large packets
fail closed at the explicit size limit. F5 returns to the latest messages
after paging far back. No history or update counters are persisted to disk;
reopening the app loads a fresh server snapshot using the saved authorization.

Incoming short/full updates, message edits/deletions, unread counts, pin order,
and archive moves are applied without mixing channel and common message IDs.
Sequence/pts gaps trigger global or channel difference requests; oversized
backlogs refresh the snapshot. Opening a chat does not mark it read. Read-only
channels and known sending restrictions disable the composer. Definitive send
errors leave the draft visible; transport retries preserve their random ID
and original recipient even if the UI switches chats.

Automatic reconnect, persistent outboxes, pin editing, multiple accounts and a
Unicode composer are not implemented. After a transport error, `r` reconnects
and recovers updates from the in-memory counters.

## Verification

Run the suites sequentially because they share generated fixture artifacts:

```sh
python3 ports/telegram/tests/test_crypto.py
python3 ports/telegram/tests/test_login.py
python3 ports/telegram/tests/test_model.py
python3 ports/telegram/tests/test_install.py
python3 ports/telegram/tests/test_guest.py --target native
python3 ports/telegram/tests/test_guest.py --target ripple
python3 ports/telegram/tests/test_guest.py --target both --chats
python3 ports/telegram/tests/test_ui.py
```

Host tests require OpenSSL and `pkg-config`; on macOS the helper selects the
Xcode compiler and its matching SDK. Local socket tests need permission to
listen on loopback. Their independent Python peer verifies RSA_PAD, DH, SRP,
encrypted packets, migration, Saved Messages and restart. Guest tests exercise
the real C16 engine, crypto device and RFL networking on both execution paths.
The UI suite independently decodes the actual 80×30 terminal QR and compares
it with the exact server-issued token. It also exercises hidden UTF-8
password entry, pinned/archived chats, title search, older history, pushed text,
selected-recipient sending, session restart and terminal restoration. Test credentials and peers are
synthetic; a live Telegram approval is a separate manual check.

Live check completed on 2026-10-18: production QR approval, 2FA, and a
user-entered message appearing in Saved Messages succeeded. A subsequent clean
guest shutdown/relaunch restored the saved session without another QR scan or
password. The refreshing preview resolved the rejected static QR attempts;
prefer it over screenshots to avoid token expiry during approval.

The chat extension was also checked against the real saved session: the pinned
chat list and Saved Messages history loaded, and a message sent from the normal
Telegram app appeared live without reopening or refreshing the guest chat.
This exposed and fixed an overly strict incoming-salt check; the synthetic
chat server now deliberately uses a different incoming salt to retain that
regression coverage.
