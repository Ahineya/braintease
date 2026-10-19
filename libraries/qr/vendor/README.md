# QR encoder source

Project Nayuki QR Code generator, release **v1.8.0**, MIT license (retained in
both source headers). Source:
https://github.com/nayuki/QR-Code-generator/tree/v1.8.0/c

Local C16 adaptations: `uint8_t`/`int8_t` use the standard minimum-width
`uint_least8_t`/`int_least8_t` carriers, and the Reed–Solomon multiplication
step explicitly masks to 255 instead of relying on an eight-bit cast. No
other algorithm changes. The public wrapper accepts at most 84 octets and
only invokes byte mode, medium correction, versions 1 through 5.
