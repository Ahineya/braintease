# Experimental macro examples

Run `rfl run rfl/examples/macros/main.rfl --target native` or
`rfl run rfl/examples/macros/main.rfl --target ripple --jit` from the repository
root. Both compile `html_macro.rfl` as real RFL host code and print the same typed
tree and event count.

`html_macro.rfl` is a general procedural macro client. `tree.rfl` is an ordinary,
pure runtime library; it is deliberately not a browser framework contract.
The compiler has no HTML parser. Embedded children must produce `Node<M>`;
`text(value)` turns a String into a text node. Component properties are positional
arguments in written order, followed by a vector of children. `on_...` properties
carry messages of type M. Other attribute values are Strings.

See the [public macro contract](../../../docs/rfl/macros.md) for the syntax API,
hygiene, declared inputs, native bootstrap limits and editor behavior.
