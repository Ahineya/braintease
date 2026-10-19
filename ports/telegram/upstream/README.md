# Protocol reference

Telegram TDLib commit `a94b83d67ab4c8397c6245e541b31a4131ea2f79`, API layer 230.
`telegram_api.tl` and `PublicRsaKeySharedMain.cpp` are copied from the official
https://github.com/tdlib/td repository under its retained Boost 1.0 license.
The C++ reference is not compiled or run. `../generate.py` extracts public RSA
moduli/fingerprints and selected schema IDs for the independent guest C engine.

The known safe DH prime is from https://core.telegram.org/mtproto/auth_key.
Runtime validation pins that prime and checks Telegram's generator/public-value
requirements. Unknown groups and unknown RSA fingerprints fail closed.

`../vendor/tinf` is jibsen/tinf commit
`57ffa1f1d5e3dde19011b2127bd26d01689b694b` (zlib license). Its altered C16
sources use explicit 32-bit integer carriers instead of assuming a 32-bit int.
